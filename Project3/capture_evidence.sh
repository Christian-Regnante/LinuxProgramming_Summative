#!/usr/bin/env bash
# Regenerates the evidence quoted in README.md and Project3_Report.docx.
set -u
cd "$(dirname "$0")" || exit 1
E=evidence; mkdir -p "$E"
PY=${PYTHON:-python3}

{ echo "\$ $PY --version; gcc --version | sed -n 1p; $PY -c 'import setuptools; print(\"setuptools\", setuptools.__version__)'"
  $PY --version; gcc --version | sed -n 1p; $PY -c 'import setuptools; print("setuptools", setuptools.__version__)'
  echo; echo "\$ rm -rf build sensor_processor*.so && $PY setup.py build_ext --inplace"
  rm -rf build sensor_processor*.so
  $PY setup.py build_ext --inplace 2>&1 | sed -E 's/^(gcc .{70}).*( -c sensor_processor\.c .*)$/\1 ...\2/'
  echo; echo "\$ ls -l sensor_processor*.so"; ls -l sensor_processor*.so | awk '{print $1, $5, $NF}'
  echo; echo "Compiler warnings: $($PY setup.py build_ext --inplace --force 2>&1 | grep -ci warning)"
} > "$E/build_log.txt" 2>&1

{ echo "\$ nm -D --defined-only sensor_processor*.so   # exported symbols"
  nm -D --defined-only sensor_processor*.so
  echo; echo "\$ $PY -c 'import sensor_processor as sp; print(sp.__file__); print(sp.__doc__); print(sp.moving_average.__doc__)'"
  $PY -c 'import sensor_processor as sp, os; print(os.path.basename(sp.__file__)); print(sp.__doc__); print(); print(sp.moving_average.__doc__)'
} > "$E/module_info.txt" 2>&1

$PY test_sensor_processor.py --demo  > "$E/demo_output.txt" 2>&1
$PY test_sensor_processor.py --tests > "$E/unit_tests.txt" 2>&1
$PY benchmark.py                     > "$E/benchmark.txt" 2>&1
./sanitizer_check.sh                 > "$E/sanitizer_check.txt" 2>&1
echo "Evidence written to $E/"
