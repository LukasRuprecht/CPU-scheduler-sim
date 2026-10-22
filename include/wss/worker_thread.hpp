#pragma once

#include <pthread.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <system_error>

namespace wss {

// A joinable thread with an explicit stack size (std::thread cannot set one).
//
// Both schedulers run tasks inside wait() ("help while waiting"), so a waiting
// task's frame can sit under the frames of the tasks it helps with. macOS gives
// secondary threads only 512 KiB, which the global-queue baseline overflowed at
// 2^17 tasks. Workers get 16 MiB of address space; untouched pages cost nothing.
class WorkerThread {
 public:
  static constexpr std::size_t kStackBytes = 16u << 20;

  explicit WorkerThread(std::function<void()> body)
      : body_(std::make_unique<std::function<void()>>(std::move(body))) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    const int rc = pthread_create(&handle_, &attr, &WorkerThread::trampoline, body_.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) throw std::system_error(rc, std::generic_category(), "pthread_create");
  }

  WorkerThread(WorkerThread&& other) noexcept
      : handle_(other.handle_), body_(std::move(other.body_)), joinable_(other.joinable_) {
    other.joinable_ = false;
  }

  WorkerThread(const WorkerThread&) = delete;
  WorkerThread& operator=(const WorkerThread&) = delete;
  WorkerThread& operator=(WorkerThread&&) = delete;

  ~WorkerThread() {
    if (joinable_) join();
  }

  void join() {
    pthread_join(handle_, nullptr);
    joinable_ = false;
  }

 private:
  static void* trampoline(void* arg) {
    (*static_cast<std::function<void()>*>(arg))();
    return nullptr;
  }

  pthread_t handle_{};
  std::unique_ptr<std::function<void()>> body_;
  bool joinable_ = true;
};

}  // namespace wss
