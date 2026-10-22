#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "chase_lev_deque.hpp"
#include "task.hpp"
#include "worker_thread.hpp"

namespace wss {

struct SchedulerStats {
  std::uint64_t executed = 0;      // tasks run
  std::uint64_t steals = 0;        // successful steals
  std::uint64_t steal_misses = 0;  // steal attempts that came back empty
  std::uint64_t sleeps = 0;        // times a worker parked on the condvar
};

// Work-stealing scheduler: one Chase-Lev deque per worker.
//
// - The thread that constructs the Scheduler becomes worker 0. It runs tasks
//   only while it is inside wait(); workers 1..N-1 are dedicated threads.
// - spawn() from a worker pushes onto that worker's own deque (no locks).
//   spawn() from any other thread goes through a mutex-guarded injection queue.
// - An idle worker pops its own deque, then checks the injection queue, then
//   steals from victims starting at a random index, then spins, then sleeps.
// - wait() keeps the calling worker busy running tasks until the group is done
//   ("help while waiting"), so fork-join recursion never blocks a thread.
//
// Precondition for destruction: every TaskGroup has been waited on.
class Scheduler {
 public:
  explicit Scheduler(unsigned num_workers = std::thread::hardware_concurrency())
      : num_workers_(num_workers == 0 ? 1 : num_workers) {
    for (unsigned i = 0; i < num_workers_; ++i) {
      workers_.push_back(std::make_unique<Worker>(i));
    }
    bind_current_thread(0);
    for (unsigned i = 1; i < num_workers_; ++i) {
      threads_.emplace_back([this, i] { worker_loop(i); });
    }
  }

  ~Scheduler() {
    {
      std::lock_guard<std::mutex> lock(sleep_mu_);
      stop_.store(true, std::memory_order_relaxed);
    }
    sleep_cv_.notify_all();
    for (auto& t : threads_) t.join();
    if (tls_scheduler == this) tls_scheduler = nullptr;
  }

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  template <typename F>
  void spawn(TaskGroup& group, F&& fn) {
    Task* task = Task::make(group, std::forward<F>(fn));
    if (tls_scheduler == this) {
      workers_[tls_index]->deque.push(task);
    } else {
      std::lock_guard<std::mutex> lock(inject_mu_);
      inject_.push_back(task);
      inject_size_.fetch_add(1, std::memory_order_relaxed);
    }
    wake_one_if_sleeping();
  }

  void wait(TaskGroup& group) {
    if (tls_scheduler != this) {
      // Outside thread: it has no deque, but it still helps (injection queue,
      // then steals). Otherwise a 1-worker scheduler whose worker 0 is blocked
      // elsewhere would never run tasks injected by this thread.
      std::uint64_t rng = reinterpret_cast<std::uintptr_t>(&group) | 1;
      while (!group.done()) {
        Task* task = pop_injected();
        if (!task) task = steal_any(rng, num_workers_);
        if (task) {
          Task::run(task);
        } else {
          std::this_thread::yield();
        }
      }
      return;
    }
    Worker& self = *workers_[tls_index];
    while (!group.done()) {
      if (Task* task = find_task(self)) {
        execute(self, task);
      } else {
        cpu_relax();
      }
    }
  }

  unsigned num_workers() const { return num_workers_; }

  // Racy per-worker counters summed; read them after all work is done.
  SchedulerStats stats() const {
    SchedulerStats s;
    for (const auto& w : workers_) {
      s.executed += w->executed.load(std::memory_order_relaxed);
      s.steals += w->steals.load(std::memory_order_relaxed);
      s.steal_misses += w->steal_misses.load(std::memory_order_relaxed);
      s.sleeps += w->sleeps.load(std::memory_order_relaxed);
    }
    return s;
  }

 private:
  static constexpr int kSpinRounds = 64;

  static std::uint64_t xorshift(std::uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }

  struct alignas(kCacheLine) Worker {
    explicit Worker(unsigned i) : index(i), rng(0x9E3779B97F4A7C15ull * (i + 1)) {}

    ChaseLevDeque<Task*> deque;
    unsigned index;
    std::uint64_t rng;  // xorshift state, touched only by this worker
    // Single-writer counters: only the owning worker increments them.
    std::atomic<std::uint64_t> executed{0}, steals{0}, steal_misses{0}, sleeps{0};
  };

