// Benchmarks: work-stealing Scheduler vs mutex-guarded GlobalQueueScheduler.
//
//   ./build/bench [--threads N] [--reps R] [--csv path] [--quick]
//
// Every configuration runs R timed repetitions (after one warm-up) on a
// scheduler that is already started, and reports the median wall time.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "wss/global_queue_scheduler.hpp"
#include "wss/scheduler.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------- busy work ----

thread_local std::uint64_t tl_sink = 0;  // keeps burn() from being optimized out

// A dependent chain of multiply-adds: ~1 ns per iteration, no memory traffic,
// so task "size" is controlled precisely and the scheduler is what's measured.
inline std::uint64_t burn(std::uint64_t iters, std::uint64_t seed) {
  std::uint64_t x = seed;
  for (std::uint64_t i = 0; i < iters; ++i) {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    asm volatile("" : "+r"(x));
  }
  return x;
}

double ns_per_iter() {
  const std::uint64_t n = 50'000'000;
  const auto t0 = Clock::now();
  tl_sink += burn(n, 1);
  const auto t1 = Clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
}

// ------------------------------------------------------------- workloads ----

// Binary tree of tasks: each internal node spawns one child, recurses into the
// other, then waits. 2^depth leaves each burn `iters`; 2^depth - 1 spawns.
template <typename S>
void tree(S& s, int depth, std::uint64_t iters) {
  if (depth == 0) {
    tl_sink += burn(iters, 7);
    return;
  }
  wss::TaskGroup g;
  s.spawn(g, [&s, depth, iters] { tree(s, depth - 1, iters); });
  tree(s, depth - 1, iters);
  s.wait(g);
}

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

std::uint64_t fib_spawns(int n, int cutoff) {
  return n < cutoff ? 0 : 1 + fib_spawns(n - 1, cutoff) + fib_spawns(n - 2, cutoff);
}

// One thread spawns `count` independent tasks, then waits. This is the shape
// work stealing is *least* suited to: every task starts on one deque, so every
// other worker contends on that deque's top.
template <typename S>
void flat(S& s, std::uint64_t count, std::uint64_t iters) {
  wss::TaskGroup g;
  for (std::uint64_t i = 0; i < count; ++i) {
    s.spawn(g, [iters] { tl_sink += burn(iters, 3); });
  }
  s.wait(g);
}

// -------------------------------------------------------------- harness ----

struct Options {
  unsigned threads = 8;
  int reps = 9;
  bool quick = false;
  std::string csv;
};

struct Result {
  double median_ms, min_ms, max_ms;
};

template <typename Fn>
Result measure(int reps, Fn&& fn) {
  fn();  // warm-up: grows deques, faults in pages, wakes workers
  std::vector<double> ms;
  for (int r = 0; r < reps; ++r) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  return {ms[ms.size() / 2], ms.front(), ms.back()};
}

FILE* g_csv = nullptr;

void emit(const char* bench, const std::string& param, const char* sched, unsigned threads,
          std::uint64_t tasks, const Result& r) {
  const double mtps = tasks / (r.median_ms * 1e3);  // million tasks per second
  std::printf("  %-14s %-22s %-6s x%-2u  %9.2f ms  [%8.2f, %8.2f]  %8.2f Mtask/s\n", bench,
              param.c_str(), sched, threads, r.median_ms, r.min_ms, r.max_ms, mtps);
  if (g_csv) {
    std::fprintf(g_csv, "%s,%s,%s,%u,%llu,%.4f,%.4f,%.4f,%.4f\n", bench, param.c_str(), sched,
                 threads, static_cast<unsigned long long>(tasks), r.median_ms, r.min_ms,
                 r.max_ms, mtps);
    std::fflush(g_csv);
  }
}

void ratio(const Result& ws, const Result& gq) {
  std::printf("  %-14s -> work-stealing is %.2fx the global queue's throughput\n", "",
              gq.median_ms / ws.median_ms);
}

