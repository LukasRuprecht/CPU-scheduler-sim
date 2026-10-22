// Correctness and stress tests. Build with `make test` and `make tsan`.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "wss/chase_lev_deque.hpp"
#include "wss/global_queue_scheduler.hpp"
#include "wss/scheduler.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

#ifdef WSS_TSAN
constexpr int kScale = 10;  // TSan is ~10x slower; shrink the stress loops
#else
constexpr int kScale = 1;
#endif

// ---------------------------------------------------------------- deque ----

void deque_lifo_fifo_and_growth() {
  wss::ChaseLevDeque<std::uint64_t> dq(4);
  const std::uint64_t n = 10'000;  // forces ~11 doublings from capacity 4
  for (std::uint64_t i = 1; i <= n; ++i) dq.push(i);
  CHECK(dq.capacity() >= static_cast<std::int64_t>(n));
  CHECK(dq.size_estimate() == static_cast<std::int64_t>(n));

  CHECK(dq.steal().value_or(0) == 1);  // thieves take the oldest
  CHECK(dq.steal().value_or(0) == 2);
  for (std::uint64_t i = n; i >= 3; --i) {  // owner takes the newest
    auto v = dq.pop();
    CHECK(v.has_value() && *v == i);
  }
  CHECK(!dq.pop().has_value());
  CHECK(!dq.steal().has_value());

  dq.push(42);  // usable again after being drained
  CHECK(dq.pop().value_or(0) == 42);
}

// Owner pushes and pops while thieves steal. Every item must be consumed
// exactly once, never lost, never duplicated. Small initial capacity so the
// buffer grows while thieves are reading it.
void deque_concurrent_exactly_once() {
  const int rounds = 20;
  const std::uint64_t items = 200'000 / kScale;
  const unsigned thieves = std::max(2u, std::thread::hardware_concurrency() - 1);

  for (int round = 0; round < rounds; ++round) {
    wss::ChaseLevDeque<std::uint64_t> dq(2);
    auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(items);
    for (std::uint64_t i = 0; i < items; ++i) seen[i].store(0, std::memory_order_relaxed);
    std::atomic<bool> owner_done{false};
    std::atomic<std::uint64_t> consumed{0};

    auto take = [&](std::uint64_t v) {
      seen[v].fetch_add(1, std::memory_order_relaxed);
      consumed.fetch_add(1, std::memory_order_relaxed);
    };

    std::vector<std::thread> ts;
    for (unsigned k = 0; k < thieves; ++k) {
      ts.emplace_back([&] {
        while (!owner_done.load(std::memory_order_acquire) || dq.size_estimate() > 0) {
          if (auto v = dq.steal()) take(*v);
        }
      });
    }

    // Owner: push in bursts, pop a few after each burst, so pop() races steal()
    // on the last element many times.
    std::uint64_t next = 0;
    while (next < items) {
      const std::uint64_t burst = 1 + (next * 2654435761u) % 7;
      for (std::uint64_t j = 0; j < burst && next < items; ++j) dq.push(next++);
      for (int j = 0; j < 3; ++j) {
        if (auto v = dq.pop()) take(*v);
      }
    }
    while (auto v = dq.pop()) take(*v);
    owner_done.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();

    CHECK(consumed.load() == items);
    std::uint64_t bad = 0;
    for (std::uint64_t i = 0; i < items; ++i) bad += seen[i].load() != 1;
    CHECK(bad == 0);
    if (bad) {
      std::fprintf(stderr, "  round %d: %llu items not seen exactly once\n", round,
                   static_cast<unsigned long long>(bad));
      return;
    }
  }
}

// ------------------------------------------------------------ schedulers ----

std::uint64_t fib_seq(int n) { return n < 2 ? n : fib_seq(n - 1) + fib_seq(n - 2); }

