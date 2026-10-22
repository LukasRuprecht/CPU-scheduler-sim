#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace wss {

// Apple M-series cores use 128-byte cache lines; most x86 parts use 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Lock-free Chase-Lev work-stealing deque.
//
//   Chase & Lev, "Dynamic Circular Work-Stealing Deque", SPAA 2005.
//   Le, Pop, Cohen & Zappa Nardelli, "Correct and Efficient Work-Stealing for
//   Weak Memory Models", PPoPP 2013  (source of the memory orderings below).
//
// One owner thread calls push() and pop() on the bottom end (LIFO).
// Any number of thief threads call steal() on the top end (FIFO).
//
//        top                           bottom
//         |                              |
//         v                              v
//   ... [ t ][ t+1 ][ ... ][ b-1 ][ free ] ...
//    thieves CAS top++        owner writes/reads near bottom
//
// Old buffers are kept until the deque is destroyed: a thief may still be
// reading from a buffer the owner has just replaced, and without hazard
// pointers or epochs there is no safe earlier point to free it.
template <typename T>
class ChaseLevDeque {
  static_assert(std::is_trivially_copyable_v<T>,
                "slots are std::atomic<T>, so T must be trivially copyable");

 public:
  explicit ChaseLevDeque(std::size_t initial_capacity = 256)
      : buffer_(new Buffer(round_up_pow2(initial_capacity))) {}

  ~ChaseLevDeque() { delete buffer_.load(std::memory_order_relaxed); }

  ChaseLevDeque(const ChaseLevDeque&) = delete;
  ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

  // Owner only.
  void push(T item) {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_acquire);
    Buffer* buf = buffer_.load(std::memory_order_relaxed);

    if (b - t > buf->capacity - 1) {  // full: double the ring
      Buffer* bigger = buf->grow(t, b);
      retired_.emplace_back(buf);
      buffer_.store(bigger, std::memory_order_release);
      buf = bigger;
    }

    buf->store(b, item);
    // Publishes the slot write (and everything the owner wrote into the task
    // before pushing) to any thief that acquire-loads bottom. The paper uses a
    // release fence + relaxed store; a release store is equivalent here and is
    // understood by ThreadSanitizer, which does not model standalone fences.
    bottom_.store(b + 1, std::memory_order_release);
  }

  // Owner only. Returns the most recently pushed item, or nullopt if empty.
  std::optional<T> pop() {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Buffer* buf = buffer_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);  // reserve slot b

    // The key fence: the bottom store must be globally visible before we read
    // top, otherwise owner and thief can both believe they got the last item.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = top_.load(std::memory_order_relaxed);

    if (t > b) {  // was already empty
      bottom_.store(b + 1, std::memory_order_relaxed);
      return std::nullopt;
    }

    T item = buf->load(b);
    if (t == b) {
      // Exactly one item left: race thieves for it with the same CAS they use.
      const bool won = top_.compare_exchange_strong(
          t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
      bottom_.store(b + 1, std::memory_order_relaxed);
      if (!won) return std::nullopt;
    }
    return item;
  }

  // Any thread. Returns the oldest item, or nullopt if the deque looked empty
  // or another thread won the race for the top item.
  std::optional<T> steal() {
    std::int64_t t = top_.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = bottom_.load(std::memory_order_acquire);
    if (t >= b) return std::nullopt;

    Buffer* buf = buffer_.load(std::memory_order_acquire);  // paper: consume
    T item = buf->load(t);
    // Claim slot t. Failure means the owner's pop() or another thief took it.
    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                      std::memory_order_relaxed)) {
      return std::nullopt;
    }
    return item;
  }

  // Racy snapshot, for heuristics only (e.g. "is there anything to steal?").
  std::int64_t size_estimate() const {
    const std::int64_t b = bottom_.load(std::memory_order_relaxed);
    const std::int64_t t = top_.load(std::memory_order_relaxed);
    return b > t ? b - t : 0;
  }

  std::int64_t capacity() const {
    return buffer_.load(std::memory_order_relaxed)->capacity;
  }

 private:
  struct Buffer {
    explicit Buffer(std::int64_t cap)
        : capacity(cap), mask(cap - 1), slots(new std::atomic<T>[cap]) {}

    // Slots are atomics (accessed relaxed) so a thief reading a slot while the
    // owner reuses it is a benign race the CAS on top resolves, not UB.
    T load(std::int64_t i) const {
      return slots[i & mask].load(std::memory_order_relaxed);
    }
    void store(std::int64_t i, T v) {
      slots[i & mask].store(v, std::memory_order_relaxed);
    }
    Buffer* grow(std::int64_t t, std::int64_t b) const {
      auto* bigger = new Buffer(capacity * 2);
      for (std::int64_t i = t; i < b; ++i) bigger->store(i, load(i));
      return bigger;
    }

    const std::int64_t capacity;
    const std::int64_t mask;
    std::unique_ptr<std::atomic<T>[]> slots;
  };

  static std::int64_t round_up_pow2(std::size_t n) {
    std::int64_t cap = 2;
    while (cap < static_cast<std::int64_t>(n)) cap <<= 1;
    return cap;
  }

  // top_ is written by thieves, bottom_ by the owner: keep them on separate
  // cache lines so owner push/pop does not bounce the thieves' line.
  alignas(kCacheLine) std::atomic<std::int64_t> top_{0};
  alignas(kCacheLine) std::atomic<std::int64_t> bottom_{0};
  alignas(kCacheLine) std::atomic<Buffer*> buffer_;
  std::vector<std::unique_ptr<Buffer>> retired_;  // owner-only
};

}  // namespace wss
