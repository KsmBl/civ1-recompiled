#!/bin/sh
# Run a script deterministically with the lifted code and with the reference
# interpreter and compare the event traces.
#   tools/compare_runs.sh SCRIPT
here=$(cd "$(dirname "$0")/.." && pwd)
T=$here/build/test; mkdir -p "$T"
cd "$here/build/game" || exit 1
export CIV_DETERMINISTIC=1 CIV_ORIGINAL=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
CIV_TRACE=$T/trace_lift.txt timeout 1200 "$here/civ" --setup --script "$1" > $T/lift.log 2>&1
CIV_INTERP=1 CIV_TRACE=$T/trace_interp.txt timeout 3600 "$here/civ" --setup --script "$1" > $T/interp.log 2>&1
wc -l $T/trace_lift.txt $T/trace_interp.txt
if cmp $T/trace_lift.txt $T/trace_interp.txt; then echo IDENTICAL; else
  line=$(cmp $T/trace_lift.txt $T/trace_interp.txt | awk '{print $NF}')
  echo "first difference at event $line"; fi