  static void bump(std::atomic<std::uint64_t>& c) {
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

  void bind_current_thread(unsigned index) {
    tls_scheduler = this;
    tls_index = index;
  }

  void worker_loop(unsigned index) {
    bind_current_thread(index);
    Worker& self = *workers_[index];
    while (!stop_.load(std::memory_order_relaxed)) {
      if (Task* task = find_task(self)) {
        execute(self, task);
      } else {
        idle(self);
      }
    }
  }

  void execute(Worker& self, Task* task) {
    Task::run(task);
    bump(self.executed);
  }

  Task* find_task(Worker& self) {
    if (auto t = self.deque.pop()) return *t;
    if (Task* t = pop_injected()) return t;
    return try_steal(self);
  }

  Task* pop_injected() {
    if (inject_size_.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard<std::mutex> lock(inject_mu_);
    if (inject_.empty()) return nullptr;
    Task* t = inject_.front();
    inject_.pop_front();
    inject_size_.fetch_sub(1, std::memory_order_relaxed);
    return t;
  }

  // One pass over every other worker, starting at a random victim so thieves
  // spread out instead of all hammering worker 0.
  Task* try_steal(Worker& self) {
    if (num_workers_ == 1) return nullptr;
    if (Task* t = steal_any(self.rng, self.index)) {
      bump(self.steals);
      return t;
    }
    bump(self.steal_misses);
    return nullptr;
  }

  // Tries every worker except `skip` once (pass num_workers_ to skip none).
  Task* steal_any(std::uint64_t& rng, unsigned skip) {
    const unsigned start = static_cast<unsigned>(xorshift(rng) % num_workers_);
    for (unsigned k = 0; k < num_workers_; ++k) {
      const unsigned victim = (start + k) % num_workers_;
      if (victim == skip) continue;
      if (auto t = workers_[victim]->deque.steal()) return *t;
    }
    return nullptr;
  }

  bool any_work_visible() const {
    if (inject_size_.load(std::memory_order_relaxed) > 0) return true;
    for (const auto& w : workers_) {
      if (w->deque.size_estimate() > 0) return true;
    }
    return false;
  }

  // Spin briefly (work often shows up within microseconds), then park.
  //
  // Lost-wakeup argument (Dekker-style, both sides use a seq_cst fence):
  //   sleeper: sleepers_++  ; fence ; read deques   -> sleep if empty
  //   spawner: write deque  ; fence ; read sleepers_ -> notify if > 0
  // At least one side must see the other's write, so either the sleeper finds
  // the new task or the spawner sees the sleeper and notifies it. The spawner
  // notifies while holding sleep_mu_, which the sleeper holds from its check
  // until cv.wait() atomically releases it, so the notify cannot slip between.
  void idle(Worker& self) {
    for (int i = 0; i < kSpinRounds; ++i) {
      if (Task* task = find_task(self)) {
        execute(self, task);
        return;
      }
      if (stop_.load(std::memory_order_relaxed)) return;
      std::this_thread::yield();
    }

    std::unique_lock<std::mutex> lock(sleep_mu_);
    sleepers_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!stop_.load(std::memory_order_relaxed) && !any_work_visible()) {
      bump(self.sleeps);
      sleep_cv_.wait(lock);
    }
    sleepers_.fetch_sub(1, std::memory_order_relaxed);
  }

  void wake_one_if_sleeping() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_relaxed) > 0) {
      std::lock_guard<std::mutex> lock(sleep_mu_);
      sleep_cv_.notify_one();
    }
  }

  const unsigned num_workers_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<WorkerThread> threads_;

  std::mutex inject_mu_;
  std::deque<Task*> inject_;
  std::atomic<std::size_t> inject_size_{0};

  std::mutex sleep_mu_;
  std::condition_variable sleep_cv_;
  alignas(kCacheLine) std::atomic<int> sleepers_{0};
  std::atomic<bool> stop_{false};

  static inline thread_local Scheduler* tls_scheduler = nullptr;
  static inline thread_local unsigned tls_index = 0;
};

}  // namespace wss
