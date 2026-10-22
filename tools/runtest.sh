#!/bin/sh
# Run the native build headless with an input script.
#   tools/runtest.sh SCRIPT [seconds]
# Output goes to build/test/<script>.log
here=$(cd "$(dirname "$0")/.." && pwd)
script=$1
secs=${2:-90}
log="$here/build/test/$(basename "$script" .txt).log"
cd "$here/build/game" || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout "$secs" "$here/civ" --setup --script "$script" > "$log" 2>&1
echo "exit $?"
tail -c 800 "$log"
