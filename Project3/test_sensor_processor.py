#!/usr/bin/env python3
"""Test and demonstration program for the sensor_processor C extension.

Running this file:
  1. prints a demonstration: sample sensor readings are passed to all five
     extension functions and the results are displayed, followed by the
     empty-input and invalid-input conditions and the exceptions they raise;
  2. runs the unittest suite (correctness, edge cases, error handling,
     reference counting and memory-leak checks).

    python3 test_sensor_processor.py            # demo + tests
    python3 test_sensor_processor.py --demo     # demo only
    python3 test_sensor_processor.py --tests    # tests only
    python3 -m unittest test_sensor_processor.TestMovingAverage   # one class
"""
import math
import sys
import tracemalloc
import unittest

try:
    import sensor_processor as sp
except ImportError:
    sys.exit("sensor_processor is not built yet - run:  python3 setup.py build_ext --inplace")


# --------------------------------------------------------------------------- #
#  Sample data: soil temperature readings (deg C) from a smart-farm sensor.
#  Acceptable operating range is 15.0 .. 35.0; 3.2, 48.9 and -1.0 are faulty.
# --------------------------------------------------------------------------- #
SOIL_TEMPERATURE = [21.4, 22.0, 23.1, 3.2, 24.5, 25.8, 48.9, 26.3, 24.9, 23.7, -1.0, 22.6]
SOIL_MOISTURE = (31, 33, 35, 34, 30, 29, 36)     # tuple of ints (% moisture)
OK_MIN, OK_MAX = 15.0, 35.0


def demo():
    line = "=" * 72
    print(line)
    print(f" sensor_processor {sp.__version__} - demonstration")
    print(line)

    print("\n[1] Examples from the assignment brief")
    examples = [
        ("filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)",
         lambda: sp.filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)),
        ("count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)",
         lambda: sp.count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)),
        ("moving_average([10.0, 20.0, 30.0, 40.0], 2)",
         lambda: sp.moving_average([10.0, 20.0, 30.0, 40.0], 2)),
        ("maximum_difference([10.0, 25.0, 40.0, 15.0])",
         lambda: sp.maximum_difference([10.0, 25.0, 40.0, 15.0])),
        ("sensor_summary([10.0, 20.0, 30.0, 40.0])",
         lambda: sp.sensor_summary([10.0, 20.0, 30.0, 40.0])),
    ]
    for text, call in examples:
        print(f"  {text}\n      -> {call()!r}")

    print(f"\n[2] Smart-farm soil temperature sensor (acceptable range {OK_MIN}..{OK_MAX} C)")
    print(f"  readings            : {SOIL_TEMPERATURE}")
    print(f"  filter_readings     : {sp.filter_readings(SOIL_TEMPERATURE, OK_MIN, OK_MAX)}")
    print(f"  count_outliers      : {sp.count_outliers(SOIL_TEMPERATURE, OK_MIN, OK_MAX)}")
    print(f"  moving_average (w=3): {[round(v, 3) for v in sp.moving_average(SOIL_TEMPERATURE, 3)]}")
    print(f"  maximum_difference  : {sp.maximum_difference(SOIL_TEMPERATURE)}")
    summary = sp.sensor_summary(SOIL_TEMPERATURE)
    print(f"  sensor_summary      : {summary}")

    clean = sp.filter_readings(SOIL_TEMPERATURE, OK_MIN, OK_MAX)
    print("  after removing outliers:")
    print(f"    sensor_summary    : {sp.sensor_summary(clean)}")

    print("\n[3] Tuple input containing ints (soil moisture %)")
    print(f"  readings            : {SOIL_MOISTURE}")
    print(f"  moving_average (w=4): {sp.moving_average(SOIL_MOISTURE, 4)}")
    print(f"  sensor_summary      : {sp.sensor_summary(SOIL_MOISTURE)}")

    print("\n[4] Empty input")
    for text, call in [
        ("filter_readings([], 0, 10)", lambda: sp.filter_readings([], 0, 10)),
        ("count_outliers([], 0, 10)", lambda: sp.count_outliers([], 0, 10)),
        ("moving_average([], 1)", lambda: sp.moving_average([], 1)),
        ("maximum_difference([])", lambda: sp.maximum_difference([])),
        ("sensor_summary(())", lambda: sp.sensor_summary(())),
    ]:
        show(text, call)

    print("\n[5] Invalid input conditions")
    for text, call in [
        ("filter_readings('12,15,18', 0, 50)      # string, not list/tuple",
         lambda: sp.filter_readings("12,15,18", 0, 50)),
        ("sensor_summary({1.0, 2.0})              # set is not supported",
         lambda: sp.sensor_summary({1.0, 2.0})),
        ("maximum_difference([12.5, 'error', 9])  # non-numeric element",
         lambda: sp.maximum_difference([12.5, "error", 9])),
        ("count_outliers([1.0, 2.0], 40.0, 20.0)  # minimum > maximum",
         lambda: sp.count_outliers([1.0, 2.0], 40.0, 20.0)),
        ("moving_average([1.0, 2.0, 3.0], 0)      # window < 1",
         lambda: sp.moving_average([1.0, 2.0, 3.0], 0)),
        ("moving_average([1.0, 2.0, 3.0], 5)      # window > len(data)",
         lambda: sp.moving_average([1.0, 2.0, 3.0], 5)),
        ("moving_average([1.0, 2.0, 3.0], 1.5)    # window not an int",
         lambda: sp.moving_average([1.0, 2.0, 3.0], 1.5)),
        ("sensor_summary([20.0, float('nan')])    # NaN reading",
         lambda: sp.sensor_summary([20.0, float("nan")])),
    ]:
        show(text, call)
    print()


