/*
 * sensor_processor.c - Python C extension for IoT sensor data filtering/analysis
 * ===========================================================================
 * Linux Programming Summative - Project 3
 *
 * Exposes five functions to Python. All numerical work is done in C with
 * `double` arithmetic:
 *
 *   filter_readings(data, minimum, maximum) -> list[float]   O(n)
 *   count_outliers(data, minimum, maximum)  -> int           O(n)
 *   moving_average(data, window)            -> list[float]   O(n)  (not O(n*w))
 *   maximum_difference(data)                -> float         O(n)
 *   sensor_summary(data)                    -> dict          O(n)  (two passes)
 *
 * `data` must be a list or a tuple of real numbers (int or float; bool is
 * rejected). NaN and +/-infinity are rejected because they silently corrupt
 * comparisons, min/max and means.
 *
 * ---------------------------------------------------------------------------
 * How Python objects are accessed and converted
 * ---------------------------------------------------------------------------
 *  - The argument is first checked with PyList_Check / PyTuple_Check, so that
 *    unsupported types (dict, set, str, generators, ...) raise TypeError.
 *  - PySequence_Fast() then gives uniform, O(1) indexed access to the items
 *    of a list or tuple. For these two types it does NOT copy anything: it
 *    simply returns the same object with its reference count incremented.
 *    PySequence_Fast_ITEMS() exposes the underlying PyObject* array, so the
 *    hot loops index a C array directly instead of calling back into Python.
 *  - Each item is converted to a C double by to_double(): a fast path reads
 *    the value straight out of exact float objects (PyFloat_AS_DOUBLE); any
 *    other number (int, or a type implementing __float__) goes through
 *    PyFloat_AsDouble(). Conversion failures become a TypeError naming the
 *    offending index.
 *
 * ---------------------------------------------------------------------------
 * Reference counting and memory management
 * ---------------------------------------------------------------------------
 *  - Every function that obtains the "fast" sequence owns one new reference
 *    to it and releases it with Py_DECREF on every exit path. Each function
 *    uses a single cleanup label so that no path can leak it.
 *  - Items obtained from PySequence_Fast_ITEMS are BORROWED references: they
 *    are kept alive by the sequence, so they are never DECREF'd.
 *  - Result lists are created at their exact final size with PyList_New(n)
 *    and filled with PyList_SET_ITEM, which STEALS the reference to the
 *    element - so new floats are not DECREF'd after insertion.
 *  - PyDict_SetItemString does NOT steal, so each value is DECREF'd after it
 *    has been inserted into the summary dictionary.
 *  - On any error, a partially built result is released with Py_XDECREF and
 *    NULL is returned with the Python exception already set.
 *  - No malloc/PyMem_Malloc is used anywhere: the readings are converted on
 *    the fly while iterating, so no temporary C array of doubles is needed.
 *    The only allocations are the Python objects that are returned.
 *
 * ---------------------------------------------------------------------------
 * Numerical accuracy
 * ---------------------------------------------------------------------------
 *  Long sums of doubles accumulate rounding error, and a sliding-window sum
 *  (add new value, subtract old value) accumulates it at every step. Both
 *  sensor_summary's mean and moving_average therefore use Neumaier's
 *  compensated summation, which tracks the lost low-order bits in a second
 *  variable. Cost: a few extra floating-point operations per element,
 *  still O(n).
 *
 * The GIL is held throughout: the loops read Python objects, so releasing
 * the GIL would not be safe.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>

/* ========================================================================= */
/*  Helpers                                                                  */
/* ========================================================================= */

/*
 * Neumaier compensated summation. `sum` holds the running total and `comp`
 * the accumulated rounding error; the accurate result is sum + comp.
 */
typedef struct {
    double sum;
    double comp;
} CompSum;

static inline void
compsum_add(CompSum *s, double x)
{
    double t = s->sum + x;
    if (fabs(s->sum) >= fabs(x))
        s->comp += (s->sum - t) + x;   /* low-order bits of x were lost */
    else
        s->comp += (x - t) + s->sum;   /* low-order bits of sum were lost */
    s->sum = t;
}

