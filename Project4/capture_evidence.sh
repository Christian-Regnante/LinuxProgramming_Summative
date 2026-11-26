#!/usr/bin/env bash
# Regenerates every file in evidence/ (quoted by README.md and the report).
set -u
cd "$(dirname "$0")" || exit 1
E=evidence; mkdir -p "$E"
export MAKEFLAGS=--no-print-directory

{ echo "\$ gcc --version | sed -n 1p; uname -srm"; gcc --version | sed -n 1p; uname -srm; echo
  echo "\$ make clean && make all library_reservation_tsan"; make clean >/dev/null
  make all library_reservation_tsan 2>&1; echo
  echo "Compiler warnings: $(make -B all 2>&1 | grep -ci warning)"; } > "$E/build_log.txt"

# 1) Full run with the real assignment timings (1 s / 3 s / 4 s).
echo "\$ ./library_reservation" > "$E/run_output.txt"
./library_reservation >> "$E/run_output.txt" 2>&1
echo "[exit status: $?]" >> "$E/run_output.txt"

# 2) ThreadSanitizer run (time scale 0.1 because TSan slows execution).
{ echo "\$ ./library_reservation_tsan --time-scale=0.1"
  ./library_reservation_tsan --time-scale=0.1 2>&1; echo "[exit status: $?]"; } > "$E/tsan_run.txt"
echo "ThreadSanitizer warnings: $(grep -c 'WARNING: ThreadSanitizer' "$E/tsan_run.txt")" >> "$E/tsan_run.txt"

# 3) Negative control: TSan must catch a deliberately injected race.
./tests/tsan_negative_control.sh > "$E/tsan_negative_control.txt" 2>&1

# 4) Stress test across seeds and time scales.
./tests/stress_test.sh 40 > "$E/stress_test.txt" 2>&1

# 5) How the threads use the kernel: thread creation, futex waits, sleeps.
strace -f -c -e trace=clone3,clone,futex,clock_nanosleep -o "$E/strace_summary.txt" \
    ./library_reservation --time-scale=0.1 > /dev/null 2>&1

echo "Evidence written to $E/"
