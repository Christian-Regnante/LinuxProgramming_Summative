#!/usr/bin/env bash
# Runs every tests/cases/*.txt through ./sensor_stats and compares stdout and
# the exit status with tests/expected/*.out (hand-derived expected results).
# Also checks the two error paths (missing file, read error).
cd "$(dirname "$0")/.." || exit 1
BIN=./sensor_stats
pass=0; fail=0

check() {   # name, expected-output, expected-exit, actual-output, actual-exit
    if [[ "$4" == "$2" && "$5" == "$3" ]]; then
        printf '  PASS  %s\n' "$1"; pass=$((pass+1))
    else
        printf '  FAIL  %s (exit %s, expected %s)\n' "$1" "$5" "$3"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$4") | sed 's/^/        /'
        fail=$((fail+1))
    fi
}

echo "== Data cases =="
for input in tests/cases/*.txt; do
    name=$(basename "$input" .txt)
    out=$("$BIN" "$input" 2>&1); code=$?
    check "$name" "$(cat "tests/expected/$name.out")" 0 "$out" "$code"
done

echo "== Default file (no argument -> sensor_readings.txt) =="
out=$("$BIN" 2>&1); code=$?
check "default_sensor_readings" "$(cat tests/expected/sensor_readings.out)" 0 "$out" "$code"

echo "== Error handling =="
out=$("$BIN" tests/cases/does_not_exist.txt 2>&1); code=$?
check "open_error_missing_file" \
  "Error: cannot open file 'tests/cases/does_not_exist.txt' (errno 2: No such file or directory)" 1 "$out" "$code"

out=$("$BIN" tests/cases 2>&1); code=$?        # open() of a directory succeeds, read() fails
check "read_error_directory" \
  "Error: failed while reading file 'tests/cases' (errno 21: Is a directory)" 2 "$out" "$code"

echo
echo "Result: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
