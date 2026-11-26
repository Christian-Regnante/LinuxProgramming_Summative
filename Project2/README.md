# Project 2 — Assembly-Based Sensor Data Validation and Analysis

An x86-64 assembly program (NASM syntax) that reads `sensor_readings.txt` and validates each measurement, one per line. It then prints six statistics: total records, valid, invalid, sum, minimum and maximum. It calls no C library: all file handling and console output go straight to the Linux kernel through the `syscall` instruction.

The detailed documentation and execution evidence are in **`Project2_Report.docx`**.

---

## 1. Files

| Path | Description |
|------|-------------|
| `sensor_stats.asm` | The assembly source, fully commented |
| `Makefile` | Assemble, link, run, test, fuzz and capture evidence |
| `sensor_readings.txt` | Main test input: valid, invalid, out-of-range and empty records |
| `tests/cases/*.txt` | 14 targeted test inputs covering LF, CRLF, empty lines, an empty file, buffer boundaries and more |
| `tests/expected/*.out` | Expected output for each case, **derived by hand** from the rules |
| `tests/run_tests.sh` | Runs every case, plus the two error paths, and compares output and exit status |
| `tests/reference.py`, `tests/fuzz.py` | An independent Python implementation of the rules and a randomised differential test |
| `tests/gdb_registers.gdb` | GDB script that shows the parser registers and memory counters for each record |
| `tests/capture_evidence.sh` | Regenerates everything in `evidence/` |
| `evidence/` | Captured terminal runs, strace traces, test and fuzz results |
| `report/build_report.js` | Generates the `.docx` report |

## 2. Environment

| Item | Used for development |
|------|----------------------|
| Assembler | **NASM 3.02** (`-f elf64`) |
| Linker | GNU `ld` 2.47 (Binutils) |
| Architecture / OS | **x86-64 Linux** (kernel 7.2), System V syscall ABI |
| Dependencies | None at run time. The binary is a static ELF that does not use libc |

Install NASM with `sudo apt install nasm` on Debian/Ubuntu or `sudo pacman -S nasm` on Arch.

## 3. Build

```bash
cd Project2
make
```

`make` runs these commands:

```bash
nasm -f elf64 -g -F dwarf sensor_stats.asm -o sensor_stats.o   # assemble (with DWARF debug info)
ld -o sensor_stats sensor_stats.o                             # link (no libc, entry = _start)
```

Both commands complete with zero warnings. `make clean` removes the object file and the binary.

## 4. Run

```bash
./sensor_stats                      # reads ./sensor_readings.txt (the required file name)
./sensor_stats path/to/other.txt    # optional: analyse a different file
```

| Exit status | Meaning |
|-------------|---------|
| `0` | Success |
| `1` | The input file could not be opened (the error goes to stderr) |
| `2` | `read()` failed, for example because the path is a directory (the error goes to stderr) |

## 5. Input format and validation rules

Each line holds one record. The rules are:

| Input line | Result | Reason |
|------------|--------|--------|
| `0` … `100` | **valid** | An integer within 0–100 inclusive |
| `007` | valid (7) | Leading zeros are allowed |
| `-0` | valid (0) | Equal to zero, so it is in range |
| `-5`, `101`, `99999999999999999999` | invalid | Numeric but out of range. Overflow is prevented because accumulation stops above 100 |
| *(empty line)* | invalid | Empty records are invalid, as the task requires |
| `invalid`, `abc12`, `12a`, `23.5`, `+5`, `-`, `5-3` | invalid | Non-numeric characters, or a misplaced sign |
| ` 5`, `5 ` | invalid | Spaces are not allowed in a record (strict format) |
| `4\r5` | invalid | A stray carriage return inside a record |

The parser handles line endings as follows:
- **LF** (`\n`) and **CRLF** (`\r\n`) are both accepted, and they can be mixed within one file.
- If the last line has no trailing newline, it is still counted.
- A newline at the very end of the file does not create an extra empty record.
- The file is read in 4096-byte chunks. Records and CRLF pairs that cross a chunk boundary are handled correctly, so a file of any size works.

## 6. Expected output

For the provided `sensor_readings.txt`, which contains `25, 30, invalid, 18, 42, -5, (empty), 100, 0, 101, 23.5, 76, (empty), abc12, 64`:

```
$ ./sensor_stats
Total records: 15
Valid measurements: 8
Invalid measurements: 7
Sum of valid measurements: 355
Minimum measurement: 0
Maximum measurement: 100
```