template <typename Fn>
void compare(const Options& o, const char* bench, const std::string& param, unsigned threads,
             std::uint64_t tasks, Fn&& run) {
  Result ws, gq;
  {
    wss::Scheduler s(threads);
    ws = measure(o.reps, [&] { run(s); });
    emit(bench, param, "ws", threads, tasks, ws);
  }
  {
    wss::GlobalQueueScheduler s(threads);
    gq = measure(o.reps, [&] { run(s); });
    emit(bench, param, "gq", threads, tasks, gq);
  }
  ratio(ws, gq);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) {
      o.threads = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) {
      o.reps = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) {
      o.csv = argv[++i];
    } else if (!std::strcmp(argv[i], "--quick")) {
      o.quick = true;
      o.reps = 3;
    } else {
      std::fprintf(stderr, "usage: %s [--threads N] [--reps R] [--csv path] [--quick]\n",
                   argv[0]);
      std::exit(2);
    }
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const Options o = parse(argc, argv);
  if (!o.csv.empty()) {
    g_csv = std::fopen(o.csv.c_str(), "w");
    if (g_csv) {
      std::fprintf(g_csv,
                   "bench,param,scheduler,threads,tasks,median_ms,min_ms,max_ms,mtasks_per_s\n");
    }
  }

  const double nspi = ns_per_iter();
  std::printf("hardware_concurrency=%u  threads=%u  reps=%d  burn=%.3f ns/iter\n",
              std::thread::hardware_concurrency(), o.threads, o.reps, nspi);
  const unsigned T = o.threads;
  const int depth = o.quick ? 14 : 17;
  const std::uint64_t tree_tasks = (1ull << depth) - 1;

  // 1. Task-size sweep: where does the shared lock stop mattering?
  std::printf("\n[grain] binary task tree, 2^%d leaves, leaf work varies\n", depth);
  for (std::uint64_t iters : {0ull, 100ull, 1000ull, 10000ull, 100000ull}) {
    const std::string param = "leaf=" + std::to_string(iters) + "it";
    compare(o, "grain", param, T, tree_tasks, [&](auto& s) { tree(s, depth, iters); });
  }

  // 2. Thread scaling at a fine grain.
  std::printf("\n[scaling] binary task tree, 2^%d leaves, leaf=100 iters\n", depth);
  for (unsigned t : {1u, 2u, 4u, 8u}) {
    if (t > T) break;
    compare(o, "scaling", "leaf=100it", t, tree_tasks, [&](auto& s) { tree(s, depth, 100); });
  }

  // 3. Recursive fib with a sequential cutoff (classic fork-join benchmark).
  const int fib_n = o.quick ? 28 : 32;
  std::printf("\n[fib] fib(%d), sequential below the cutoff\n", fib_n);
  {
    // fib_seq is pure, so without the barrier clang hoists it out of the loop.
    const Result seq = measure(o.reps, [&] {
      int n = fib_n;
      asm volatile("" : "+r"(n));
      tl_sink += fib_seq(n);
    });
    std::printf("  %-14s %-22s %-6s x1   %9.2f ms\n", "fib", "sequential", "seq", seq.median_ms);
    if (g_csv) {
      std::fprintf(g_csv, "fib,sequential,seq,1,0,%.4f,%.4f,%.4f,0\n", seq.median_ms,
                   seq.min_ms, seq.max_ms);
    }
  }
  for (int cutoff : {10, 16, 22}) {
    const std::string param = "cutoff=" + std::to_string(cutoff);
    compare(o, "fib", param, T, fib_spawns(fib_n, cutoff),
            [&](auto& s) { tl_sink += fib_par(s, fib_n, cutoff); });
  }

  // 4. Flat spawn from one thread (work stealing's weak spot, reported honestly).
  const std::uint64_t flat_n = o.quick ? 50'000 : 200'000;
  std::printf("\n[flat] one thread spawns %llu independent tasks\n",
              static_cast<unsigned long long>(flat_n));
  for (std::uint64_t iters : {100ull, 1000ull}) {
    const std::string param = "task=" + std::to_string(iters) + "it";
    compare(o, "flat", param, T, flat_n, [&](auto& s) { flat(s, flat_n, iters); });
  }

  // Work-stealing internals for one representative run.
  {
    wss::Scheduler s(T);
    tree(s, depth, 100);
    const auto st = s.stats();
    std::printf("\n[stats] tree 2^%d leaf=100it x%u: executed=%llu steals=%llu (%.2f%% of tasks) "
                "steal_misses=%llu sleeps=%llu\n",
                depth, T, static_cast<unsigned long long>(st.executed),
                static_cast<unsigned long long>(st.steals),
                100.0 * st.steals / std::max<std::uint64_t>(1, st.executed),
                static_cast<unsigned long long>(st.steal_misses),
                static_cast<unsigned long long>(st.sleeps));
  }

  if (g_csv) std::fclose(g_csv);
  return 0;
}
