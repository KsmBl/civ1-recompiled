# Civilization (1991, v475.01) — recompiled to C

This directory turns the original DOS `CIV.EXE` (plus its overlays and the
MGRAPHIC / sound drivers) into C source that builds natively for Linux
(SDL2) and for DOS (DJGPP).  The original game files are still needed at
run time (graphics, text, and the program image itself).

**The generated source is derived from your copy of the game. Keep it
private; do not redistribute it.**

## Running

    make                 # builds ./civ (needs gcc, SDL2, zlib)
    ./civ                # reads civ.cfg: 1920x1080, widescreen, fullscreen

Useful options (all also valid in `civ.cfg` as `name = value`):

| option | meaning |
|---|---|
| `--res 1920x1080` | window / screen size |
| `--fullscreen` | fullscreen (Alt+Enter toggles) |
| `--widescreen` | wider map view filling the screen (square pixels) |
| `--classic` | original 320x200, letterboxed at 4:3 |
| `--zoom N` | widescreen: screen pixels per game pixel (default height/200) |
| `--mapwidth N` | world width in squares, even, 80 (original) to 160; height stays 50 |
| `--realtime` | turns pass on a clock (see below) |
| `--turnsecs S` | realtime: seconds per turn (default 3) |
| `--barbarians 0` | no barbarians: no raids, uprisings or barbarians from huts |
| `--sound none\|speaker` | sound driver (AdLib emulation is not implemented yet) |
| `--setup` | ask the original graphics / sound / mouse questions |
| `--gamedir DIR` | where the original files are |

Widescreen at 1920x1080 gives a 384x216 game screen at 5x: the map view is
19x13 squares instead of 15x12.  Other screens (city, reports, intro) keep
their 320x200 layout and are shown centred.

`make run` builds and starts the game.

## Added features

* **Produce gold** — in a city's "What shall we build" list (CHANGE on the
  city screen, or when production is due) pick **Gold: production to
  treasury**; picking anything else from the list switches it off again.
  The GOLD button (bottom right of the city screen, next to EXIT) and the
  **G** key toggle it too.  While it is on ("GOLD!") the city's production
  goes into your treasury each turn instead of the production box, so
  nothing gets built.  The setting is saved with the game (`CIVILn.NAT`
  next to `CIVILn.SVE`).
* **Auto-work settlers** — press **A** on an active Settlers unit.  It
  improves the 21 squares of the nearest own city (within 2 squares), or
  the 5x5 squares around it: road, then railroad, then irrigation (not on
  forest, swamp or jungle, where irrigating changes the terrain).  Orders
  the game refuses (no Railroad yet, no water nearby) are skipped and
  retried in a later pass once new work has been done.  Press any key while
  the settlers are active to take control back.

* **Mouse wheel zoom** (widescreen) — on the map, the wheel changes the
  size of a game pixel: at 1920x1080 5x (19x13 squares), 4x (25x16) or 3x
  (35x22).  The view never gets wider than half the world.
* **Drag-scroll** — drag the map with the left (or right) button; a click
  without moving still goes to the game as before.  Zoom and scrolling are
  applied while the game waits for your orders.

* **Real-time mode** (`realtime = 1`) — time passes even when you do
  nothing: each turn lasts a few seconds (default 3) and then ends by
  itself, the other civilizations move and the year advances.  Units with
  goto orders (**G**, then click the destination) walk on every turn, so
  fast units cover more ground than slow ones.  **Tab** switches to the next
  unit, **clicking** one of your units makes it the active one (cancelling
  its standing orders).  **[** / **-** slower, **]** / **=** faster, **F12**
  pause.  The bar at the bottom of the status panel shows the turn time.
  Dialogs, menus and city screens stop the clock.
