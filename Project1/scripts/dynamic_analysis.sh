#!/usr/bin/env bash
# Part C - dynamic analysis of the STRIPPED executable with strace.
set -euo pipefail
cd "$(dirname "$0")/.."
E=evidence; mkdir -p "$E"

./program                                      > "$E/A_program_output.txt"
# 1) stdout to a pseudo-terminal -> line buffered (one write per line).
#    `script` gives the program a real tty so the buffering mode is the same as an interactive run.
script -qec "strace -o $E/C1_strace_tty.txt ./program" /dev/null > /dev/null
# 2) stdout to a pipe -> fully buffered (one write at exit).
strace -o "$E/C2_strace_pipe.txt" ./program | cat > /dev/null
# 3) Per-syscall summary table.
strace -c -o "$E/C3_strace_summary.txt" ./program > /dev/null
# 4) Timestamps + decoded memory-related calls for the heap discussion.
strace -tt -e trace=memory,exit_group -o "$E/C4_strace_memory.txt" ./program > /dev/null
# 5) How stdio decides its buffering mode: fstat(1) (+ ioctl TCGETS2 isatty probe when not a pty).
strace -e trace=fstat,ioctl,write -o "$E/C5_stdio_buffering_devnull.txt" ./program > /dev/null
echo "Dynamic analysis evidence written to $E/"
