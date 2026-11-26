# Project 5 — Concurrent TCP-Based University Event Registration System

A client–server system in C. Students connect to a central server over TCP, authenticate with their student ID, view the university events and register for them. The server handles every client in its own POSIX thread and protects the shared event records with mutexes. Even when many students compete for the **last place** at the same instant, the capacity is never exceeded.

The design discussion, protocol specification, analysis and full evidence are in **`Project5_Report.pdf`**.

---

## 1. Files

| Path | Description |
|------|-------------|
| `server.c` | The server: TCP listener, one thread per client, authentication, the event table, the registration critical section, logging, persistence, timeouts and graceful shutdown |
| `client.c` | An interactive client: connects, authenticates, lists events, registers and shows the results. It can also be driven by a script through stdin |
| `common.h`, `common.c` | Shared protocol definitions plus the TCP helpers (`read_line` framing, `send_all`, `send_line`) |
| `students.txt` | Authorised students (`ID,Name`). Edit this file to add students |
| `Makefile` | `make`, `make test`, `make race`, `make demo`, `make tsan`, `make evidence`, `make clean` |
| `tests/protocol_test.py` | 11 automated tests over raw TCP that cover every requirement |
| `tests/race_test.sh` | A last-seat race between 10 real `client` processes, repeated over many rounds |
| `tests/race_negative_control.sh` | Shows that a server with a check-then-act bug **over-books**, so the race test is meaningful |
| `tests/tsan_check.sh` | Runs the ThreadSanitizer build of the server under a mixed concurrent workload |
| `tests/run_demo.sh` | The Part D demonstration: writes a transcript for every required scenario to `evidence/demo/` |
| `capture_evidence.sh` | Regenerates everything in `evidence/` |

## 2. Requirements

You need Linux with GCC and POSIX threads (developed with GCC 16.2.1 on Linux 7.2 x86-64). The tests also need Python 3 and bash, and `make tsan` needs ThreadSanitizer, which ships with GCC.

## 3. Build

```bash
cd Project5
make            # builds ./server and ./client with -Wall -Wextra -O2 -pthread (zero warnings)
```

## 4. Run

**Terminal 1, the server:**

```bash
./server                          # port 8080; keeps registrations in registrations.csv
./server -p 9000 --fresh          # another port, starting with no registrations
./server --help
```

| Server option | Meaning (default) |
|---------------|-------------------|
| `-p, --port N` | TCP port to listen on (8080) |
| `--fresh` | Start with no registrations and truncate the state file |
| `--timeout SEC` | Disconnect clients that stay idle for SEC seconds (120) |
| `--max-clients N` | Maximum number of simultaneous clients (64). Any extra client receives `ERR SERVER_FULL` |
| `--students FILE` | The list of authorised students (`students.txt`) |
| `--state FILE` | The persistent registration file (`registrations.csv`) |
| `--log FILE` | The server log file (`server.log`). Everything is also printed to stdout |
| `--race-delay-ms MS` | A testing aid: simulated latency *inside* the registration critical section (0) |

Stop the server with **Ctrl-C**. It notifies the connected clients, waits for their sessions to finish, prints the final registration table and exits.

**Terminals 2…N, one client per student:**

```bash
./client                    # connects to 127.0.0.1:8080
./client 192.168.1.20 9000  # another host or port
```

**Inputs:**
- Student IDs from `students.txt`, `S1001` … `S1020`; for example `S1001` is Amina Uwase.
- Menu choices: `1` list events, `2` register, `3` my registrations, `0` exit.
- Event IDs: `101`, `102` or `103`.

| Event ID | Event name | Capacity |
|----------|------------|----------|
| 101 | AI Workshop | 5 |
| 102 | Cybersecurity Seminar | 4 |
| 103 | Research Conference | 6 |

## 5. Expected output

**A successful session**, from `evidence/demo/01_auth_list_register_success.txt`:

```
Connected to 127.0.0.1:34201
University Event Registration Server - please authenticate

Enter your student ID: S1001
Authentication successful. Welcome, Amina Uwase (S1001)!

  ID    Event                    Registered   Availability
  ----- ------------------------ ----------   ------------
  101   AI Workshop              0 / 5        5 places left
  102   Cybersecurity Seminar    0 / 4        4 places left
  103   Research Conference      0 / 6        6 places left

Menu:  1) List events   2) Register for an event   3) My registrations   0) Exit
Choose an option: 2
Enter the ID of the event to register for: 101
>> SUCCESS: you are registered for 101 AI Workshop (place 1 of 5).
...
Choose an option: 0
Registration session closed. Goodbye, S1001.
```

**The other results the client handles:**

```
Authentication FAILED: Unknown student ID (attempt 1 of 3)
>> NOT REGISTERED: duplicate registration - You are already registered for 103 Research Conference.
>> NOT REGISTERED: the event is full - 102 Cybersecurity Seminar has reached its capacity of 4.
>> NOT REGISTERED: event unavailable - There is no event with ID 555.
Server: The server is shutting down. Goodbye.
```

**Server log**, from `evidence/demo/server_log.txt`, showing six simultaneous clients competing for event 102 (capacity 4):

```
[..] client 127.0.0.1:48168 connected | active clients: 6
[..] client 127.0.0.1:48158 (S1007): REGISTERED for 102 Cybersecurity Seminar - seat 1/4
[..] client 127.0.0.1:48168 (S1009): REGISTERED for 102 Cybersecurity Seminar - seat 2/4
[..] client 127.0.0.1:48164 (S1008): REGISTERED for 102 Cybersecurity Seminar - seat 3/4
[..] client 127.0.0.1:48140 (S1004): REGISTERED for 102 Cybersecurity Seminar - seat 4/4
[..] client 127.0.0.1:48156 (S1005): event 102 FULL (4/4) - registration rejected
[..] client 127.0.0.1:48172 (S1006): event 102 FULL (4/4) - registration rejected
[..] client 127.0.0.1:36226 (S1011): session ended - DISCONNECTED UNEXPECTEDLY (connection closed without QUIT) | active clients: 0
```

