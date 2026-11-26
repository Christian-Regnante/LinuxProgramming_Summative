# Part B6 supplement: observe lazy binding live on the STRIPPED binary.
# GDB disables ASLR, so the PIE load base is 0x555555554000; addresses are
# base + the file offsets recovered by static analysis (main = 0x1607, etc.).
set pagination off
set debuginfod enabled off
set confirm off
starti
echo \n=== stopped at first instruction (dynamic loader). Break at reconstructed main = base+0x1607 ===\n
break *0x555555555607
continue
echo \n--- printf GOT slot (base+0x4018) BEFORE first call: points back into printf@plt (base+0x1066) ---\n
x/gx 0x555555558018
echo \n--- calloc GOT slot (base+0x4020) BEFORE first call ---\n
x/gx 0x555555558020
echo \n=== break at reconstructed process_transactions = base+0x1299 (printf + calloc have now been called) ===\n
break *0x555555555299
continue
echo \n--- printf GOT slot AFTER first call: now holds the real libc address ---\n
x/gx 0x555555558018
info symbol *(long *)0x555555558018
echo \n--- calloc GOT slot AFTER first call ---\n
x/gx 0x555555558020
info symbol *(long *)0x555555558020
echo \n--- backtrace on a stripped binary: no names, only addresses ---\n
backtrace
kill
