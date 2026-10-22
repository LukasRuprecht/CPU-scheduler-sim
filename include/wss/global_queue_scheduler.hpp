#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "task.hpp"
#include "worker_thread.hpp"

namespace wss {

// Baseline: one deque shared by every worker, guarded by one mutex.
// Same API, thread layout and Task type as Scheduler (the calling thread helps
// inside wait()), and the same order policy: a thread inside wait() takes the
// newest task (like a Chase-Lev owner pop), an idle worker takes the oldest
// (like a steal). So the only thing that differs is one shared lock versus
// per-worker lock-free deques.
//
// Why not plain FIFO for wait()? A waiter that helps by running the *oldest*
// task runs a large subtree that spawns and waits again, so waits nest without
// bound and overflow the stack (the test suite hit this at 1 thread).
class GlobalQueueScheduler {
 public:
  explicit GlobalQueueScheduler(unsigned num_workers = std::thread::hardware_concurrency())
      : num_workers_(num_workers == 0 ? 1 : num_workers) {
    for (unsigned i = 1; i < num_workers_; ++i) {
      threads_.emplace_back([this] { worker_loop(); });
    }
  }

  ~GlobalQueueScheduler() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }

  GlobalQueueScheduler(const GlobalQueueScheduler&) = delete;
  GlobalQueueScheduler& operator=(const GlobalQueueScheduler&) = delete;

  template <typename F>
  void spawn(TaskGroup& group, F&& fn) {
    Task* task = Task::make(group, std::forward<F>(fn));
    bool wake;
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.push_back(task);
      wake = sleeping_ > 0;
    }
    // Only signal when someone is parked: an unconditional notify_one() per
    // spawn would make the baseline pay for syscalls it doesn't need.
    if (wake) cv_.notify_one();
  }

  void wait(TaskGroup& group) {
    while (!group.done()) {
      if (Task* task = try_pop()) {
        Task::run(task);
      } else {
        cpu_relax();
      }
    }
  }

  unsigned num_workers() const { return num_workers_; }

 private:
  Task* try_pop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) return nullptr;
    Task* t = queue_.back();
    queue_.pop_back();
    return t;
  }

  void worker_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      while (!stop_ && queue_.empty()) {
        ++sleeping_;
        cv_.wait(lock);
        --sleeping_;
      }
      if (queue_.empty()) return;  // stop_ and drained
      Task* task = queue_.front();
      queue_.pop_front();
      lock.unlock();
      Task::run(task);
      lock.lock();
    }
  }

  const unsigned num_workers_;
  std::vector<WorkerThread> threads_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task*> queue_;
  int sleeping_ = 0;  // guarded by mu_
  bool stop_ = false;
};

}  // namespace wss
