#!/bin/sh
# Wide map storage at the original width must give the same game state as
# the normal layout: run tests/scripts/statedump.txt (dumps the state every
# 25 keys) both ways and compare the dumps.
here=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/civ_wscheck.$$
rm -rf "${W:?}"; mkdir -p "$W/nat" "$W/ws"
cp -r "$here/build/game" "$W/g1"; cp -r "$here/build/game" "$W/g2"
sed "s#@OUT@/#$W/nat/nat_#" "$here/tests/scripts/statedump.txt" > "$W/nat.txt"
sed "s#@OUT@/#$W/nat/ws_#" "$here/tests/scripts/statedump.txt" > "$W/ws.txt"
export CIV_DETERMINISTIC=1 CIV_ORIGINAL=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
(cd "$W/g1" && CIV_GFX=native timeout ${LIMIT:-900} "$here/civ" --setup --script "$W/nat.txt" > /dev/null 2>&1) &
(cd "$W/g2" && CIV_WIDESTORE=1 timeout ${LIMIT:-900} "$here/civ" --setup --script "$W/ws.txt" > /dev/null 2>&1) &
wait
"$here/tools/py" "$here/tools/sdcmp.py" "$W/nat"
rm -rf "${W:?}"