def show(text, call):
    try:
        print(f"  {text}\n      -> {call()!r}")
    except Exception as exc:  # demonstration of the raised exception
        print(f"  {text}\n      -> raises {type(exc).__name__}: {exc}")


# --------------------------------------------------------------------------- #
#  Unit tests
# --------------------------------------------------------------------------- #
class TestFilterReadings(unittest.TestCase):
    def test_brief_example(self):
        self.assertEqual(sp.filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0), [25.0, 35.5])

    def test_bounds_are_inclusive(self):
        self.assertEqual(sp.filter_readings([20.0, 30.0, 40.0], 20.0, 40.0), [20.0, 30.0, 40.0])

    def test_tuple_and_int_input_returns_floats(self):
        result = sp.filter_readings((5, 15, 25), 10, 30)
        self.assertEqual(result, [15.0, 25.0])
        self.assertTrue(all(type(v) is float for v in result))

    def test_returns_new_list_and_keeps_order(self):
        data = [3.0, 1.0, 2.0]
        result = sp.filter_readings(data, 0.0, 10.0)
        self.assertEqual(result, data)
        self.assertIsNot(result, data)

    def test_nothing_in_range(self):
        self.assertEqual(sp.filter_readings([1.0, 2.0], 5.0, 6.0), [])

    def test_empty(self):
        self.assertEqual(sp.filter_readings([], 0.0, 1.0), [])


class TestCountOutliers(unittest.TestCase):
    def test_brief_example(self):
        self.assertEqual(sp.count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0), 2)

    def test_boundaries_are_not_outliers(self):
        self.assertEqual(sp.count_outliers([20.0, 40.0, 19.999, 40.001], 20.0, 40.0), 2)

    def test_empty(self):
        self.assertEqual(sp.count_outliers((), 0.0, 1.0), 0)

    def test_consistent_with_filter(self):
        n_in = len(sp.filter_readings(SOIL_TEMPERATURE, OK_MIN, OK_MAX))
        self.assertEqual(n_in + sp.count_outliers(SOIL_TEMPERATURE, OK_MIN, OK_MAX), len(SOIL_TEMPERATURE))