static inline double
compsum_value(const CompSum *s)
{
    return s->sum + s->comp;
}

/*
 * Validate that `data` is a list or tuple and return a NEW reference to a
 * "fast sequence" view of it (no copy for list/tuple). Returns NULL with
 * TypeError set otherwise. The caller must Py_DECREF the result.
 */
static PyObject *
get_sequence(PyObject *data, const char *fname)
{
    if (!PyList_Check(data) && !PyTuple_Check(data)) {
        PyErr_Format(PyExc_TypeError,
                     "%s() expects a list or tuple of numbers, got '%.100s'",
                     fname, Py_TYPE(data)->tp_name);
        return NULL;
    }
    return PySequence_Fast(data, "expected a list or tuple");
}

/*
 * Convert one item (a BORROWED reference) to a finite C double.
 * Returns 0 on success, -1 with an exception set on failure.
 */
static int
to_double(PyObject *item, Py_ssize_t index, const char *fname, double *out)
{
    double value;

    if (PyFloat_CheckExact(item)) {
        value = PyFloat_AS_DOUBLE(item);          /* fast path: no call */
    } else {
        if (PyBool_Check(item) || !PyNumber_Check(item)) {
            PyErr_Format(PyExc_TypeError,
                         "%s(): element %zd is not a number (got '%.100s')",
                         fname, index, Py_TYPE(item)->tp_name);
            return -1;
        }
        value = PyFloat_AsDouble(item);           /* int, __float__, ... */
        if (value == -1.0 && PyErr_Occurred()) {
            /* e.g. an int too large for a double (OverflowError) */
            return -1;
        }
    }
    if (!isfinite(value)) {
        PyErr_Format(PyExc_ValueError,
                     "%s(): element %zd is not a finite number (NaN or infinity)",
                     fname, index);
        return -1;
    }
    *out = value;
    return 0;
}

/* Validate the inclusive range [minimum, maximum]. */
static int
check_range(double minimum, double maximum, const char *fname)
{
    if (!isfinite(minimum) || !isfinite(maximum)) {
        PyErr_Format(PyExc_ValueError,
                     "%s(): minimum and maximum must be finite numbers", fname);
        return -1;
    }
    if (minimum > maximum) {
        /* PyErr_Format has no floating-point conversions (%g is not
           supported), so the message is formatted with snprintf first. */
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s(): minimum (%g) must not be greater than maximum (%g)",
                 fname, minimum, maximum);
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }
    return 0;
}

