#!/bin/sh
# Instructions per 128-frame block of plugin modes on the device's CPU (4-core Cortex-A53): builds
# tools/OscairnEffectsCost.cpp for -mcpu=cortex-a53 in an arm64 Linux container and counts the
# instructions of cost_run() with callgrind. Needs a configured build directory (its _deps).
#   scripts/cost-a53.sh <build dir> <image> <jobs file>
# With "-" for the image it runs here instead (an arm64 Linux machine with g++ and valgrind;
# shortcut: there the repository and build paths may not contain spaces).
# Each line of the jobs file: <label> <plugin-id> <rate> <signal|low|full|silence> ["Module/Name=value" ...]
set -e
repo=$(cd "$(dirname "$0")/../../.." && pwd)
build=$(cd "$1" && pwd)
jobs=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
run='
set -e
I="-I$S/src/oscairn-clap -I$S/libs/oscairn-clap/include -I$D/clap-src/include -I$D/simde-src -I$D/sst-basic-blocks-src/include -I$D/sst-basic-blocks-src/libs -I$D/sst-filters-src/include -I$D/sst-waveshapers-src/include -I$D/fmt-src/include -I$S/include"
F="-O3 -std=gnu++20 -mcpu=cortex-a53 -DNDEBUG -D_USE_MATH_DEFINES=1 -DFMT_HEADER_ONLY -DSST_CPPUTILS_UNAVAILABLE=1"
W=$(mktemp -d)
g++ $F $I -o $W/cost $S/src/oscairn-clap/tools/OscairnEffectsCost.cpp $S/src/oscairn-clap/OscairnEffects.cpp -lpthread
blocks=200
while read -r label id rate sig rest; do
  [ -z "$label" ] && continue
  eval "set -- $rest"
  valgrind --tool=callgrind --toggle-collect=cost_run --callgrind-out-file=$W/cg $W/cost "$id" "$rate" $blocks "$sig" "$@" >/dev/null 2>&1
  ir=$(callgrind_annotate $W/cg 2>/dev/null | awk "/PROGRAM TOTALS/ {gsub(\",\",\"\",\$1); print \$1}")
  echo "$label $((ir / blocks)) instructions per block"
done < $J
rm -rf $W
'
if [ "$2" = - ]; then
  S=$repo D=$build/_deps J=$jobs sh -c "$run"
else
  docker run --rm --platform linux/arm64 -v "$repo:/src:ro" -v "$build/_deps:/deps:ro" -v "$jobs:/jobs.txt:ro" \
    -e S=/src -e D=/deps -e J=/jobs.txt "$2" sh -c "$run"
fi
