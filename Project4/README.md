# Project 4 — Multithreaded Library Book Reservation System in C

This program simulates a library reservation desk with POSIX threads. It has three threads:
- a **student** (producer) that submits 15 book reservations into a bounded queue of capacity 5;
- a **library staff** member (consumer) who approves or rejects each request;
- a **monitor** that reports the system statistics every 4 seconds.

All shared state is protected by a `pthread_mutex_t`, and the threads coordinate through `pthread_cond_t` condition variables.

The design discussion, analysis and full evidence are in **`Project4_Report.pdf`**.

---

## 1. Files

| Path | Description |
|------|-------------|
| `library_reservation.c` | The complete program, with detailed comments on synchronisation |
| `Makefile` | Targets: `all`, `run`, `fast`, `tsan`, `evidence` and `clean` |
| `tests/stress_test.sh` | 40 runs over different seeds and time scales, each checking the invariants |
| `tests/tsan_negative_control.sh` | Shows that ThreadSanitizer *does* catch a deliberately injected race |
| `capture_evidence.sh` | Regenerates everything in `evidence/` |
| `evidence/` | The full 46-second run, the TSan runs, stress-test results and an strace summary |

## 2. Requirements

You need Linux with GCC (or Clang) and POSIX threads. It was developed with GCC 16.2.1 on Linux 7.2 x86-64. The `make tsan` target also needs ThreadSanitizer, which ships with GCC.

## 3. Build

```bash
cd Project4
make                     # gcc -Wall -Wextra -O2 -pthread -o library_reservation library_reservation.c
```

The program compiles with zero warnings. `make clean` removes the binaries.

## 4. Run

```bash
./library_reservation                     # the assignment timings: 1 s submit, 3 s process, 4 s monitor (~46 s)
./library_reservation --time-scale=0.1    # identical behaviour, every delay ×0.1 (~4.6 s)
./library_reservation --seed=7            # a different sequence of book IDs
./library_reservation --help
```

**Inputs:** there are none. The program generates all 15 requests itself. The book IDs (100–199) come from a fixed seed (default `2026`), so each run is reproducible and includes both even (available) and odd (unavailable) books. `--time-scale` shortens the delays for the sanitizer and stress tests; without it the timings are exactly those in the specification.

**Exit status:** `0` when all five end-of-run invariant checks pass, `1` otherwise.

## 5. Expected output

These excerpts come from `evidence/run_output.txt`, a real run with the default timings:

```
[  0.00s] MAIN     | Library reservation system starting: 15 requests, queue capacity 5
[  0.00s] STAFF    | queue EMPTY - waiting for reservations
[  0.00s] STUDENT  | preparing reservation #1 for book 167 ...
[  1.00s] STUDENT  | SUBMITTED reservation #1 (book 167)  queue 1/5
[  1.00s] STAFF    | took reservation #1 (book 167) from queue after 0.0s  queue 0/5
[  4.00s] MONITOR  | ------ System status report #1 ------
[  4.00s] MONITOR  |   submitted  3 | processed  0 | approved  0 | rejected  0 | queue 2/5 [#2 #3]
[  4.00s] STAFF    | REJECTED reservation #1 - book 167 is unavailable (odd ID)  (processed 1/15)
...
[  8.00s] STUDENT  | SUBMITTED reservation #8 (book 133)  queue 5/5
[  9.00s] STUDENT  | queue FULL (5/5) - waiting to submit #9
[ 10.00s] STAFF    | REJECTED reservation #3 - book 125 is unavailable (odd ID)  (processed 3/15)
[ 10.00s] STAFF    | took reservation #4 (book 120) from queue after 6.0s  queue 4/5
[ 10.00s] STUDENT  | SUBMITTED reservation #9 (book 117)  queue 5/5
...
[ 13.00s] STAFF    | APPROVED reservation #4 - book 120 is available (even ID)  (processed 4/15)
...
[ 46.01s] MONITOR  | ------ FINAL report #12 ------
[ 46.01s] MONITOR  |   submitted 15 | processed 15 | approved  6 | rejected  9 | queue 0/5 []
[ 46.01s] MAIN     | all threads joined

=================== FINAL SUMMARY ===================
 Reservations submitted      : 15
 Reservations processed      : 15
 Approved (even book ID)     : 6
 Rejected (odd book ID)      : 9
 Queue size at exit          : 0
 Maximum queue occupancy     : 5 / 5
 Producer waits (queue full) : 7
 Consumer waits (queue empty): 1
 Average time in queue       : 10.73 s
 Total running time          : 46.01 s
-----------------------------------------------------
 [PASS] all 15 requests submitted
 [PASS] all 15 requests processed
 [PASS] approved + rejected == processed
 [PASS] queue empty at exit
 [PASS] queue never exceeded capacity 5
=====================================================
All mutexes and condition variables destroyed. Program terminated successfully.
```