When no record is valid, the minimum and maximum are reported as `N/A` and the sum is 0:

```
$ ./sensor_stats tests/cases/05_no_valid_values.txt
Total records: 4
Valid measurements: 0
Invalid measurements: 4
Sum of valid measurements: 0
Minimum measurement: N/A
Maximum measurement: N/A
```

The error paths look like this:

```
$ ./sensor_stats missing.txt
Error: cannot open file 'missing.txt' (errno 2: No such file or directory)        # exit 1
$ ./sensor_stats tests/
Error: failed while reading file 'tests/' (errno 21: Is a directory)              # exit 2
```

### ⚠ Note on the example in the assignment brief

The brief's example input is `25, 30, invalid, 18, 42, -5`. For it, the brief prints *Sum 70, Max 30*. That cannot be right: 25, 30, 18 and 42 are all within 0–100, so the correct results are **sum = 115, min = 18, max = 42**. The valid and invalid counts in the brief (4 and 2) are consistent with this. The program prints the mathematically correct values:

```
$ ./sensor_stats tests/cases/01_task_example_lf.txt
Total records: 6
Valid measurements: 4
Invalid measurements: 2
Sum of valid measurements: 115
Minimum measurement: 18
Maximum measurement: 42
```

## 7. Testing

```bash
make test     # 14 data cases + the default file + 2 error cases → expects "17 passed, 0 failed"
make fuzz     # 500 random files (89 larger than the read buffer) compared with tests/reference.py
make evidence # regenerate evidence/ (terminal runs, strace, gdb, tests, fuzz)
```

| Case | What it proves |
|------|----------------|
| `01` / `02` | The task example with LF and with CRLF endings gives identical results |
| `03` | Mixed LF/CRLF in one file, including an empty CRLF line |
| `04`, `10` | Empty lines count as invalid records |
| `05`, `06` | No valid values, and an empty file → `N/A` for min/max |
| `07`, `11` | The last line is counted even without a trailing newline |
| `08` | Boundaries (0, 100, 101, -0, 007) and malformed numbers |
| `09` | A stray `\r` inside a record |
| `12` | A 9,800-byte file read in 3 chunks (2020 valid, sum 101000) |
| `13`, `14` | A valid record, and a `\r`\|`\n` pair, split exactly across the 4096-byte read boundary |
| errors | A missing file (exit 1) and a read failure on a directory (exit 2) |

## 8. How the program works

```
_start ── choose path (argv[1] or "sensor_readings.txt")
   │
   ├─ open() ──fail──► report_error → exit(1)
   │
   ├─ read_loop: read(fd, buffer, 4096) ──<0──► report_error → close → exit(2)
   │      │ 0 (EOF) ─────────────────────────────┐
   │      ▼                                       │
   │   scan_loop (one byte at a time)             │
   │      '\n'      → finalize_record             │
   │      '\r'      → remember (CRLF check)       │
   │      '-'       → sign (first char only)      │
   │      '0'..'9'  → value = value*10 + digit    │
   │      other     → mark record invalid         │
   │      end of chunk → back to read_loop        │
   │                                              ▼
   └─ end_of_file: finalize the last line if needed → close() → print_results → exit(0)
```

| Storage | Holds | Why |
|---------|-------|-----|
| `rbx` | The file descriptor | Callee-saved, and preserved across `syscall` |
| `r12` | The value of the current record | Updated for every digit, so it is kept in a register |
| `r13b` | Flags for the current record (HAS_CHAR, HAS_DIGIT, INVALID, NEGATIVE, PENDING_CR) | Lets one pass classify the record |
| `r14` / `r15` | The buffer cursor and end pointer | The per-byte loop bounds |
| `.bss` qwords | `total_count`, `valid_count`, `invalid_count`, `sum_valid`, `min_valid`, `max_valid` | Persistent statistics in memory |

Key techniques:
- **Text to integer:** `imul r12, r12, 10` followed by `add r12, digit`.
- **Overflow guard:** accumulation stops once the value exceeds 100.
- **Branch-free min/max:** `cmp` followed by `cmovb` or `cmova`.
- **Integer to text:** repeated `div` by 10, storing the digits backwards.
- **Partial writes:** `write()` is retried until every byte is written.

## 9. Rebuilding the report

```bash
make evidence
cd report && npm install && node build_report.js   # writes ../Project2_Report.docx
```