## 6. Application protocol

The protocol is plain text: one message per line, terminated by `\n`. You can try it by hand with `nc 127.0.0.1 8080`.

| Client → server | Server → client |
|-----------------|-----------------|
| *(on connect)* | `WELCOME …` (or `ERR SERVER_FULL`) |
| `AUTH <student_id>` | `OK AUTH <id> <name>` · `ERR AUTH_FAILED …` (3 attempts allowed) · `ERR ALREADY_LOGGED_IN` |
| `LIST` | `EVENTS n`, then n lines `EVENT id\|name\|registered\|capacity`, then `END` |
| `REGISTER <event_id>` | `OK REGISTERED <id> <seat> <capacity> <name>` · `ERR EVENT_FULL` · `ERR DUPLICATE` · `ERR NO_SUCH_EVENT` · `ERR BAD_REQUEST` |
| `MYEVENTS` | `MYEVENTS n`, then n lines `EVENT id\|name`, then `END` |
| `QUIT` | `BYE <id>` |
| anything else before `AUTH` | `ERR NOT_AUTHENTICATED` |
| *(server initiated)* | `ERR TIMEOUT` (idle session) · `ERR SERVER_SHUTDOWN` |

TCP is a byte stream, so messages are framed by `\n`. `read_line()` buffers the incoming bytes until a complete line has arrived, which handles both split and coalesced segments.

## 7. Design summary

| Topic | Approach |
|-------|----------|
| Concurrency | **One detached POSIX thread per client.** The main thread only calls `accept()` |
| Connected clients | A `clients[]` table (fd, address, student ID) and an active count, guarded by `clients_lock`. The count is shown on every connect and disconnect |
| Authentication | IDs come from `students.txt`. There are 3 attempts, after which the server disconnects. The same ID cannot be logged in twice at once. Every command except `AUTH` and `QUIT` is refused until the student authenticates |
| Shared event data | The `events[]` table (counts and attendee lists) is guarded by `events_lock` |
| **Last-seat race** | `register_student()` does the existence check, the duplicate check, the capacity check, the seat assignment and persistence **inside one critical section**. When two students race for the last place, the second thread waits on the mutex and then sees `registered == capacity`, so exactly one of them succeeds |
| No I/O under lock | `LIST` and `MYEVENTS` copy the data under the lock and send it after unlocking, so a slow client cannot stall the other clients |
| Unexpected disconnects | `recv()` returning 0 or `ECONNRESET` ends the session. The slot is freed and the registrations are kept. `SIGPIPE` is ignored and `MSG_NOSIGNAL` is used |
| Extras | A timestamped, thread-safe log (stdout + `server.log`), an idle timeout (`SO_RCVTIMEO`), a server-full limit, graceful `SIGINT`/`SIGTERM` shutdown, persistence in `registrations.csv`, and a client that reacts at once to server messages (`poll()` on stdin and the socket) |

## 8. Testing and demonstration

```bash
make test       # tests/protocol_test.py: 11 tests → OK
make race       # tests/race_test.sh: 10 rounds × 10 clients racing for 1 seat → 10/10 PASS
make tsan       # ThreadSanitizer server under 20 concurrent sessions → 0 data races
make demo       # tests/run_demo.sh: transcripts for every Part D scenario in evidence/demo/
./tests/race_negative_control.sh   # the same race against a server with a check-then-act bug
```

| Part D requirement | Evidence |
|--------------------|----------|
| 1. Successful authentication | `evidence/demo/01_auth_list_register_success.txt` |
| 2. Unsuccessful authentication | `02_auth_failure.txt` (3 wrong IDs → disconnected), `02b_auth_failure_then_success.txt` |
| 3. Available events displayed | `01_…` (event table) |
| 4. Successful registration | `01_…` (`SUCCESS … place 1 of 5`) |
| 5. Registration for a full event | `05_event_full.txt` |
| 6. Same student registers twice | `06_duplicate_registration.txt` (also an unknown event ID → "event unavailable") |
| 7. ≥ 5 simultaneous clients | `07_simultaneous_S1004…S1009.txt`, the server log (`active clients: 6`), and `07_ss_established_connections.txt` (6 established TCP connections from `ss`) |
| 8. Simultaneous requests never exceed capacity | The six clients race for event 102 (capacity 4): 4 are registered and 2 are refused. `evidence/race_test.txt`: 10 clients race for 1 seat in 10 rounds, always 1 winner and a final count of 4/4 |
| 9. Unexpected client disconnection | `09_unexpected_disconnect_killed_client.txt` (client killed with SIGKILL), plus the server log entry `DISCONNECTED UNEXPECTEDLY`. The server keeps working and the student can log in again (`09b_…`) |
| (extra) Graceful server shutdown | `10_server_shutdown_while_connected.txt` |

**Negative control** (`evidence/race_negative_control.txt`): this copy of the server releases the lock between the capacity check and the update. Against it, all 10 racers "succeed" and event 102 ends at **13/4**. The real server ends at **4/4** in every round, which shows that the single critical section is what prevents over-booking.

**ThreadSanitizer finding:** an early version used `volatile sig_atomic_t` for the shutdown flag, which the signal handler writes and the session threads read. TSan reported this as a data race. The flag is now a lock-free C11 `atomic_int`, and repeated TSan runs report no races.

