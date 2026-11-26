#!/usr/bin/env bash
# Captures the terminal evidence quoted in README.md and the report.
# Each demo shows the exact input bytes (cat -A: '$' = LF, '^M' = CR) and the
# program output, so valid, invalid, empty and CRLF records are all visible.
set -u
cd "$(dirname "$0")/.." || exit 1
E=evidence; mkdir -p "$E"

demo() {   # title, file
    echo "### $1"
    echo "\$ cat -A $2"
    cat -A "$2"; [[ -s "$2" ]] || echo "(empty file - 0 bytes)"
    echo "\$ ./sensor_stats $2"
    ./sensor_stats "$2"; echo "[exit status: $?]"
    echo
}

{
  echo "\$ nasm -v ; ld -v ; uname -srm"; nasm -v; ld -v; uname -srm; echo
  echo "\$ make clean && make"; make clean >/dev/null; make 2>&1; echo
  echo "### Default input file (no argument)"
  echo "\$ cat -A sensor_readings.txt"; cat -A sensor_readings.txt
  echo "\$ ./sensor_stats"; ./sensor_stats; echo "[exit status: $?]"; echo
  demo "Task example, Unix LF endings"           tests/cases/01_task_example_lf.txt
  demo "Task example, Windows CRLF endings"      tests/cases/02_task_example_crlf.txt
  demo "Mixed LF / CRLF, including an empty CRLF line" tests/cases/03_mixed_line_endings.txt
  demo "Empty lines are invalid records"         tests/cases/04_empty_lines.txt
  demo "No valid measurements (min/max = N/A)"   tests/cases/05_no_valid_values.txt
  demo "Empty file"                               tests/cases/06_empty_file.txt
  demo "Last line without trailing newline"      tests/cases/07_no_trailing_newline.txt
  demo "Boundaries and malformed numbers"        tests/cases/08_boundaries_and_formats.txt
  echo "### Error: file does not exist"
  echo "\$ ./sensor_stats missing.txt"; ./sensor_stats missing.txt; echo "[exit status: $?]"; echo
  echo "### Error: read() fails (path is a directory)"
  echo "\$ ./sensor_stats tests/"; ./sensor_stats tests/; echo "[exit status: $?]"; echo
  echo "### Error: default file missing (run from another directory)"
  echo "\$ cd /tmp && <project>/Project2/sensor_stats"; (cd /tmp && "$OLDPWD/sensor_stats" 2>&1; echo "[exit status: $?]")
} > "$E/terminal_runs.txt" 2>&1

# System-call level view of the file handling (open/read/write/close/exit).
strace -o "$E/strace_default.txt" ./sensor_stats > /dev/null
strace -e trace=openat,open,read,close,exit -o "$E/strace_large_file.txt" \
       ./sensor_stats tests/cases/12_large_file_over_buffer.txt > /dev/null
strace -o "$E/strace_missing_file.txt" ./sensor_stats missing.txt 2> /dev/null

./tests/run_tests.sh > "$E/test_results.txt" 2>&1
python3 tests/fuzz.py 500 > "$E/fuzz_results.txt" 2>&1
gdb -q -batch -x tests/gdb_registers.gdb ./sensor_stats 2>&1 | grep -E "^---|^    memory|^Total|^Valid|^Invalid|^Sum|^Min|^Max" > "$E/gdb_registers.txt"
{ echo "\$ size sensor_stats ; file sensor_stats"; size sensor_stats; file sensor_stats; echo; \
  echo "\$ readelf -h sensor_stats | grep -E 'Type|Entry'"; readelf -h sensor_stats | grep -E 'Type|Entry'; echo; \
  echo "\$ ldd sensor_stats"; ldd sensor_stats 2>&1; } > "$E/binary_info.txt"
echo "Evidence written to $E/"
