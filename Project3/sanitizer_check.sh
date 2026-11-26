#!/usr/bin/env bash
# Builds an AddressSanitizer + UndefinedBehaviorSanitizer copy of the extension
# in build/asan/ and runs the full unit-test suite against it. Any out-of-bounds
# access, use-after-free or undefined behaviour in sensor_processor.c aborts the
# run with a report. (Leak detection is left to the tracemalloc/refcount tests:
# LeakSanitizer would also report the interpreter's own intentional allocations.)
set -euo pipefail
cd "$(dirname "$0")"
OUT=build/asan; mkdir -p "$OUT"
SO="$OUT/sensor_processor$(python3-config --extension-suffix)"
gcc -shared -fPIC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -std=c11 $(python3-config --includes) sensor_processor.c -o "$SO"
cp test_sensor_processor.py "$OUT/"
ABS_SO="$(realpath "$SO")"
cd "$OUT"
export LD_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
LOADED="$(python3 -c 'import sensor_processor, os; print(os.path.realpath(sensor_processor.__file__))')"
[[ "$LOADED" == "$ABS_SO" ]] || { echo "ERROR: tests did not load the instrumented module"; exit 1; }
echo "Loaded instrumented module: $SO"
echo "ASan/UBSan runtime functions referenced by it: $(nm -D "$ABS_SO" | grep -cE ' U __(asan|ubsan)_')"
python3 test_sensor_processor.py --tests 2>&1 | tail -4
echo "Sanitizer run finished with no AddressSanitizer/UBSan reports."
