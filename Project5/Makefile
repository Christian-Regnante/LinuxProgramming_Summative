# Project 5 - concurrent TCP event registration (server + client)
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TSAN    = -Wall -Wextra -O1 -g -fsanitize=thread -pthread

.PHONY: all test demo race tsan evidence clean

all: server client

server: server.c common.c common.h
	$(CC) $(CFLAGS) -o $@ server.c common.c

client: client.c common.c common.h
	$(CC) $(CFLAGS) -o $@ client.c common.c

# ThreadSanitizer build of the server (data-race detection)
server_tsan: server.c common.c common.h
	$(CC) $(TSAN) -o $@ server.c common.c

test: all
	python3 tests/protocol_test.py

race: all
	./tests/race_test.sh

demo: all
	./tests/run_demo.sh

tsan: all server_tsan
	./tests/tsan_check.sh

evidence: all server_tsan
	./capture_evidence.sh

clean:
	rm -f server client server_tsan
