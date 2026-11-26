#!/usr/bin/env bash
# Part D - run the scripted GDB session and save the transcript.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p evidence
gdb -q -batch -x scripts/gdb_commands.gdb ./program_debug > evidence/D_gdb_session.txt 2>&1
echo "GDB evidence written to evidence/D_gdb_session.txt"
gdb -q -batch -x scripts/gdb_lazy_binding.gdb ./program > evidence/B6_gdb_lazy_binding.txt 2>&1
echo "Lazy-binding evidence written to evidence/B6_gdb_lazy_binding.txt"
