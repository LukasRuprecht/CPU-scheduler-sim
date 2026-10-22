# Work-stealing task scheduler (C++17)

A header-only fork-join task scheduler with one lock-free **Chase-Lev deque** per
worker, benchmarked against a mutex-guarded global queue that uses the same API,
the same thread layout and the same task type.

```cpp
wss::Scheduler sched(8);                 // calling thread becomes worker 0
wss::TaskGroup g;
std::uint64_t a = 0;
sched.spawn(g, [&] { a = work(1); });    // pushed onto this worker's deque
std::uint64_t b = work(2);
sched.wait(g);                           // runs tasks until g is done
```

## Layout

| File | Contents |
| --- | --- |
| `include/wss/chase_lev_deque.hpp` | Lock-free deque (orderings from Le et al., PPoPP 2013), growable ring buffer |
| `include/wss/scheduler.hpp` | Work-stealing scheduler: random-victim stealing, help-while-waiting, spin-then-sleep |
| `include/wss/global_queue_scheduler.hpp` | Baseline: one `std::deque` + `std::mutex` + condvar |
| `include/wss/task.hpp` | `Task` and `TaskGroup` (atomic pending counter) |
| `include/wss/worker_thread.hpp` | pthread wrapper with a 16 MiB stack |
| `tests/test_main.cpp` | Exactly-once stress tests for the deque and both schedulers |
| `bench/bench.cpp` | Grain sweep, thread scaling, fib, flat spawn |

## Build and run

```sh
make test     # stress tests (-O2)
make tsan     # same tests under ThreadSanitizer
make bench    # writes results/bench.csv
```

Requires a C++17 compiler and POSIX threads. `make tsan` and `make asan` need a
sanitizer runtime that matches your OS. On macOS 26 with Apple clang 14
(Xcode 14.3 CLT) the runtime crashes on startup. Update the Command Line Tools
first.

## Results (Apple M1, 4P + 4E cores, 8 threads, median of 9 runs)

| Workload | Work stealing | Global queue | Ratio |
| --- | --- | --- | --- |
| Task tree 2^17, empty leaves | 5.43 ms | 100.37 ms | 18.5x |
| Task tree 2^17, ~170 ns leaves | 7.98 ms | 162.30 ms | 20.3x |
| Task tree 2^17, ~1.7 us leaves | 38.17 ms | 361.24 ms | 9.5x |
| Task tree 2^17, ~17 us leaves | 294.95 ms | 589.60 ms | 2.0x |
| Task tree 2^17, ~170 us leaves | 2787.67 ms | 2986.66 ms | 1.07x |
| fib(32), cutoff 10 (75,024 tasks) | 4.74 ms | 115.75 ms | 24.4x |
| fib(32), cutoff 16 (4,180 tasks) | 2.37 ms | 15.89 ms | 6.7x |
| Flat spawn, 200k x ~1.7 us tasks | 81.56 ms | 250.33 ms | 3.07x |

Full output: `results/bench.txt` and `results/bench.csv`. Only 0.06% of tasks were
stolen in the tree benchmark: almost all work runs on the deque that spawned it.

## Origin: `sim/`

`sim/` holds the project's first version: a single-threaded simulator of CPU
cores, each with its own task deque, that moves work from the busiest core to
idle ones. This repo turns that model into a real multithreaded runtime. The
cores become OS threads, the deques become lock-free, and moving work becomes
concurrent stealing.

## Known limitations

- One heap allocation per task (`new Task` + `std::function`), paid equally by both schedulers.
- Retired deque buffers are freed only when the deque is destroyed (bounded at under 2x the peak size).
- Flat spawning from one thread sends every thief to the same deque's `top`.
- Tasks must not throw.
