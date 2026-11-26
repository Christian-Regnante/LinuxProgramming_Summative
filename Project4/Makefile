# Project 4 - multithreaded library reservation system (POSIX threads)
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TSAN    = -Wall -Wextra -O1 -g -fsanitize=thread -pthread

.PHONY: all run fast tsan helgrind evidence clean

all: library_reservation

library_reservation: library_reservation.c
	$(CC) $(CFLAGS) -o $@ $<

# ThreadSanitizer build: reports any data race at run time
library_reservation_tsan: library_reservation.c
	$(CC) $(TSAN) -o $@ $<

run: library_reservation
	./library_reservation                     # real timings, ~46 s

fast: library_reservation
	./library_reservation --time-scale=0.1    # same behaviour, 10x faster

tsan: library_reservation_tsan
	./library_reservation_tsan --time-scale=0.1

evidence: library_reservation library_reservation_tsan
	./capture_evidence.sh

clean:
	rm -f library_reservation library_reservation_tsan