/* Raise ValueError for functions that are undefined on an empty dataset. */
static int
require_non_empty(Py_ssize_t n, const char *fname)
{
    if (n == 0) {
        PyErr_Format(PyExc_ValueError,
                     "%s(): the dataset is empty (at least one reading is required)",
                     fname);
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/*  1. filter_readings(data, minimum, maximum) -> list                       */
/* ========================================================================= */
/*
 * Returns a new list containing the readings x with minimum <= x <= maximum,
 * in their original order, as floats.
 *
 * Two passes over the input:
 *   pass 1: validate every element and count k = number of readings in range
 *   pass 2: allocate the result list with exactly k slots and fill it
 * Exact sizing avoids the repeated reallocations of PyList_Append.
 * Exact float items are reused (Py_INCREF) instead of allocating a new float
 * object with the same value.
 *
 * Time O(n), extra memory O(1) beyond the k-element result.
 * Empty input -> [] (filtering nothing is well defined).
 */
static PyObject *
sensor_filter_readings(PyObject *self, PyObject *args)
{
    static const char *fname = "filter_readings";
    PyObject *data, *seq = NULL, *result = NULL;
    double minimum, maximum, x;
    (void)self;

    /* "Odd": object + two doubles. 'd' accepts int or float, rejects str. */
    if (!PyArg_ParseTuple(args, "Odd:filter_readings", &data, &minimum, &maximum))
        return NULL;
    if (check_range(minimum, maximum, fname) < 0)
        return NULL;
    if ((seq = get_sequence(data, fname)) == NULL)
        return NULL;

    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);    /* borrowed */

    /* pass 1: validate + count */
    Py_ssize_t k = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto done;
        if (x >= minimum && x <= maximum)
            k++;
    }

    /* pass 2: build the result of exactly k elements */
    if ((result = PyList_New(k)) == NULL)
        goto done;
    Py_ssize_t j = 0;
    for (Py_ssize_t i = 0; i < n && j < k; i++) {
        PyObject *item = items[i];
        x = PyFloat_CheckExact(item) ? PyFloat_AS_DOUBLE(item)
                                     : PyFloat_AsDouble(item);
        if (x == -1.0 && PyErr_Occurred()) {
            Py_CLEAR(result);
            goto done;
        }
        if (x >= minimum && x <= maximum) {
            PyObject *value;
            if (PyFloat_CheckExact(item)) {
                value = Py_NewRef(item);       /* reuse the existing float */
            } else if ((value = PyFloat_FromDouble(x)) == NULL) {
                Py_CLEAR(result);
                goto done;
            }
            PyList_SET_ITEM(result, j++, value);   /* steals `value` */
        }
    }

done:
    Py_DECREF(seq);
    return result;          /* NULL if an exception is set */
}

/* ========================================================================= */
/*  2. count_outliers(data, minimum, maximum) -> int                         */
/* ========================================================================= */
/*
 * Number of readings x with x < minimum or x > maximum.
 * Single pass, time O(n), extra memory O(1). Empty input -> 0.
 */
static PyObject *
sensor_count_outliers(PyObject *self, PyObject *args)
{
    static const char *fname = "count_outliers";
    PyObject *data, *seq, *result = NULL;
    double minimum, maximum, x;
    (void)self;

    if (!PyArg_ParseTuple(args, "Odd:count_outliers", &data, &minimum, &maximum))
        return NULL;
    if (check_range(minimum, maximum, fname) < 0)
        return NULL;
    if ((seq = get_sequence(data, fname)) == NULL)
        return NULL;

    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);
    Py_ssize_t outliers = 0;

    for (Py_ssize_t i = 0; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto done;
        if (x < minimum || x > maximum)
            outliers++;
    }
    result = PyLong_FromSsize_t(outliers);

done:
    Py_DECREF(seq);
    return result;
}

/* ========================================================================= */
/*  3. moving_average(data, window) -> list                                  */
/* ========================================================================= */
/*
 * Simple moving average with window size w over n readings:
 *
 *      avg_i = (x_i + x_{i+1} + ... + x_{i+w-1}) / w,   i = 0 .. n-w
 *
 * giving n - w + 1 values. Instead of re-adding w values for every output
 * (O(n*w)), a sliding window sum is kept:
 *
 *      S_0 = x_0 + ... + x_{w-1}
 *      S_i = S_{i-1} + x_{i+w-1} - x_{i-1}
 *
 * so each step costs O(1) and the whole function is O(n). The window sum uses
 * compensated summation so that rounding error does not drift over long
 * series. Result list allocated once at its exact size (n - w + 1).
 *
 * Errors: empty data, window not an int, window < 1 or window > len(data).
 */
