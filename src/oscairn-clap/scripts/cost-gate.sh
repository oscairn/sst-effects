#!/bin/sh
# The cost gate: instructions per block of the cases in cost-baseline.txt (scripts/cost-a53.sh),
# each within 2 % of its baseline either way; a case cheaper by more says the baseline is stale.
#   scripts/cost-gate.sh <build dir> <image|->
# The baseline is counted with GCC 13.3 and glibc 2.39 (Ubuntu 24.04, arm64); other compilers or
# maths libraries count differently.
set -e
here=$(cd "$(dirname "$0")" && pwd)
jobs=$(mktemp)
grep -v '^#' "$here/cost-baseline.txt" | grep . | cut -d' ' -f1,3- > "$jobs"
"$here/cost-a53.sh" "$1" "$2" "$jobs" > "$jobs.out"
fail=0
while read -r label n _; do
  want=$(awk -v l="$label" '$1 == l {print $2}' "$here/cost-baseline.txt")
  if [ $((n * 100)) -gt $((want * 102)) ] || [ $((n * 100)) -lt $((want * 98)) ]; then
    echo "$label: $n instructions per block, baseline $want: FAIL"
    fail=1
  else
    echo "$label: $n instructions per block, baseline $want"
  fi
done < "$jobs.out"
[ "$(wc -l < "$jobs.out")" -eq "$(wc -l < "$jobs")" ] || { echo "cases missing"; fail=1; }
rm -f "$jobs" "$jobs.out"
exit $fail