class TestMovingAverage(unittest.TestCase):
    def test_brief_example(self):
        self.assertEqual(sp.moving_average([10.0, 20.0, 30.0, 40.0], 2), [15.0, 25.0, 35.0])

    def test_window_one_is_identity(self):
        self.assertEqual(sp.moving_average((1, 2, 3), 1), [1.0, 2.0, 3.0])

    def test_window_equals_length(self):
        self.assertEqual(sp.moving_average([2.0, 4.0, 6.0], 3), [4.0])

    def test_matches_naive_definition(self):
        data = [float((i * 37) % 101) / 7 for i in range(500)]
        for w in (1, 2, 7, 50, 500):
            expected = [math.fsum(data[i:i + w]) / w for i in range(len(data) - w + 1)]
            got = sp.moving_average(data, w)
            self.assertEqual(len(got), len(expected))
            for a, b in zip(got, expected):
                self.assertAlmostEqual(a, b, places=9)

    def test_no_drift_on_long_series(self):
        # Large values followed by small ones: a naive running sum loses the small values.
        data = [1e15, -1e15] * 50000 + [0.1] * 10
        self.assertEqual(sp.moving_average(data, 10)[-1], 0.1)

    def test_invalid_windows(self):
        with self.assertRaises(ValueError):
            sp.moving_average([1.0, 2.0], 0)
        with self.assertRaises(ValueError):
            sp.moving_average([1.0, 2.0], -3)
        with self.assertRaises(ValueError):
            sp.moving_average([1.0, 2.0], 3)
        with self.assertRaises(TypeError):
            sp.moving_average([1.0, 2.0], 1.0)
        with self.assertRaises(TypeError):
            sp.moving_average([1.0, 2.0], True)
        with self.assertRaises(TypeError):
            sp.moving_average([1.0, 2.0], "2")

    def test_empty(self):
        with self.assertRaises(ValueError):
            sp.moving_average([], 1)


class TestMaximumDifference(unittest.TestCase):
    def test_brief_example(self):
        self.assertEqual(sp.maximum_difference([10.0, 25.0, 40.0, 15.0]), 30.0)

    def test_single_value(self):
        self.assertEqual(sp.maximum_difference([7.5]), 0.0)

    def test_negative_values(self):
        self.assertEqual(sp.maximum_difference((-5, -20, 3)), 23.0)

    def test_empty(self):
        with self.assertRaises(ValueError):
            sp.maximum_difference([])


class TestSensorSummary(unittest.TestCase):
    def test_brief_example(self):
        self.assertEqual(sp.sensor_summary([10.0, 20.0, 30.0, 40.0]),
                         {"samples": 4, "minimum": 10.0, "maximum": 40.0,
                          "average": 25.0, "above_average": 2})

    def test_types(self):
        s = sp.sensor_summary((1, 2, 3))
        self.assertIs(type(s["samples"]), int)
        self.assertIs(type(s["above_average"]), int)
        self.assertIs(type(s["average"]), float)

    def test_above_average_is_strict(self):
        self.assertEqual(sp.sensor_summary([5.0, 5.0, 5.0])["above_average"], 0)

    def test_matches_python(self):
        s = sp.sensor_summary(SOIL_TEMPERATURE)
        mean = math.fsum(SOIL_TEMPERATURE) / len(SOIL_TEMPERATURE)
        self.assertEqual(s["samples"], len(SOIL_TEMPERATURE))
        self.assertEqual(s["minimum"], min(SOIL_TEMPERATURE))
        self.assertEqual(s["maximum"], max(SOIL_TEMPERATURE))
        self.assertAlmostEqual(s["average"], mean, places=12)
        self.assertEqual(s["above_average"], sum(1 for x in SOIL_TEMPERATURE if x > mean))

    def test_empty(self):
        with self.assertRaises(ValueError):
            sp.sensor_summary([])