static PyObject *
sensor_moving_average(PyObject *self, PyObject *args)
{
    static const char *fname = "moving_average";
    PyObject *data, *window_obj, *seq = NULL, *result = NULL;
    double x, old;
    (void)self;

    if (!PyArg_ParseTuple(args, "OO:moving_average", &data, &window_obj))
        return NULL;

    /* window must be a genuine int (bool and float are rejected) */
    if (!PyLong_Check(window_obj) || PyBool_Check(window_obj)) {
        PyErr_Format(PyExc_TypeError,
                     "%s(): window must be an int, got '%.100s'",
                     fname, Py_TYPE(window_obj)->tp_name);
        return NULL;
    }
    Py_ssize_t w = PyLong_AsSsize_t(window_obj);
    if (w == -1 && PyErr_Occurred())
        return NULL;                          /* OverflowError for huge ints */

    if ((seq = get_sequence(data, fname)) == NULL)
        return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);

    if (require_non_empty(n, fname) < 0)
        goto done;
    if (w < 1 || w > n) {
        PyErr_Format(PyExc_ValueError,
                     "%s(): window must satisfy 1 <= window <= len(data) "
                     "(got window=%zd, len(data)=%zd)", fname, w, n);
        goto done;
    }

    if ((result = PyList_New(n - w + 1)) == NULL)
        goto done;

    CompSum window_sum = {0.0, 0.0};
    for (Py_ssize_t i = 0; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto fail;
        compsum_add(&window_sum, x);                  /* x enters window */
        if (i >= w) {                                 /* x_{i-w} leaves */
            if (to_double(items[i - w], i - w, fname, &old) < 0)
                goto fail;
            compsum_add(&window_sum, -old);
        }
        if (i >= w - 1) {                             /* window is full */
            PyObject *avg = PyFloat_FromDouble(compsum_value(&window_sum) / (double)w);
            if (avg == NULL)
                goto fail;
            PyList_SET_ITEM(result, i - w + 1, avg);  /* steals `avg` */
        }
    }
    goto done;

fail:
    Py_CLEAR(result);            /* frees the list and any items already set */
done:
    Py_DECREF(seq);
    return result;
}

/* ========================================================================= */
/*  4. maximum_difference(data) -> float                                     */
/* ========================================================================= */
/*
 * max(data) - min(data), found in a single pass that tracks both extremes.
 * Time O(n), extra memory O(1). A single reading gives 0.0.
 */
static PyObject *
sensor_maximum_difference(PyObject *self, PyObject *data)
{
    static const char *fname = "maximum_difference";
    PyObject *seq, *result = NULL;
    double x, lo, hi;
    (void)self;

    if ((seq = get_sequence(data, fname)) == NULL)
        return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);

    if (require_non_empty(n, fname) < 0)
        goto done;
    if (to_double(items[0], 0, fname, &x) < 0)
        goto done;
    lo = hi = x;
    for (Py_ssize_t i = 1; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto done;
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    result = PyFloat_FromDouble(hi - lo);

done:
    Py_DECREF(seq);
    return result;
}

/* ========================================================================= */
/*  5. sensor_summary(data) -> dict                                          */
/* ========================================================================= */
/*
 * Returns {"samples": n, "minimum": min, "maximum": max,
 *          "average": mean, "above_average": count(x > mean)}
 *
 *   pass 1: n, min, max and the (compensated) sum  ->  mean = sum / n
 *   pass 2: count readings strictly greater than the mean
 *
 * The second pass is unavoidable without storing the data, because the mean
 * is only known after every value has been seen. Time O(n), memory O(1).
 */
static PyObject *
sensor_sensor_summary(PyObject *self, PyObject *data)
{
    static const char *fname = "sensor_summary";
    PyObject *seq, *result = NULL, *value = NULL;
    double x, lo, hi;
    (void)self;

    if ((seq = get_sequence(data, fname)) == NULL)
        return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);

    if (require_non_empty(n, fname) < 0)
        goto done;

    /* pass 1 */
    CompSum total = {0.0, 0.0};
    if (to_double(items[0], 0, fname, &x) < 0)
        goto done;
    lo = hi = x;
    compsum_add(&total, x);
    for (Py_ssize_t i = 1; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto done;
        if (x < lo) lo = x;
        if (x > hi) hi = x;
        compsum_add(&total, x);
    }
    double mean = compsum_value(&total) / (double)n;

    /* pass 2 (all items were validated in pass 1) */
    Py_ssize_t above = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (to_double(items[i], i, fname, &x) < 0)
            goto done;
        if (x > mean)
            above++;
    }

    /* Build the dict. PyDict_SetItemString does NOT steal the value
       reference, so each value is released right after insertion. */
    if ((result = PyDict_New()) == NULL)
        goto done;

