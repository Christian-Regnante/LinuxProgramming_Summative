#!/usr/bin/env bash
# Part B - static analysis of the STRIPPED executable with readelf/objdump.
# Every output file in evidence/ is quoted verbatim by the report.
set -euo pipefail
cd "$(dirname "$0")/.."
E=evidence; mkdir -p "$E"; P=./program

{ uname -srm; gcc --version | sed -n 1p; readelf --version | sed -n 1p; objdump --version | sed -n 1p; \
  strace -V | sed -n 1p; gdb --version | sed -n 1p; ldd --version | sed -n 1p; } > "$E/00_environment.txt"
file "$P"                                      > "$E/B0_file.txt"
readelf -h "$P"                                > "$E/B1_elf_header.txt"
readelf -S -W "$P"                             > "$E/B2_sections.txt"
readelf -l -W "$P"                             > "$E/B2_program_headers.txt"
objdump -s -j .data "$P"                       > "$E/B2_data_section.txt"
objdump -s -j .rodata "$P"                     > "$E/B2_rodata_section.txt"
{ readelf -d "$P"; echo; echo "--- ldd ---"; ldd "$P"; echo; echo "--- INTERP ---"; readelf -l "$P" | grep -A1 INTERP; } \
                                               > "$E/B3_dynamic_linking.txt"
readelf -s -W "$P"                             > "$E/B3_symbols_after_strip.txt" 2>&1 || true
objdump -d -M intel -j .text "$P"              > "$E/B4_text_disassembly.txt"
objdump -d -M intel -j .plt "$P"               > "$E/B6_plt_disassembly.txt"
{ readelf -r "$P"; echo; echo "--- .got / .got.plt raw contents (file image, before relocation) ---"; \
  objdump -s -j .got -j .got.plt "$P"; }       > "$E/B6_relocations_got.txt"
# Verification ONLY (done after blind reconstruction): symbol addresses in the debug build.
nm program_debug | grep -E ' (T|D|B) '         > "$E/B4_verify_nm_debug.txt"
echo "Static analysis evidence written to $E/"
