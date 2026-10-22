#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace wss {

// Counts tasks spawned into it that have not finished yet. wait() returns once
// the count drops to zero. A group must outlive every task spawned into it.
class TaskGroup {
 public:
  TaskGroup() = default;
  TaskGroup(const TaskGroup&) = delete;
  TaskGroup& operator=(const TaskGroup&) = delete;

  bool done() const { return pending_.load(std::memory_order_acquire) == 0; }

 private:
  friend struct Task;

  std::atomic<std::int64_t> pending_{0};
};

// One unit of work. Both schedulers use this exact type, so the benchmark
// compares queueing strategies, not task representations.
struct Task {
  std::function<void()> fn;
  TaskGroup* group;

  // Tasks must not throw: an exception escaping a worker calls std::terminate.
  static void run(Task* task) {
    task->fn();
    TaskGroup* group = task->group;
    delete task;
    // Release: everything the task wrote happens-before the waiter's acquire
    // load that observes zero. The group may be destroyed right after this.
    group->pending_.fetch_sub(1, std::memory_order_release);
  }

  template <typename F>
  static Task* make(TaskGroup& group, F&& fn) {
    group.pending_.fetch_add(1, std::memory_order_relaxed);
    return new Task{std::function<void()>(std::forward<F>(fn)), &group};
  }
};

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

}  // namespace wss
