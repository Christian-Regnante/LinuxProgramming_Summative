#!/usr/bin/env bash
# Runs the program many times with different book-ID seeds and very short
# time scales (where thread interleavings vary the most) and checks that every
# run terminates on its own, exits with status 0, passes all five invariant
# checks, never logs a queue size above 5, and approves exactly the even IDs.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./library_reservation
RUNS=${1:-40}
fail=0
for i in $(seq 1 "$RUNS"); do
    seed=$((1000 + i * 7919))
    scale=$(awk -v i="$i" 'BEGIN { s[0]=0.002; s[1]=0.005; s[2]=0.01; s[3]=0.03; print s[i%4] }')
    out=$(timeout 60 "$BIN" --time-scale="$scale" --seed="$seed" 2>&1); code=$?
    passes=$(grep -c '^ \[PASS\]' <<<"$out")
    over=$(grep -oE 'queue [0-9]+/5' <<<"$out" | awk -F'[ /]' '$2>5' | wc -l)
    # every APPROVED line must name an even book, every REJECTED line an odd one
    wrong=$(grep -E 'APPROVED|REJECTED' <<<"$out" | awk '{for(k=1;k<=NF;k++) if($k=="book") b=$(k+1);
            if (($0 ~ /APPROVED/ && b%2) || ($0 ~ /REJECTED/ && !(b%2))) print}' | wc -l)
    if [[ $code -ne 0 || $passes -ne 5 || $over -ne 0 || $wrong -ne 0 ]]; then
        echo "run $i (seed $seed, scale $scale): FAIL exit=$code passes=$passes over=$over wrong=$wrong"
        fail=$((fail+1))
    fi
done
echo "Stress test: $RUNS runs, $((RUNS-fail)) passed, $fail failed"
[[ $fail -eq 0 ]]
