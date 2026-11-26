# Project 1 - ELF analysis. Compiler flags are mandated by the assignment.
CC      = gcc
CFLAGS  = -Wall -O0 -fno-inline

.PHONY: all clean analysis run

all: program program_debug

program: program.c
	$(CC) $(CFLAGS) -o program program.c
	strip program

program_debug: program.c
	$(CC) $(CFLAGS) -g -o program_debug program.c

run: program
	./program

# Regenerate every piece of evidence quoted in the report.
analysis: all
	./scripts/static_analysis.sh
	./scripts/dynamic_analysis.sh
	./scripts/gdb_analysis.sh

clean:
	rm -f program program_debug
