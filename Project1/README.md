# Project 1 — Investigating and Reverse Engineering an ELF Executable

A small C program that simulates a bank transaction processing system. The repository holds two builds of it: a stripped ELF executable and a debug executable. It also holds the analysis of both, done with `readelf`, `objdump`, `strace` and `gdb`.

The full analysis (Parts B, C and D) is in **`Project1_ELF_Analysis_Report.docx`**.

---

## 1. Files

| Path | Description |
|------|-------------|
| `program.c` | C source: 3 user-defined functions + `main`, a `struct Transaction`, two globals, `calloc`/`free`, a loop and conditionals |
| `program` | **Stripped** ELF executable (deliverable) |
| `program_debug` | Debug executable built with `-g` (deliverable) |
| `Makefile` | Builds both binaries with the mandated flags and regenerates all evidence |
| `scripts/static_analysis.sh` | Part B: `readelf` / `objdump` → `evidence/B*.txt` |
| `scripts/dynamic_analysis.sh` | Part C: `strace` runs → `evidence/C*.txt` |
| `scripts/gdb_analysis.sh` | Part D: runs `scripts/gdb_commands.gdb` and `scripts/gdb_lazy_binding.gdb` → `evidence/D_gdb_session.txt`, `evidence/B6_gdb_lazy_binding.txt` |
| `evidence/` | Raw command output quoted in the report |
| `report/build_report.js` | Generates the `.docx` report from `evidence/` |
| `Project1_ELF_Analysis_Report.docx` | **Analysis report** (deliverable) |

## 2. Environment and prerequisites

These are the versions used to produce the evidence (`evidence/00_environment.txt`):

| Tool | Version |
|------|---------|
| OS / arch | Linux 7.2, x86-64 |
| GCC | 16.2.1 |
| GNU Binutils (`readelf`, `objdump`, `strip`) | 2.47 |
| strace | 7.2 |
| GDB | 18.1 |
| glibc | 2.44 |
| Node.js | 26 (needed only to rebuild the report) |

On Debian/Ubuntu, install the tools with `sudo apt install build-essential binutils strace gdb`. On Arch, use `sudo pacman -S base-devel strace gdb`.

## 3. Compilation

```bash
cd Project1
make            # builds both binaries
```

`make` runs exactly the commands the assignment requires:

```bash
gcc -Wall -O0 -fno-inline -o program program.c        # compiles with zero warnings
strip program                                         # remove symbols and debug info
gcc -Wall -O0 -g -fno-inline -o program_debug program.c
```

`make clean` removes both binaries.

## 4. Execution

```bash
./program          # or: make run
```

**Inputs:** none. The program is deliberately deterministic: six transactions are hard-coded, so every run and every trace in the report can be reproduced exactly. These are the transactions:

| ID | Type | Amount |
|----|------|--------|
| 1001 | Deposit | 500.00 |
| 1002 | Withdrawal | 200.00 |
| 1003 | Deposit | 250.75 |
| 1004 | Withdrawal | 1200.50 |
| 1005 | Withdrawal | 900.00 (rejected: only 350.25 is available) |
| 1006 | Deposit | 125.25 |

The opening balance is the global `account_balance = 1000.00`.

**Expected output** (captured in `evidence/A_program_output.txt`):

```
=== Bank Transaction Processing System ===
Opening balance        :    1000.00

Transaction 1001: DEPOSIT        500.00  -> balance    1500.00
Transaction 1002: WITHDRAWAL     200.00  -> balance    1300.00
Transaction 1003: DEPOSIT        250.75  -> balance    1550.75
Transaction 1004: WITHDRAWAL    1200.50  -> balance     350.25
Transaction 1005: WITHDRAWAL     900.00  -> REJECTED (insufficient funds)
Transaction 1006: DEPOSIT        125.25  -> balance     475.50

========== Account Summary ==========
Transactions processed : 6
Total deposits         :     876.00
Total withdrawals      :    1400.50
Final account balance  :     475.50
=====================================
```

The exit status is `0`.

## 5. Program design

| Function | Responsibility |
|----------|----------------|
| `create_transactions(int n)` | `calloc`s `n` records on the heap (with a NULL check that calls `perror` and `exit`), fills them from `static const` tables in `.rodata`, sets `transaction_count`, and returns the pointer |
| `process_transactions(records, n, &dep, &wd)` | `for` loop. `if 'D'` → deposit. `else if 'W'` → withdraw when `amount <= balance`, otherwise reject. Updates the global balance and the caller's totals |
| `print_summary(dep, wd)` | Prints the totals, the transaction count and the final balance |
| `main()` | Calls the three functions, then `free()`s the records |

