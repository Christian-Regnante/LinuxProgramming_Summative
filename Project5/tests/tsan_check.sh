#!/usr/bin/env bash
# Runs the ThreadSanitizer build of the server (make server_tsan) under a mixed
# concurrent workload and counts data-race reports:
#   - 20 clients connect at once, authenticate, LIST, REGISTER for event 103
#     (capacity 6, so 14 are refused), MYEVENTS;
#   - every third client disconnects abruptly instead of sending QUIT;
#   - then the server is stopped with SIGINT, exercising the shutdown path.
set -u
cd "$(dirname "$0")/.." || exit 1
[ -x ./server_tsan ] || make -s server_tsan
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# KEEP=<dir> copies the full server output (including any TSan report) there.
[ -n "${KEEP:-}" ] && trap 'mkdir -p "$KEEP"; cp "$TMP/out.txt" "$KEEP/"; rm -rf "$TMP"' EXIT
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
./server_tsan -p "$PORT" --fresh --race-delay-ms 5 --state "$TMP/r.csv" --log "$TMP/l.log" > "$TMP/out.txt" 2>&1 &
SRV=$!
sleep 0.5
PORT=$PORT python3 - <<'PY'
import os, socket, threading
port = int(os.environ["PORT"])
barrier = threading.Barrier(20)
results = []
def session(i):
    s = socket.create_connection(("127.0.0.1", port)); f = s.makefile("rw")
    f.readline()
    f.write(f"AUTH S10{i:02d}\nLIST\n"); f.flush()
    for _ in range(6): f.readline()
    barrier.wait()                                   # all register at once
    f.write("REGISTER 103\nMYEVENTS\n"); f.flush()
    results.append(f.readline().split()[0:2])
    while f.readline().strip() != "END": pass
    if i % 3 == 0:
        s.close()                                    # abrupt disconnect
    else:
        f.write("QUIT\n"); f.flush(); f.readline(); s.close()
threads = [threading.Thread(target=session, args=(i,)) for i in range(1, 21)]
for t in threads: t.start()
for t in threads: t.join()
ok = sum(1 for r in results if r == ["OK", "REGISTERED"])
print(f"workload: 20 concurrent sessions, {ok} registrations accepted for event 103 (capacity 6), "
      f"{len(results) - ok} refused, 6 abrupt disconnects")
PY
sleep 0.5
kill -INT "$SRV"; wait "$SRV"
echo "sessions ended: $(grep -c 'session ended' "$TMP/out.txt") (unexpected disconnects: $(grep -c 'DISCONNECTED UNEXPECTEDLY' "$TMP/out.txt"))"
grep -A3 'final registrations' "$TMP/out.txt" | sed -E 's/^\[[^]]*\] //'
n=$(grep -c 'WARNING: ThreadSanitizer' "$TMP/out.txt")
echo "ThreadSanitizer data-race reports: $n"
[[ $n -eq 0 ]]