template <typename S>
std::uint64_t fib_par(S& s, int n, int cutoff) {
  if (n < cutoff) return fib_seq(n);
  std::uint64_t a = 0;
  wss::TaskGroup g;
  s.spawn(g, [&] { a = fib_par(s, n - 1, cutoff); });
  const std::uint64_t b = fib_par(s, n - 2, cutoff);
  s.wait(g);
  return a + b;
}

template <typename S>
void sched_fib(unsigned threads) {
  S s(threads);
  for (int cutoff : {2, 5, 10}) {
    CHECK(fib_par(s, 24 - (kScale > 1 ? 4 : 0), cutoff) == fib_seq(24 - (kScale > 1 ? 4 : 0)));
  }
}

template <typename S>
void sched_flat_exactly_once(unsigned threads) {
  S s(threads);
  const std::size_t n = 500'000 / kScale;
  auto hits = std::make_unique<std::atomic<std::uint8_t>[]>(n);
  for (std::size_t i = 0; i < n; ++i) hits[i].store(0, std::memory_order_relaxed);
  wss::TaskGroup g;
  for (std::size_t i = 0; i < n; ++i) {
    s.spawn(g, [&hits, i] { hits[i].fetch_add(1, std::memory_order_relaxed); });
  }
  s.wait(g);
  std::size_t bad = 0;
  for (std::size_t i = 0; i < n; ++i) bad += hits[i].load() != 1;
  CHECK(bad == 0);
}

// Each task writes plain (non-atomic) memory; the waiter reads it after wait().
// TSan flags this if TaskGroup's release/acquire pairing is wrong.
template <typename S>
void sched_results_visible_after_wait(unsigned threads) {
  S s(threads);
  std::vector<std::uint64_t> out(10'000, 0);
  wss::TaskGroup g;
  for (std::size_t i = 0; i < out.size(); ++i) {
    s.spawn(g, [&out, i] { out[i] = i * i; });
  }
  s.wait(g);
  bool ok = true;
  for (std::size_t i = 0; i < out.size(); ++i) ok &= out[i] == i * i;
  CHECK(ok);
}

// Spawn from a thread that is not a worker (goes through the injection queue).
template <typename S>
void sched_external_spawner(unsigned threads) {
  S s(threads);
  std::atomic<int> count{0};
  std::thread outsider([&] {
    wss::TaskGroup g;
    for (int i = 0; i < 20'000; ++i) {
      s.spawn(g, [&] { count.fetch_add(1, std::memory_order_relaxed); });
    }
    s.wait(g);
  });
  outsider.join();
  CHECK(count.load() == 20'000);
}

// Construct and destroy repeatedly: shutdown must not hang or leak a sleeper.
template <typename S>
void sched_start_stop(unsigned threads) {
  for (int i = 0; i < 200 / kScale; ++i) {
    S s(threads);
    if (i % 2) {
      wss::TaskGroup g;
      std::atomic<int> x{0};
      for (int j = 0; j < 100; ++j) s.spawn(g, [&] { x.fetch_add(1); });
      s.wait(g);
      CHECK(x.load() == 100);
    }
  }
}

template <typename S>
void sched_suite(const char* name) {
  for (unsigned threads : {1u, 2u, 8u}) {
    std::printf("  %s x%u\n", name, threads);
    sched_fib<S>(threads);
    sched_flat_exactly_once<S>(threads);
    sched_results_visible_after_wait<S>(threads);
    sched_external_spawner<S>(threads);
    sched_start_stop<S>(threads);
  }
}

}  // namespace

int main() {
  std::printf("deque: single-thread order and growth\n");
  deque_lifo_fifo_and_growth();
  std::printf("deque: concurrent exactly-once (owner + thieves)\n");
  deque_concurrent_exactly_once();
  std::printf("work-stealing scheduler\n");
  sched_suite<wss::Scheduler>("Scheduler");
  std::printf("global-queue scheduler\n");
  sched_suite<wss::GlobalQueueScheduler>("GlobalQueueScheduler");

  if (g_failures) {
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
