# Project 3 — Python C Extension for IoT Sensor Data Filtering and Analysis

`sensor_processor` is a native extension module written in C against the Python C API. It filters and analyses sensor readings from a smart-agriculture platform, and it does all of the numerical work in C using `double`.

The detailed documentation, analysis and evidence are in **`Project3_Report.docx`**.

---

## 1. Files

| Path | Description |
|------|-------------|
| `sensor_processor.c` | Extension source: the 5 functions, the method table, the module definition and `PyInit_sensor_processor`. It is fully commented (object conversion, reference counting, formulas, complexity) |
| `setup.py` | setuptools build configuration (**required deliverable**) |
| `test_sensor_processor.py` | Test program: a demonstration of all 5 functions with sample data, empty input and invalid input, plus 34 unit tests |
| `benchmark.py` | Timings of the C functions against equivalent pure-Python code on 1,000,000 readings |
| `sanitizer_check.sh` | Builds an AddressSanitizer + UBSan copy of the extension and runs the whole test suite against it |
| `capture_evidence.sh` | Regenerates everything in `evidence/` |
| `Makefile` | Shortcuts: `make`, `make test`, `make demo`, `make bench`, `make sanitize`, `make evidence`, `make clean` |
| `evidence/` | Captured build log, demo output, test results, benchmark and sanitizer run |
| `report/` | Generator for the `.docx` report |

## 2. Requirements

| Item | Version used |
|------|--------------|
| Python (with development headers, `Python.h`) | 3.14.8 (any CPython 3.8 or later works) |
| setuptools | 84.2.1 |
| C compiler | GCC 16.2.1 |
| OS | Linux x86-64 |

The Python headers must be installed. On Debian/Ubuntu, run `sudo apt install python3-dev python3-setuptools build-essential`. On Arch the headers already come with the `python` package; also install `python-setuptools`.

## 3. Build

```bash
cd Project3
python3 setup.py build_ext --inplace        # or: make
```

This compiles `sensor_processor.c` with `-O2 -Wall -Wextra -std=c11`, with zero warnings. It creates `sensor_processor.cpython-314-x86_64-linux-gnu.so` in the project directory, so `import sensor_processor` works from there.

