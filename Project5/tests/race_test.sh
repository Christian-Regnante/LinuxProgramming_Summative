#!/usr/bin/env bash
# Last-seat race test using the real ./client program.
#
# Each round:
#   1. start a fresh server with --race-delay-ms 50 (50 ms of simulated latency
#      inside the registration critical section - a wide race window);
#   2. fill event 102 (capacity 4) up to 3/4 with three students;
#   3. connect and authenticate RACERS more students (launched in random order),
#      all parked at a barrier;
#   4. release them at the same instant: every one sends REGISTER 102;
#   5. require exactly ONE success, RACERS-1 "event is full" answers and a
#      final count of 4/4 in the server's registration table.
#
# Usage: tests/race_test.sh [ROUNDS] [RACERS]      (defaults 10 rounds, 10 racers)
#        SERVER=./server_tsan tests/race_test.sh 5  (run against another binary)
#        VERBOSE=1 shows the client transcripts and server log of round 1
set -u
cd "$(dirname "$0")/.." || exit 1
SERVER=${SERVER:-./server}
ROUNDS=${1:-10}
RACERS=${2:-10}
EVENT=102
TMP=$(mktemp -d); trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

student() { printf 'S%04d' $((1000 + $1)); }   # 1 -> S1001

pass=0
for round in $(seq 1 "$ROUNDS"); do
    R="$TMP/round$round"; mkdir -p "$R"
    PORT=$(free_port)
    "$SERVER" -p "$PORT" --fresh --race-delay-ms 50 --state "$R/reg.csv" --log "$R/server.log" \
        > "$R/server.out" 2>&1 &
    SRV=$!
    for _ in $(seq 200); do grep -q "server listening" "$R/server.out" 2>/dev/null && break; sleep 0.02; done

    # 2. three seats taken in advance -> exactly one place left
    for i in 1 2 3; do
        printf '%s\n2\n%s\n0\n' "$(student $i)" "$EVENT" | ./client 127.0.0.1 "$PORT" > "$R/prefill_$i.txt"
    done

    # 3. racers authenticate, then wait for the GO file
    GO="$R/go"
    for i in $(seq 4 $((3 + RACERS)) | shuf); do      # random launch order each round
        sid=$(student "$i")
        { echo "$sid"; while [ ! -e "$GO" ]; do sleep 0.002; done; printf '2\n%s\n0\n' "$EVENT"; } \
            | ./client 127.0.0.1 "$PORT" > "$R/racer_$sid.txt" &
    done
    for _ in $(seq 500); do                      # until every racer is authenticated
        [ "$(grep -c 'authenticated as' "$R/server.out")" -ge $((3 + RACERS)) ] && break
        sleep 0.01
    done

    # 4. release all racers at once
    touch "$GO"
    wait $(jobs -p | grep -v "^$SRV\$") 2>/dev/null
    kill -INT "$SRV"; wait "$SRV"

    # 5. verify
    ok=$(grep -l 'SUCCESS' "$R"/racer_*.txt | wc -l)
    full=$(grep -l 'event is full' "$R"/racer_*.txt | wc -l)
    final=$(grep -E "final registrations" -A3 "$R/server.out" | grep " $EVENT " | grep -oE '[0-9]+/[0-9]+')
    winner=$(grep -l 'SUCCESS' "$R"/racer_*.txt | sed -E 's/.*racer_(S[0-9]+)\.txt/\1/' | paste -sd, -)
    if [[ $ok -eq 1 && $full -eq $((RACERS - 1)) && $final == "4/4" ]]; then
        status=PASS; pass=$((pass + 1))
    else
        status=FAIL
    fi
    printf 'round %2d: %d racers -> %d registered (%s), %d rejected as full, final %s  %s\n' \
        "$round" "$RACERS" "$ok" "${winner:-none}" "$full" "${final:-?}" "$status"

    if [[ $round -eq 1 && ${VERBOSE:-0} -eq 1 ]]; then
        echo; echo "---- round 1: server log during the race ----"
        grep -E "REGISTERED|FULL" "$R/server.out" | sed 's/^/  /'
        echo "---- round 1: final registration table ----"
        grep -A3 "final registrations" "$R/server.out" | sed 's/^/  /'
        echo "---- round 1: the winner's client transcript (registration part) ----"
        grep -B1 -A1 'SUCCESS' "$R/racer_${winner%%,*}.txt" | sed 's/^/  /'
        echo "---- round 1: a losing client's transcript (registration part) ----"
        loser=$(grep -l 'event is full' "$R"/racer_*.txt | head -1)
        grep -B1 -A1 'event is full' "$loser" | sed 's/^/  /'
        echo
    fi
done
echo "Race test: $pass of $ROUNDS rounds passed"
[[ $pass -eq $ROUNDS ]]