#define SET_ITEM(key, expr)                                            \
    do {                                                               \
        if ((value = (expr)) == NULL ||                                \
            PyDict_SetItemString(result, (key), value) < 0) {          \
            Py_XDECREF(value);                                         \
            Py_CLEAR(result);                                          \
            goto done;                                                 \
        }                                                              \
        Py_DECREF(value);                                              \
    } while (0)

    SET_ITEM("samples",       PyLong_FromSsize_t(n));
    SET_ITEM("minimum",       PyFloat_FromDouble(lo));
    SET_ITEM("maximum",       PyFloat_FromDouble(hi));
    SET_ITEM("average",       PyFloat_FromDouble(mean));
    SET_ITEM("above_average", PyLong_FromSsize_t(above));
#undef SET_ITEM

done:
    Py_DECREF(seq);
    return result;
}

/* ========================================================================= */
/*  Method table, module definition and initialisation                       */
/* ========================================================================= */

PyDoc_STRVAR(filter_doc,
"filter_readings(data, minimum, maximum) -> list\n\n"
"Return the readings within the inclusive range [minimum, maximum], in order.\n"
"Raises TypeError for unsupported input and ValueError if minimum > maximum.");

PyDoc_STRVAR(count_doc,
"count_outliers(data, minimum, maximum) -> int\n\n"
"Return how many readings lie outside the inclusive range [minimum, maximum].");

PyDoc_STRVAR(moving_doc,
"moving_average(data, window) -> list\n\n"
"Return the simple moving averages of `data` for the given window size\n"
"(len(data) - window + 1 values). Requires 1 <= window <= len(data).");

PyDoc_STRVAR(maxdiff_doc,
"maximum_difference(data) -> float\n\n"
"Return max(data) - min(data). Raises ValueError for an empty dataset.");

PyDoc_STRVAR(summary_doc,
"sensor_summary(data) -> dict\n\n"
"Return {'samples', 'minimum', 'maximum', 'average', 'above_average'}.\n"
"Raises ValueError for an empty dataset.");

PyDoc_STRVAR(module_doc,
"Native C routines for filtering and analysing IoT sensor readings.\n\n"
"All functions accept a list or tuple of real numbers (int or float).");

/* Method table: Python name, C function, calling convention, docstring.
   Single-argument functions use METH_O (the argument is passed directly,
   no tuple to unpack); the others use METH_VARARGS + PyArg_ParseTuple. */
static PyMethodDef SensorMethods[] = {
    {"filter_readings",    sensor_filter_readings,    METH_VARARGS, filter_doc},
    {"count_outliers",     sensor_count_outliers,     METH_VARARGS, count_doc},
    {"moving_average",     sensor_moving_average,     METH_VARARGS, moving_doc},
    {"maximum_difference", sensor_maximum_difference, METH_O,       maxdiff_doc},
    {"sensor_summary",     sensor_sensor_summary,     METH_O,       summary_doc},
    {NULL, NULL, 0, NULL}                       /* sentinel */
};

/* Module definition. m_size = -1: the module keeps no per-module state.
   Designated initialisers leave the remaining fields (slots, GC hooks) NULL. */
static struct PyModuleDef sensor_module = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "sensor_processor",
    .m_doc     = module_doc,
    .m_size    = -1,
    .m_methods = SensorMethods,
};

/* Initialisation function: called by the interpreter on `import
   sensor_processor`. Its name must be PyInit_<module name>. */
PyMODINIT_FUNC
PyInit_sensor_processor(void)
{
    PyObject *module = PyModule_Create(&sensor_module);
    if (module == NULL)
        return NULL;
    if (PyModule_AddStringConstant(module, "__version__", "1.0.0") < 0) {
        Py_DECREF(module);
        return NULL;
    }
    return module;
}