To install it into a virtual environment instead:

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install .                 # uses setup.py
```

`make clean` removes the build artefacts.

## 4. Usage

```python
>>> import sensor_processor as sp
>>> sp.filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)
[25.0, 35.5]
>>> sp.count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)
2
>>> sp.moving_average([10.0, 20.0, 30.0, 40.0], 2)
[15.0, 25.0, 35.0]
>>> sp.maximum_difference([10.0, 25.0, 40.0, 15.0])
30.0
>>> sp.sensor_summary([10.0, 20.0, 30.0, 40.0])
{'samples': 4, 'minimum': 10.0, 'maximum': 40.0, 'average': 25.0, 'above_average': 2}
```

All five outputs above match the brief exactly.

### Function reference

| Function | Returns | Formula / behaviour | Time | Extra memory |
|----------|---------|---------------------|------|--------------|
| `filter_readings(data, minimum, maximum)` | `list[float]` | Readings with `minimum ≤ x ≤ maximum`, in their original order | O(n) | O(1), plus the result |
| `count_outliers(data, minimum, maximum)` | `int` | Count of readings with `x < minimum` or `x > maximum` | O(n) | O(1) |
| `moving_average(data, window)` | `list[float]` | `avgᵢ = (xᵢ + … + xᵢ₊w₋₁) / w` for i = 0…n−w, using a sliding sum `Sᵢ = Sᵢ₋₁ + xᵢ₊w₋₁ − xᵢ₋₁` | **O(n)**, not O(n·w) | O(1), plus the result |
| `maximum_difference(data)` | `float` | `max(x) − min(x)`, found in a single pass | O(n) | O(1) |
| `sensor_summary(data)` | `dict` | samples `n`; minimum; maximum; average `Σx / n`; above_average = count of `x > mean` | O(n), two passes | O(1) |

### Input rules and errors

| Situation | Behaviour |
|-----------|-----------|
| `data` is a list or tuple of `int`/`float` | Accepted. Ints are converted to double |
| `data` is any other type (str, dict, set, generator, range, None, …) | `TypeError` |
| An element is not a number (e.g. `"x"`, `None`, `True`) | `TypeError` that names the element index |
| An element is NaN or ±infinity | `ValueError`, because it would silently corrupt min/max/mean |
| Empty `data` | `filter_readings` → `[]`, `count_outliers` → `0` (both well defined). `moving_average`, `maximum_difference` and `sensor_summary` raise `ValueError` (undefined, as with Python's `max([])`) |
| `minimum > maximum`, or a non-finite bound | `ValueError`. A non-numeric bound raises `TypeError` |
| `window` is not an `int` (float, bool, str) | `TypeError` |
| `window < 1` or `window > len(data)` | `ValueError` |

## 5. Running the test program

```bash
python3 test_sensor_processor.py            # demonstration + 34 unit tests
python3 test_sensor_processor.py --demo     # demonstration only
python3 test_sensor_processor.py --tests    # unit tests only
python3 -m unittest -v test_sensor_processor.TestMovingAverage   # a single test class
```

The demonstration (`evidence/demo_output.txt`) prints the following:
1. the brief's examples;
2. a smart-farm soil-temperature series with three faulty readings, analysed before and after outlier removal;
3. tuple input with integer elements;
4. empty input for all five functions;
5. eight invalid-input conditions with the exceptions they raise.

The unit tests cover:
- the brief's examples;
- inclusive boundaries and order preservation;
- moving averages checked against an independent `math.fsum` definition for several window sizes;
- the absence of numerical drift;
- every error condition;
- **reference counting**: the refcounts of the input list and its items are unchanged after 1000 calls, on both success and error paths;
- **memory**: tracemalloc shows no growth over 800 calls, including error paths that fail while a result list is half built.

## 6. Memory-safety check

```bash
make sanitize     # or ./sanitizer_check.sh
```

This builds an AddressSanitizer + UndefinedBehaviorSanitizer copy of the module in `build/asan/` and runs all 34 tests against it. The result is no reports (`evidence/sanitizer_check.txt`). Valgrind could not be used on this machine because the distribution's dynamic loader is stripped of the symbols Valgrind needs.

## 7. Performance

`python3 benchmark.py` times each function on 1,000,000 Gaussian readings. It compares against *idiomatic* Python (comprehensions, plus the built-in `min`, `max` and `math.fsum`, which are themselves written in C). The numbers are the best of 5 runs, from `evidence/benchmark.txt`:

| Function | C extension | Pure Python | Speed-up |
|----------|------------:|------------:|---------:|
| filter_readings | 23.12 ms | 74.31 ms | 3.2× |
| count_outliers | 9.63 ms | 52.22 ms | 5.4× |
| moving_average (w=50) | 59.59 ms | 199.05 ms | 3.3× |
| maximum_difference | 5.93 ms | 27.72 ms | 4.7× |
| sensor_summary | 13.84 ms | 96.16 ms | 6.9× |
| moving_average vs naive O(n·w) Python (100k readings) | 4.17 ms | 96.87 ms | 23× |

The C code is faster for four reasons:
- It does no bytecode dispatch for each element.
- Its comparisons and sums work on unboxed `double`s instead of creating temporary `float` objects.
- It reads the items through a direct `PyObject*` array (`PySequence_Fast_ITEMS`).
- It computes several statistics in a single pass.

The functions that return large lists (`filter_readings`, `moving_average`) gain less, because most of their time goes into allocating up to a million result `float` objects. That cost is the same in C and in Python.

## 8. Implementation highlights

- **Python to C conversion:** a list or tuple check comes first, then `PySequence_Fast` (no copy for lists and tuples) and `PySequence_Fast_ITEMS`. There is a fast path for exact floats (`PyFloat_AS_DOUBLE`); everything else goes through `PyFloat_AsDouble`.
- **References:**
  - The fast sequence is released by `Py_DECREF` on every path, through a single cleanup label.
  - Items are borrowed references.
  - `PyList_SET_ITEM` steals the reference to each new float.
  - `PyDict_SetItemString` does not steal, so each value is `DECREF`'d after insertion.
  - A partially built result is released with `Py_CLEAR` on error.
- **No `malloc`:** the values are converted on the fly. Result lists are allocated once, at their exact final size.
- **Accuracy:** sums use Neumaier compensated summation. After 100,000 alternating ±10¹⁵ values, a window of 0.1s still averages to exactly 0.1.
- **Calling conventions:** single-argument functions use `METH_O`, which avoids building an argument tuple. The others use `METH_VARARGS` with `PyArg_ParseTuple`.

## 9. Rebuilding the report

```bash
make evidence
cd report && npm install && node build_report.js     # writes ../Project3_Report.docx
```