| Global | Section | Why |
|--------|---------|-----|
| `double account_balance = 1000.00` | `.data` | An initialised global, so its value is stored in the file |
| `int transaction_count` | `.bss` | An uninitialised global, which is zero-filled at load time |

`struct Transaction { int id; double amount; char type; }` is **24 bytes** because of alignment padding. This shows up in the assembly as `i * 24` and in the GDB heap dump.

## 6. Running the analysis (Parts B, C and D)

```bash
make analysis
```

This rebuilds the binaries and regenerates every file in `evidence/`. The main commands each script runs are:

```bash
# Part B — static analysis of the stripped binary
readelf -h program;  readelf -S -W program;  readelf -l -W program;  readelf -d program
objdump -d -M intel program;  objdump -s -j .data -j .rodata program;  readelf -r program

# Part C — dynamic analysis
strace ./program                      # stdout on a terminal → one write() per line
strace ./program | cat                # stdout on a pipe     → a single write()
strace -c ./program > /dev/null       # per-syscall summary

# Part D — scripted debugging session
gdb -q -batch -x scripts/gdb_commands.gdb ./program_debug
```

### Interactive GDB walkthrough (for the demo)

```text
$ gdb -q ./program_debug
(gdb) break _start
(gdb) break main
(gdb) break process_transactions
(gdb) run                       # stops at _start (entry point 0x...50a0)
(gdb) continue                  # stops in main
(gdb) print account_balance     # global in .data
(gdb) info symbol &transaction_count   # -> section .bss
(gdb) continue                  # stops in process_transactions
(gdb) backtrace                 # process_transactions <- main
(gdb) print *records@n          # the six heap records
(gdb) x/18gx records            # raw heap bytes (24-byte records)
(gdb) print i                   # stack local (garbage before init)
(gdb) next
(gdb) print i                   # 0
(gdb) info proc mappings        # see the [heap] and [stack] regions
```

## 7. Key findings (summary of the report)

| Question | Finding |
|----------|---------|
| Architecture / type / entry | ELF64, x86-64, `DYN` (Position-Independent Executable), entry point `0x10a0` (`_start`) |
| Linking | Dynamic: `PT_INTERP /lib64/ld-linux-x86-64.so.2`, `NEEDED libc.so.6`, lazy binding, partial RELRO |
| Functions recovered from the stripped binary | `main` @ `0x1607` (found via `lea rdi` in `_start`), `create_transactions` @ `0x1199`, `process_transactions` @ `0x1299`, `print_summary` @ `0x155e`. All were verified against `nm program_debug` |
| Globals | `account_balance` @ `0x4048` (.data, bytes `0x408F4000…` = 1000.0), `transaction_count` @ `0x4054` (.bss) |
| Branch / loop | `cmp al,0x44 ; jne` (the 'D' test), `comisd ; jb` (insufficient funds), `add [rbp-0x4],1 ; cmp ; jl` (for loop) |
| PLT / GOT | The `printf` GOT slot `0x4018` holds `0x1066` (back into its PLT stub) before the first call and libc's `printf` after it. This was observed live in GDB |
| System calls | 17–18 distinct. `calloc` → one `brk` extension; `printf` → `write(1, …)` (15 calls on a tty, 1 when piped); termination → `exit_group(0)` |
| Memory | Globals at fixed image offsets; heap records at `0x55555555a020` in `[heap]`; locals at `0x7fffffffbd..` in `[stack]` |

## 8. Rebuilding the report

```bash
make analysis                  # refresh the evidence first
cd report && npm install       # one-time: installs the 'docx' package locally
node build_report.js           # writes ../Project1_ELF_Analysis_Report.docx
```

Every excerpt in the report is read from `evidence/` when the report is built, so the report always matches the binaries that were actually analysed.

## 9. Notes

- Under GDB, addresses appear as `0x5555555…`. GDB disables ASLR, so the PIE always loads at base `0x555555554000`. Outside GDB the base changes on every run.
- GCC replaces `printf("text\n")` calls that have no format specifiers with `puts`, which is why `puts` shows up among the imports.
- GCC adds a stack canary (`fs:0x28` / `__stack_chk_fail`) to `main` by default because `main` takes the address of its local variables.