* **Multiplayer over the network** — two PCs on the same network play one
  game in real time.  Choose **Multiplayer** in the start menu: **H** hosts
  a game, **F** searches the network (UDP broadcast) and lists the games
  found; **Enter** joins.  When the other player is in, the host presses
  **Enter** and starts a new game as usual; the joining player gets one of
  the other civilizations.  Both play at the same time; the turn clock (the
  host's `turnsecs`) never stops — open dialogs and city screens are closed
  when a turn is over.  Ports: TCP 28400, UDP 28401.  Both need the same
  `mapwidth`.

  How it works: the host's game is the real one.  At every turn start it
  sends the game state (the game's own save data plus the map) to the other
  PC, whose player keeps playing on it; the orders given there (unit keys,
  goto targets, city builds, gold settings) are sent to the host twice a
  second and carried out when that civilization moves on the host.  Nobody
  waits for anybody: the joining player's turn simply ends when the host's
  next turn begins, and orders that arrive late are used in the next turn.
  The joining player sees the final results of their moves (and the other
  civilizations' moves) turn by turn.  Diplomacy, taxes and research of the
  joining player's civilization are still decided by the computer.
* **Wider worlds** — `--mapwidth 160` (or `mapwidth = 160` in `civ.cfg`)
  makes the world up to twice as wide; it still wraps east-west.  New
  random worlds use the whole width (land amount scales with it), AI
  civilizations start anywhere, and EARTH is stretched to the chosen width.
  The minimap shows an 80 column window that follows the view; the world
  map and the end-of-game replay show 80 columns around your capital.
  Saved games of a wider world need the same `--mapwidth` to load (the map
  layers are in `CIVILn.WMP` next to the save) and can't be loaded by the
  original game.

Code: `src/native/capital.c`, `src/native/autowork.c`, `src/native/mapnav.c`,
`src/native/mapsize.c` (wide map support), `src/native/realtime.c`,
`src/native/mp.c` and `mp_net.c` (multiplayer), overlay drawing in
`src/native/overlay.c`; the hook points are in `tools/hooks.txt`.

## DOS build

    make dos             # -> civ.exe (DPMI host embedded, no CWSDPMI.EXE needed)

Uses the DJGPP cross compiler in `toolchain/djgpp` (prebuilt GCC 12.2 from
github.com/andrewwutw/build-djgpp) and CWSDPMI from delorie.com, both
downloaded into `toolchain/`.  Copy `civ.exe` into the game directory under
another name (e.g. `CIVR.EXE`, the original `CIV.EXE` must stay) and run it.
The DOS build uses real VGA mode 13h and the classic 320x200 layout.

## Layout

    tools/        lifter (x86-16 -> C) and analysis / test tools (Python)
      lift.py       the static recompiler
      seeds.txt     extra code entry points (only reachable via pointers)
      patches.txt   instruction constants, segments and displacements replaced
                    by expressions (widescreen, wider maps)
      hooks.txt     native functions called before given instructions
      overrides.txt lifted functions replaced by hand written C
    src/gen/      generated C, one file per code segment / overlay / driver
    src/native/   hand written replacements of game functions
    runtime/      CPU helpers, DOS/BIOS/VGA/PIT/mouse emulation, native
                  graphics driver (gfx.c), SDL and DOS platform layers,
                  8086 interpreter (fallback + reference)
    tests/        differential test harnesses; tests/scripts: input scripts
                  for the run checks below

`make lift` regenerates `src/gen` from `../CIV` (Python 3 with capstone and
unicorn: `tools/py` points at a virtualenv; recreate it with
`python3 -m venv venv && venv/bin/pip install capstone unicorn` and edit
`tools/py`).

## How it works

* `CIV.EXE` is EXEPACK-compressed; the root image, 23 MS-LINK overlays and
  the drivers are disassembled by recursive descent (1258 functions).
* Each function becomes a C function operating on an emulated register file
  and 1 MB of real-mode memory, so behaviour is exactly the original's.
  Flag computations are only emitted where a later instruction reads them,
  and compare+branch pairs become plain C conditions.
* A shadow call stack handles the program's tricks: thunks that pop their
  own return address, `_setargv`/`__chkstk` stack games, self-modifying
  driver thunks (executed through the interpreter), INT 3Fh overlay calls.
* The runtime emulates DOS (files, memory blocks, EXEC 4B03 driver loading,
  FCBs), BIOS (video, keyboard, timer), PIC/PIT/keyboard controller, VGA DAC
  and an INT 33h mouse.
* The MCGA graphics driver is reimplemented natively (`runtime/gfx.c`) so
  screen pages can be any size; in classic mode it can be checked against
  the original driver call by call (`CIV_GFX=shadow`).

## Adding features

Replace a lifted function with C: add its name (e.g. `f_1b05_0004`) to
`tools/overrides.txt`, run `make lift`, and define the function in
`src/native/*.c`.  The original stays callable as `lifted_<name>`; see
`src/native/mapview.c` for an example (arguments via `FARG(n)`, game
variables via `GW()/GB()`, finish with `RET_FAR(0)`).  For single constants
use `tools/patches.txt`.

`tools/dumpfunc.py ../CIV root SEG:OFF` prints the disassembly of a function.

## Testing

    make test       # every distinct instruction vs unicorn (102k cases)
    make functest   # whole pure functions vs unicorn
    tools/compare_runs.sh tests/scripts/monkey1d.txt
                                   # lifted vs interpreter, deterministic trace
    tools/widestore_check.sh       # wide map storage at width 80 vs the original
                                   # layout: game state must be the same
    tools/mp_check.sh              # multiplayer: host + joining game on one PC
    CIV_GFX=shadow  # native graphics driver vs original, per call

Test runs use a copy of the game in `build/game` (they save games).
`CIV_ORIGINAL=1` leaves the game's menus unchanged (no Multiplayer and Gold
entries), as the comparison runs need.  `CIV_MPDEBUG=1` logs multiplayer
turns and orders.

Debug aids: `CIV_WATCHDOG=N`, `CIV_DUMP=secs`, `CIV_TRACE=file`
(`CIV_TRACE_STACK=1` adds the call stack, lifted runs only), `CIV_ITRACE=n`,
`CIV_MAPTRACE=file` (map square writes), `CIV_GFXDEBUG=1`, `--script FILE`
(see runtime/script.c).  Analysis helpers: `tools/mapscan.py`,
`tools/widthscan.py` (map width constants), `tools/ctxat.sh MOD SEG:OFF`.

## Known limitations

* Widescreen: dialogs over the map are still placed for 320x200 (left of
  centre); some view-size constants deeper in unit/city code may remain
  unpatched — report odd behaviour near the right/bottom of the map view.
* No AdLib/Sound Blaster emulation yet (PC speaker works); text-mode setup
  screen is printed to the terminal, not drawn in the window.
* A rare timing-dependent "divide error" was seen once at startup in
  widescreen and could not be reproduced; the message shows a call stack.
* The DOS build compiles and links but could not be run here (DOSBox needs
  a display); untested on real DOS.
