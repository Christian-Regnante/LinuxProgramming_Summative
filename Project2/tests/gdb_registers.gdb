# Shows the per-record parser state (registers) and the statistics (memory)
# each time a record is finalised. Input: tests/cases/01_task_example_lf.txt
set pagination off
set confirm off
set debuginfod enabled off
break finalize_record
run tests/cases/01_task_example_lf.txt
set $n = 0
while $n < 6
  printf "--- record %d ---  r12 (value) = %ld   r13 (flags) = 0x%02lx   [HAS_CHAR=1 HAS_DIGIT=2 INVALID=4 NEGATIVE=8]\n", $n+1, $r12, $r13
  printf "    memory before update: total=%ld valid=%ld invalid=%ld sum=%ld\n", *(long*)&total_count, *(long*)&valid_count, *(long*)&invalid_count, *(long*)&sum_valid
  set $n = $n + 1
  continue
end