The output shows every scenario the assignment asks for:

| Required demonstration | Where it appears in the output |
|------------------------|--------------------------------|
| Requests being generated | `STUDENT preparing …` / `SUBMITTED reservation #n` |
| Waiting when the queue is full | `queue FULL (5/5) - waiting to submit #9` (7 times, from t = 9 s) |
| Waiting when the queue is empty | `STAFF queue EMPTY - waiting` at t = 0 |
| Reservation processing | `took reservation #n … from queue` |
| Approved and rejected reservations | `APPROVED … (even ID)` and `REJECTED … (odd ID)` |
| Monitoring reports | `MONITOR System status report #k` every 4 s, including the queue contents |
| Successful termination | `all threads joined`, the summary with all checks `PASS`, sync objects destroyed |

**Timeline:** the student submits one request per second, while staff finish one every 3 seconds. The queue therefore fills at t ≈ 8 s. From then on the student blocks until staff free a slot. All 15 requests are submitted by 28 s, and the last one is processed at 1 + 15 × 3 = **46 s**.

## 6. Design

| Component | Implementation |
|-----------|----------------|
| Queue | A **circular buffer** `buf[5]` with `head`, `tail` and `count`. Enqueue and dequeue are both O(1), and no element is ever shifted |
| Shared state | One `ReservationSystem` struct holding the queue, all counters and the life-cycle flags |
| Mutex | **One** `pthread_mutex_t lock` protects every field. The queue and the counters change together, so each update is atomic as a whole |
| `not_full` condvar | The producer waits on it in `while (count == 5)`. The consumer signals it after a dequeue |
| `not_empty` condvar | The consumer waits on it in `while (count == 0 && !producer_done)`. The producer signals it after an enqueue |
| `shutdown` condvar | The monitor's `pthread_cond_timedwait` uses absolute `CLOCK_MONOTONIC` deadlines every 4 s. The consumer broadcasts on it at the end, so the monitor exits immediately |
| Slow work | `sleep` (1 s or 3 s) runs **outside** the lock, so the lock is only ever held for microseconds |
| Monitor | Copies the statistics under the lock, releases the lock, then prints. Slow terminal I/O therefore never blocks the other threads |
| Approval rule | An even book ID is available and gets **APPROVED**. An odd ID is unavailable and gets **REJECTED** |
| Logging | `log_msg()` is thread-safe: a separate `log_lock` prints whole lines with a timestamp and the thread's role. The lock order is always `sys.lock` → `log_lock`, so a deadlock is impossible |
| Termination | The producer sets `producer_done` and broadcasts. The consumer drains the queue, sets `all_done` and broadcasts `shutdown`. The monitor exits. `main` joins all 3 threads, checks the invariants and destroys every mutex and condvar |
| Extra statistics | Maximum queue occupancy, producer and consumer wait counts, and the average time a request spends in the queue |

## 7. Verification

```bash
make tsan                         # ThreadSanitizer build + run  → 0 warnings
./tests/tsan_negative_control.sh  # an injected unlocked write   → TSan reports the race
./tests/stress_test.sh 40         # 40 runs, 4 time scales, 40 seeds → 40 passed
make evidence                     # regenerate all of evidence/ (~80 s)
```

- **ThreadSanitizer** finds no data races in the real program (`evidence/tsan_run.txt`).
- **Negative control:** the script copies the source and moves one `sys.submitted++` outside the mutex. TSan then reports the race between the producer's unlocked write and the monitor's locked read, so the clean result above is meaningful.
- **Stress test:** time scales down to 0.002 make the thread interleavings vary widely. Every run must terminate on its own, exit 0, pass all 5 invariants, never log a queue size above 5, and approve only even IDs.
- **System calls:** `strace -f -c` shows 3 `clone3` calls (the threads), 30 `clock_nanosleep` calls (15 submissions + 15 processing delays), and `futex` calls, which are the kernel side of blocking mutex and condition-variable waits.
