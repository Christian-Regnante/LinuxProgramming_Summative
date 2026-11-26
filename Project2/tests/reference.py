"""Independent Python reference implementation of the validation rules.

Used only for testing: it is written in a completely different style from the
assembly (split into lines + regular expression) so that agreement between the
two is strong evidence that the assembly implements the rules correctly.
"""
import re

VALID_RE = re.compile(r"-?[0-9]+")


def analyse(data: bytes):
    if not data:
        records = []
    else:
        records = data.split(b"\n")
        if data.endswith(b"\n"):
            records.pop()                     # trailing newline: no extra record
    total = valid = invalid = total_sum = 0
    lo = hi = None
    for rec in records:
        if rec.endswith(b"\r"):               # CRLF -> strip the CR
            rec = rec[:-1]
        text = rec.decode("latin-1")
        if VALID_RE.fullmatch(text) and 0 <= int(text) <= 100:
            v = int(text)
            valid += 1
            total_sum += v
            lo = v if lo is None else min(lo, v)
            hi = v if hi is None else max(hi, v)
        else:
            invalid += 1
        total += 1
    fmt = lambda x: "N/A" if x is None else str(x)
    return (f"Total records: {total}\nValid measurements: {valid}\n"
            f"Invalid measurements: {invalid}\nSum of valid measurements: {total_sum}\n"
            f"Minimum measurement: {fmt(lo)}\nMaximum measurement: {fmt(hi)}\n")
