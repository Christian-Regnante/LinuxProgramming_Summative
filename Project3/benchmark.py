#!/usr/bin/env python3
"""Performance comparison: sensor_processor (C) vs equivalent pure-Python code.

    python3 benchmark.py            # 1,000,000 readings (default)
    python3 benchmark.py 200000     # custom size

Each function is timed with time.perf_counter, taking the best of 5 runs to
reduce noise. Pure-Python versions are written idiomatically (comprehensions
and built-ins such as min/max/sum, which are themselves implemented in C), so
the comparison is fair rather than against deliberately slow Python.
"""
import math
import platform
import random
import sys
import time

import sensor_processor as sp

N = int(sys.argv[1]) if len(sys.argv) > 1 else 1_000_000
LO, HI, WINDOW = 20.0, 40.0, 50
REPEAT = 5


# ---- pure-Python reference implementations --------------------------------
def py_filter(data, lo, hi):
    return [float(x) for x in data if lo <= x <= hi]


def py_count_outliers(data, lo, hi):
    return sum(1 for x in data if x < lo or x > hi)


def py_moving_average(data, w):              # O(n) sliding window, like the C code
    out, s = [], math.fsum(data[:w])
    out.append(s / w)
    for i in range(w, len(data)):
        s += data[i] - data[i - w]
        out.append(s / w)
    return out


def py_moving_average_naive(data, w):        # O(n*w) textbook version
    return [sum(data[i:i + w]) / w for i in range(len(data) - w + 1)]


def py_max_diff(data):
    return max(data) - min(data)             # built-ins: two C-level passes


def py_summary(data):
    n = len(data)
    mean = math.fsum(data) / n
    return {"samples": n, "minimum": min(data), "maximum": max(data),
            "average": mean, "above_average": sum(1 for x in data if x > mean)}


def best_time(fn, *args):
    best = float("inf")
    for _ in range(REPEAT):
        t0 = time.perf_counter()
        fn(*args)
        best = min(best, time.perf_counter() - t0)
    return best


def main():
    rng = random.Random(42)
    data = [rng.gauss(30.0, 8.0) for _ in range(N)]

    print(f"Python {platform.python_version()} on {platform.machine()}, "
          f"{N:,} readings, best of {REPEAT} runs\n")
    rows = [
        ("filter_readings",    lambda: sp.filter_readings(data, LO, HI),  lambda: py_filter(data, LO, HI)),
        ("count_outliers",     lambda: sp.count_outliers(data, LO, HI),   lambda: py_count_outliers(data, LO, HI)),
        (f"moving_average w={WINDOW}", lambda: sp.moving_average(data, WINDOW), lambda: py_moving_average(data, WINDOW)),
        ("maximum_difference", lambda: sp.maximum_difference(data),       lambda: py_max_diff(data)),
        ("sensor_summary",     lambda: sp.sensor_summary(data),           lambda: py_summary(data)),
    ]
    print(f"{'function':<26}{'C extension':>14}{'pure Python':>14}{'speed-up':>11}")
    print("-" * 65)
    for name, c_fn, py_fn in rows:
        c_t, p_t = best_time(c_fn), best_time(py_fn)
        print(f"{name:<26}{c_t * 1000:>11.2f} ms{p_t * 1000:>11.2f} ms{p_t / c_t:>10.1f}x")

    # Algorithmic effect: O(n) sliding window vs O(n*w) naive definition.
    small = data[:100_000]
    print(f"\nmoving_average on {len(small):,} readings, window {WINDOW}:")
    c_t = best_time(lambda: sp.moving_average(small, WINDOW))
    n_t = best_time(lambda: py_moving_average_naive(small, WINDOW))
    print(f"  C sliding window O(n)      : {c_t * 1000:9.2f} ms")
    print(f"  Python naive O(n*w)        : {n_t * 1000:9.2f} ms   ({n_t / c_t:.0f}x slower)")

    # Numerical accuracy: naive sliding sum vs the C compensated sliding sum.
    tricky = [1e15, -1e15] * 50_000 + [0.1] * 10      # true last window mean = 0.1
    naive_sum = sum(tricky[:10])
    for i in range(10, len(tricky)):
        naive_sum += tricky[i] - tricky[i - 10]
    print("\nAccuracy: last 10-reading window of [1e15, -1e15] * 50000 + [0.1] * 10 (exact mean 0.1)")
    print(f"  naive sliding sum (Python)          : {naive_sum / 10!r}")
    print(f"  compensated sliding sum (C extension): {sp.moving_average(tricky, 10)[-1]!r}")

    # Correctness cross-check of the benchmarked results.
    assert sp.count_outliers(data, LO, HI) == py_count_outliers(data, LO, HI)
    assert sp.filter_readings(data, LO, HI) == py_filter(data, LO, HI)
    assert sp.maximum_difference(data) == py_max_diff(data)
    c_ma, p_ma = sp.moving_average(data, WINDOW), py_moving_average(data, WINDOW)
    assert all(math.isclose(a, b, rel_tol=1e-9) for a, b in zip(c_ma, p_ma))
    print("\nAll C results match the pure-Python results.")


if __name__ == "__main__":
    main()
