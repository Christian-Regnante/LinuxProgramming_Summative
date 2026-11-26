#!/usr/bin/env bash
# Negative control for the ThreadSanitizer check.
# A clean TSan run is only meaningful if TSan WOULD detect a race. This script
# copies library_reservation.c to a temporary directory, introduces ONE
# deliberate bug - the student thread increments sys.submitted AFTER releasing
# sys.lock - builds it with -fsanitize=thread and shows that TSan reports the
# conflict with the monitor, which reads the counter under the lock.
# The submitted source file is never modified.
set -u
cd "$(dirname "$0")/.." || exit 1
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/racy.c" <<'PY'
import sys
src = open("library_reservation.c").read()
# 1) remove the correctly locked increment ...
locked = "        enqueue(r);\n        sys.submitted++;\n"
assert src.count(locked) == 1, "anchor 1 not found"
src = src.replace(locked, "        enqueue(r);\n")
# 2) ... and re-insert it after the mutex has been released.
unlock = ("        pthread_cond_signal(&sys.not_empty);   /* wake the consumer if asleep */\n"
          "        pthread_mutex_unlock(&sys.lock);\n")
assert src.count(unlock) == 1, "anchor 2 not found"
src = src.replace(unlock, unlock +
          "        sys.submitted++;   /* INJECTED BUG: shared counter updated without the lock */\n")
open(sys.argv[1], "w").write(src)
PY

echo "Injected change (diff of correct vs racy producer):"
diff -u library_reservation.c "$TMP/racy.c" | sed -n '/^@@/,$p' | sed 's/^/    /'
gcc -Wall -Wextra -O1 -g -fsanitize=thread -pthread -o "$TMP/racy" "$TMP/racy.c"
TSAN_OPTIONS="halt_on_error=0" "$TMP/racy" --time-scale=0.05 > "$TMP/out.txt" 2>&1
n=$(grep -c "WARNING: ThreadSanitizer: data race" "$TMP/out.txt")
echo
echo "ThreadSanitizer data-race reports for the racy copy: $n"
echo "First report (abridged):"
awk '/WARNING: ThreadSanitizer: data race/{p=1} p{print "    " $0; if (++k==14) exit}' "$TMP/out.txt" | sed -E 's/ \(BuildId: [0-9a-f]+\)//; s|/tmp/tmp\.[A-Za-z0-9]+/||'
[[ $n -gt 0 ]] && echo "=> TSan detects an unlocked access, so the clean run of the real program is meaningful."
