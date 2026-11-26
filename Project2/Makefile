# Project 2 - NASM x86-64 sensor statistics (no libc, raw Linux syscalls)
ASM     = nasm
ASFLAGS = -f elf64 -g -F dwarf
LD      = ld

.PHONY: all run test fuzz evidence clean

all: sensor_stats

sensor_stats.o: sensor_stats.asm
	$(ASM) $(ASFLAGS) sensor_stats.asm -o sensor_stats.o

sensor_stats: sensor_stats.o
	$(LD) -o sensor_stats sensor_stats.o

run: sensor_stats
	./sensor_stats

# Deterministic test cases with hand-derived expected output
test: sensor_stats
	./tests/run_tests.sh

# Randomised differential test against an independent Python reference
fuzz: sensor_stats
	python3 tests/fuzz.py 500

# Capture terminal evidence used in README and report
evidence: sensor_stats
	./tests/capture_evidence.sh

clean:
	rm -f sensor_stats sensor_stats.o
