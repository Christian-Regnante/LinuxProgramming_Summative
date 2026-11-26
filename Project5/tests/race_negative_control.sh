#!/usr/bin/env bash
# Negative control for the last-seat race test.
# Builds a temporary copy of server.c containing the classic CHECK-THEN-ACT
# (TOCTOU) bug: events_lock is released after the capacity check and taken
# again for the update, so several threads can pass the check for the same
# last seat. Running tests/race_test.sh against that copy must show
# over-booking - proving that the test really detects the race and that the
# real server passes because of its single critical section.
# server.c itself is never modified.
set -u
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/server_racy.c" <<'PY'
import sys
src = open("server.c").read()
correct = ("        if (opt_race_delay_ms > 0)\n"
           "            sleep_ms(opt_race_delay_ms);\n")
buggy = ("        pthread_mutex_unlock(&events_lock);   /* INJECTED BUG: lock released after the check */\n"
         "        if (opt_race_delay_ms > 0)\n"
         "            sleep_ms(opt_race_delay_ms);\n"
         "        pthread_mutex_lock(&events_lock);     /* ...and re-acquired for the update */\n")
assert src.count(correct) == 1, "anchor not found"
open(sys.argv[1], "w").write(src.replace(correct, buggy))
PY
echo "Injected change (check and update in two separate critical sections):"
diff -u server.c "$TMP/server_racy.c" | sed -n '/^@@/,$p' | sed 's/^/    /'
gcc -Wall -Wextra -O2 -pthread -I. -o "$TMP/server_racy" "$TMP/server_racy.c" common.c
echo
echo "Race test against the BUGGY server:"
SERVER="$TMP/server_racy" ./tests/race_test.sh 3
status=$?
echo
if [[ $status -ne 0 ]]; then
    echo "=> The buggy server over-books the event, so the race test detects the bug;"
    echo "   the real server passes it because check and update share one critical section."
else
    echo "=> WARNING: the injected race was not observed."
fi
