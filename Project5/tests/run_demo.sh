#!/usr/bin/env bash
# Part D demonstration: drives the real ./client program against one running
# ./server through every required scenario and saves the transcripts.
#
#   1 successful authentication        5 registration for a full event
#   2 unsuccessful authentication      6 duplicate registration
#   3 list of available events          7 >= 5 simultaneous clients
#   4 successful registration           8 simultaneous requests never exceed capacity
#   9 unexpected client disconnection  (+ graceful server shutdown)
#
# Output: evidence/demo/*.txt (client transcripts) and evidence/demo/server_log.txt
set -u
cd "$(dirname "$0")/.." || exit 1
OUT=evidence/demo
rm -rf "$OUT"; mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT
PORT=${PORT:-$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')}

client() {   # client <transcript-name>  (stdin = scripted answers)
    { echo "\$ ./client 127.0.0.1 $PORT"; ./client 127.0.0.1 "$PORT" 2>&1; echo "[client exit status: $?]"; } > "$OUT/$1.txt"
}
hold() {      # keep a client's stdin open until the script creates $TMP/release
    while [ ! -e "$TMP/release" ]; do sleep 0.05; done
}
wait_for() {  # wait_for <pattern> <count>: until the server log shows it
    for _ in $(seq 1000); do [ "$(grep -c -- "$1" "$TMP/server.out")" -ge "$2" ] && return; sleep 0.01; done
}

# The server runs inside the temporary directory, so its state and log files
# (registrations.csv, server.log) are created there with their default names.
PROJECT=$PWD
(cd "$TMP" && exec "$PROJECT/server" -p "$PORT" --fresh --students "$PROJECT/students.txt") \
    > "$TMP/server.out" 2>&1 &
SRV=$!
wait_for "server listening" 1
echo "server running on port $PORT"

# --- 1, 3, 4: successful authentication, event list, successful registration
printf 'S1001\n2\n101\n3\n0\n' | client 01_auth_list_register_success
echo "scenario 1/3/4 done"

# --- 2: unsuccessful authentication (three unknown IDs -> server disconnects)
printf 'X9999\nS9999\nabc!\n' | client 02_auth_failure
# and a typo followed by the correct ID
printf 'S10O1\nS1002\n1\n0\n' | client 02b_auth_failure_then_success
echo "scenario 2 done"

# --- 6: the same student registers for the same event twice (+ an unknown event ID)
printf 'S1003\n2\n103\n2\n103\n2\n555\n3\n0\n' | client 06_duplicate_registration
echo "scenario 6 done"

# --- 7 + 8: six students connected at the same time, all asking for event
#     102 (capacity 4) at the same instant -> exactly 4 accepted, 2 refused
GO="$TMP/go"
for sid in S1004 S1005 S1006 S1007 S1008 S1009; do
    { echo "$sid"; while [ ! -e "$GO" ]; do sleep 0.002; done
      printf '2\n102\n'; sleep 1.5; printf '1\n0\n'; } | client "07_simultaneous_$sid" &
done
wait_for "authenticated as" 9          # 3 earlier logins + 6 now
sleep 0.2
# kernel view: the server's established TCP connections at this moment
{ echo "\$ ss -tn state established '( sport = :$PORT )'"
  ss -tn state established "( sport = :$PORT )"; } > "$OUT/07_ss_established_connections.txt"
touch "$GO"
wait $(jobs -p | grep -v "^$SRV\$")
echo "scenario 7/8 done"

# --- 5: registration attempt for an event that is already full
printf 'S1010\n1\n2\n102\n0\n' | client 05_event_full
echo "scenario 5 done"

# --- 9: unexpected disconnection - the client process is killed (SIGKILL)
#        in the middle of its session; the server must clean up and continue
{ printf 'S1011\n2\n103\n'; hold; } | { echo "\$ ./client 127.0.0.1 $PORT   (killed with SIGKILL after registering)"; exec ./client 127.0.0.1 "$PORT"; } \
    > "$OUT/09_unexpected_disconnect_killed_client.txt" 2>&1 &
wait_for "S1011): REGISTERED" 1
CLIENT_PID=$(pgrep -n -f "^./client 127.0.0.1 $PORT\$")
kill -9 "$CLIENT_PID"
echo "[client process $CLIENT_PID killed with SIGKILL - no QUIT was sent]" >> "$OUT/09_unexpected_disconnect_killed_client.txt"
wait_for "S1011): session ended" 1
# the server keeps working, the seat is kept, and S1011 can log in again
printf 'S1011\n3\n1\n0\n' | client 09b_after_disconnect_same_student_again
echo "scenario 9 done"

# --- graceful server shutdown while a client is connected
{ printf 'S1012\n'; hold; } | client 10_server_shutdown_while_connected &
wait_for "S1012): authenticated" 1
kill -INT "$SRV"; wait "$SRV"
touch "$TMP/release"                      # let the input feeders finish
wait 2>/dev/null
echo "server stopped"

cp "$TMP/server.out" "$OUT/server_log.txt"
cp "$TMP/registrations.csv" "$OUT/registrations.csv"
echo "Transcripts written to $OUT/"