class TestInputValidation(unittest.TestCase):
    ALL = [
        lambda d: sp.filter_readings(d, 0.0, 1.0),
        lambda d: sp.count_outliers(d, 0.0, 1.0),
        lambda d: sp.moving_average(d, 1),
        sp.maximum_difference,
        sp.sensor_summary,
    ]

    def test_unsupported_container_types(self):
        for bad in ("123", {1: 2}, {1.0, 2.0}, 42, None, (x for x in [1.0]), range(3), b"\x01"):
            for fn in self.ALL:
                with self.assertRaises(TypeError):
                    fn(bad)

    def test_non_numeric_elements(self):
        for bad in ([1.0, "2"], [1.0, None], (1.0, [2.0]), [True, 2.0]):
            for fn in self.ALL:
                with self.assertRaises(TypeError):
                    fn(bad)

    def test_non_finite_elements(self):
        for bad in ([1.0, math.nan], [math.inf], (-math.inf, 1.0)):
            for fn in self.ALL:
                with self.assertRaises(ValueError):
                    fn(bad)

    def test_invalid_range_arguments(self):
        for fn in (sp.filter_readings, sp.count_outliers):
            with self.assertRaises(ValueError):
                fn([1.0], 5.0, 1.0)                 # min > max
            with self.assertRaises(ValueError):
                fn([1.0], math.nan, 1.0)            # non-finite bound
            with self.assertRaises(TypeError):
                fn([1.0], "low", 1.0)               # non-numeric bound
            with self.assertRaises(TypeError):
                fn([1.0], 1.0)                      # missing argument

    def test_input_is_not_modified(self):
        data = [3.0, 1.0, 2.0]
        sp.filter_readings(data, 0, 2)
        sp.moving_average(data, 2)
        sp.sensor_summary(data)
        self.assertEqual(data, [3.0, 1.0, 2.0])


class TestReferenceCountsAndMemory(unittest.TestCase):
    """Checks that the C code neither leaks nor over-releases references."""

    def test_input_refcount_unchanged(self):
        data = [1.0, 2.0, 3.0, 4.0]
        item = data[0]
        before_list, before_item = sys.getrefcount(data), sys.getrefcount(item)
        for _ in range(1000):
            sp.filter_readings(data, 0, 10)
            sp.count_outliers(data, 0, 10)
            sp.moving_average(data, 2)
            sp.maximum_difference(data)
            sp.sensor_summary(data)
        self.assertEqual(sys.getrefcount(data), before_list)
        self.assertEqual(sys.getrefcount(item), before_item)

    def test_refcount_unchanged_on_error_paths(self):
        data = [1.0, 2.0, "x"]
        before = sys.getrefcount(data)
        for _ in range(1000):
            for call in (lambda: sp.sensor_summary(data), lambda: sp.moving_average(data, 2),
                         lambda: sp.filter_readings(data, 0, 5)):
                try:
                    call()
                except TypeError:
                    pass
        self.assertEqual(sys.getrefcount(data), before)

    def test_no_memory_growth(self):
        data = [float(i % 97) for i in range(2000)]

        def workload():
            for _ in range(200):
                sp.filter_readings(data, 10, 50)
                sp.moving_average(data, 16)
                sp.sensor_summary(data)
                try:
                    sp.moving_average(data + ["bad"], 4)    # error path mid-build
                except TypeError:
                    pass

        workload()                                  # warm-up (free lists, caches)
        tracemalloc.start()
        workload()
        first, _ = tracemalloc.get_traced_memory()
        workload()
        second, _ = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        # 800 calls producing ~2000-element lists: a leak of even one object per
        # call would show up as tens of KB. Allow a small tolerance for caches.
        self.assertLess(second - first, 10_000)


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--tests" not in args:
        demo()
        sys.stdout.flush()       # keep demo output ahead of unittest's stderr output
    if "--demo" not in args:
        print("=" * 72)
        print(" Unit tests")
        print("=" * 72, flush=True)
        unittest.main(argv=[sys.argv[0], "-v"], exit=True)
