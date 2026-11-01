#!/usr/bin/env bash
# The stock-vs-fork gate (doc/oscairn-plan.md, section 8): render every stock bus effect with
# this branch's headers and with the headers of the upstream commit the branch is based on, and
# require identical output. Usage: stock-gate.sh [build-dir]. STOCK_GATE_BASE overrides the base
# commit; STOCK_GATE_CMAKE_ARGS adds configure arguments.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
build=${1:-$root/build-stock-gate}
base=${STOCK_GATE_BASE:-$(git -C "$root" merge-base HEAD origin/main)}
echo "stock gate: base $(git -C "$root" rev-parse --short "$base"), head $(git -C "$root" rev-parse --short HEAD)"
echo "stock gate: files changed under include/ since the base: $(git -C "$root" diff --name-only "$base" -- include | wc -l | tr -d ' ')"
rm -rf "$build/base-include"
mkdir -p "$build/base-include"
git -C "$root" archive "$base" include | tar -x -C "$build/base-include"
# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSST_EFFECTS_BUILD_OSCAIRN=ON \
  -DOSCAIRN_STOCK_BASE_INCLUDE="$build/base-include/include" ${STOCK_GATE_CMAKE_ARGS:-} > /dev/null
if ! cmake --build "$build" --target oscairn-stock-render oscairn-stock-render-base > "$build/stock-build.log" 2>&1; then
  tail -60 "$build/stock-build.log" >&2
  echo "stock gate: build failed" >&2
  exit 1
fi
for which in fork base; do
  exe="$build/src/oscairn-clap/oscairn-stock-render"
  [[ "$which" == base ]] && exe="$exe-base"
  if ! "$exe" > "$build/stock-$which.txt"; then
    echo "stock gate: the $which render program failed" >&2
    exit 1
  fi
done
lines=$(wc -l < "$build/stock-fork.txt" | tr -d ' ')
if [[ "$lines" -ne 90 ]]; then
  echo "stock gate: expected 90 renders, got $lines" >&2
  exit 1
fi
if ! diff -u "$build/stock-base.txt" "$build/stock-fork.txt"; then
  echo "stock gate: FAILED, stock effects render differently from upstream" >&2
  exit 1
fi
echo "stock gate: passed, $lines renders (10 effects x 9 parameter sets) identical"
