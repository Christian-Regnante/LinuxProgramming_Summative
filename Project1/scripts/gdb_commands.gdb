# Part D - scripted GDB session on program_debug.
set pagination off
set debuginfod enabled off
set confirm off
set disassembly-flavor intel
set print pretty on

echo \n===== D1: setting breakpoints =====\n
break _start
break main
break process_transactions
info breakpoints

echo \n===== Run -> stops at _start (ELF entry point) =====\n
run
info registers rip rsp
x/6i $pc
echo \n--- backtrace at _start (no caller: first user-space instruction) ---\n
backtrace

echo \n===== continue -> stops at main =====\n
continue
backtrace
echo \n--- D3a: global variables (static storage) ---\n
print account_balance
print &account_balance
print transaction_count
print &transaction_count
info symbol &account_balance
info symbol &transaction_count
echo \n--- D3c: local variables on main's stack frame ---\n
print &total_deposits
print &total_withdrawals
print $rsp
print $rbp
info frame

echo \n===== continue -> stops in process_transactions =====\n
continue
echo \n--- D2: call stack ---\n
backtrace
info frame
echo \n--- parameters / locals of process_transactions ---\n
info args
info locals
echo \n--- D3b: heap-allocated transaction records ---\n
print records
print *records@n
print sizeof(struct Transaction)
echo \n--- raw heap bytes: 6 records x 24 bytes = 18 quadwords ---\n
x/18gx records
echo \n--- record[0].amount decoded as double, record[0].type as char ---\n
x/1fg &records[0].amount
x/1cb &records[0].type
echo \n--- D3c: loop counter i (stack local) BEFORE initialisation: stack is not zeroed ---\n
print i
print &i
next
echo \n--- after executing `int i = 0` ---\n
print i

echo \n--- step through two loop iterations watching the global change ---\n
watch account_balance
continue
continue
backtrace
print i
print *total_deposits
delete

echo \n===== D4: which memory region does each address belong to? =====\n
info proc mappings
echo \n--- global / heap / stack addresses side by side ---\n
print &account_balance
print records
print &i
echo \n--- total_deposits parameter: lives in THIS frame, points INTO main's frame ---\n
print &total_deposits
print total_deposits
up
print &total_deposits
down

echo \n===== finish the program =====\n
continue
