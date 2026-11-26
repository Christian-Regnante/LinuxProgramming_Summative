# Linux Programming — Summative Assessment

This repository holds five independent Linux systems-programming projects. Each one lives in its own directory and has its own source code, build files, tests, evidence, a detailed `README.md` and an analytical report in `.docx` format.

| # | Project | Language / key techniques | Report |
|---|---------|---------------------------|--------|
| 1 | [Investigating and Reverse Engineering an ELF Executable](Project1/README.md) | C · `readelf` · `objdump` · `strace` · `gdb` · PLT/GOT lazy binding | [`Project1_ELF_Analysis_Report.docx`](Project1/Project1_ELF_Analysis_Report.pdf) |
| 2 | [Assembly-Based Sensor Data Validation and Analysis](Project2/README.md) | x86-64 NASM · raw Linux syscalls (no libc) · chunked streaming parser | [`Project2_Report.docx`](Project2/Project2_Report.pdf) |
| 3 | [Python C Extension for IoT Sensor Data Filtering](Project3/README.md) | C + Python C API · reference counting · setuptools · benchmarks | [`Project3_Report.docx`](Project3/Project3_Report.pdf) |
| 4 | [Multithreaded Library Book Reservation System](Project4/README.md) | C · POSIX threads · mutex + condition variables · circular buffer | [`Project4_Report.docx`](Project4/Project4_Report.pdf) |
| 5 | [Concurrent TCP University Event Registration System](Project5/README.md) | C · TCP sockets · thread per client · race-free registration | [`Project5_Report.docx`](Project5/Project5_Report.pdf) |

---

## Environment

Everything was developed and tested on **Linux x86-64** (kernel 7.2) with these tools:

| Tool | Version | Used by |
|------|---------|---------|
| GCC | 16.2.1 | 1, 3, 4, 5 |
| GNU Binutils (`readelf`, `objdump`, `ld`, `strip`) | 2.47 | 1, 2 |
| NASM | 3.02 | 2 |
| strace | 7.2 | 1, 2, 4 |
| GDB | 18.1 | 1, 2 |
| Python (with headers) + setuptools | 3.14.8 / 84.2.1 | 3, plus the test scripts of 2 and 5 |
| ThreadSanitizer / AddressSanitizer (bundled with GCC) | — | 3, 4, 5 |

To install the essentials on Debian/Ubuntu:
`sudo apt install build-essential nasm gdb strace python3-dev python3-setuptools`

## Quick start

Run each block from the repository root. Every project's README gives full details of compilation, execution, inputs and expected output.

```bash
# Project 1 — build the stripped and debug ELF files, run them, regenerate the analysis evidence
cd Project1 && make && ./program && make analysis && cd ..

# Project 2 — assemble and run the sensor statistics program, then run the tests
cd Project2 && make && ./sensor_stats && make test && cd ..

# Project 3 — build the C extension, run the demo and the 34 unit tests, then the benchmark
cd Project3 && python3 setup.py build_ext --inplace && python3 test_sensor_processor.py && python3 benchmark.py && cd ..

# Project 4 — the reservation simulation (about 46 s at real timings; --time-scale=0.1 for about 5 s)
cd Project4 && make && ./library_reservation --time-scale=0.1 && cd ..

# Project 5 — the server in one terminal, then one or more clients in other terminals
cd Project5 && make && ./server            # terminal 1
cd Project5 && ./client                    # terminals 2..N (student IDs S1001..S1020)
cd Project5 && make test && make race      # automated protocol tests and the last-seat race test
```

## Evidence and reproducibility

Every terminal output, trace, test result and benchmark figure in the READMEs and reports comes from a real run on the machine described above. Each project can regenerate its evidence:

| Project | Command | What it captures |
|---------|---------|------------------|
| 1 | `make analysis` | `readelf`/`objdump` output, strace traces, GDB sessions (including live lazy binding) |
| 2 | `make evidence` | Terminal runs for every test case, strace, a GDB register trace, test and fuzz results |
| 3 | `make evidence` | Build log, demo output, unit tests, benchmark, sanitizer run |
| 4 | `make evidence` | The full 46 s run, a TSan run plus a negative control, the stress test, an strace summary |
| 5 | `make evidence` | Protocol tests, race test plus a negative control, the TSan workload, Part D demo transcripts |


## Highlights

- **Project 1:** all four functions are reconstructed from the *stripped* binary and then verified against the debug symbols. Lazy binding is observed live, with the `printf` GOT slot changing from the PLT stub to libc. The project also explains why `printf` makes 15 `write()` calls on a terminal but only 1 on a pipe.
- **Project 2:** the program streams the file through a 4 KiB buffer and handles records and CRLF pairs that are split across `read()` calls. It passes 17 hand-verified tests plus 500 random files fuzzed against an independent reference. The README documents an arithmetic error in the brief's sample output.
- **Project 3:** the extension is 3–7× faster than idiomatic Python and 23× faster than a naive moving average. Compensated summation gives exact results where a naive sum is 25 % off. Reference counts were verified with refcount tests, tracemalloc and an ASan/UBSan build.
- **Project 4:** the queue never exceeds 5 items. A clean ThreadSanitizer run is backed by a negative control showing that TSan does catch an injected race, and 40 randomised stress runs all pass.
- **Project 5:** in 10 rounds, 10 real clients raced for 1 seat and there was always exactly one winner, whereas a check-then-act copy of the server over-books to 13/4. ThreadSanitizer found a signal-flag race, which was fixed with a C11 atomic.
