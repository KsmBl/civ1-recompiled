#!/bin/sh
# Multiplayer on one PC: a host and a joining game (each with its own copy
# of build/game) play a few turns; the joining player moves east every
# 0.7 s.  Prints the multiplayer log of both (CIV_MPDEBUG).
here=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/civ_mpcheck.$$
rm -rf "${W:?}"; mkdir -p "$W"
cp -r "$here/build/game" "$W/host"; cp -r "$here/build/game" "$W/join"
sed "s#@OUT@/#$W/#" "$here/tests/scripts/mp_host.txt" > "$W/host.txt"
sed "s#@OUT@/#$W/#" "$here/tests/scripts/mp_join.txt" > "$W/join.txt"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy CIV_MPDEBUG=1
(cd "$W/host" && CIV_MP_TEST_HOSTCIV=$HOSTCIV timeout 150 "$here/civ" --setup --script "$W/host.txt" > "$W/host.log" 2>&1) &
sleep 2
(cd "$W/join" && timeout 150 "$here/civ" --setup --script "$W/join.txt" > "$W/join.log" 2>&1) &
wait
echo "== host"; grep -a "^mp:" "$W/host.log" | grep -v "key 0020"
echo "== joining player"; grep -a "^mp:" "$W/join.log"
echo "(screenshots and logs in $W)"
