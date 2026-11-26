#!/usr/bin/env bash
# Regenerates every file in evidence/ (quoted by README.md and the report).
set -u
cd "$(dirname "$0")" || exit 1
E=evidence; mkdir -p "$E"
export MAKEFLAGS=--no-print-directory

{ echo "\$ gcc --version | sed -n 1p; uname -srm"; gcc --version | sed -n 1p; uname -srm; echo
  echo "\$ make clean && make all server_tsan"; make clean >/dev/null; make all server_tsan 2>&1; echo
  echo "Compiler warnings: $(make -B all server_tsan 2>&1 | grep -ci warning)"; } > "$E/build_log.txt"

python3 tests/protocol_test.py      > "$E/protocol_tests.txt" 2>&1
VERBOSE=1 ./tests/race_test.sh 10    > "$E/race_test.txt" 2>&1
./tests/race_negative_control.sh     > "$E/race_negative_control.txt" 2>&1
./tests/tsan_check.sh                > "$E/tsan_check.txt" 2>&1
./tests/run_demo.sh                  > "$E/demo_run.txt" 2>&1
echo "Evidence written to $E/"
