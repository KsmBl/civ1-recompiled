/* Helpers for hand written replacements of lifted game functions. */
#pragma once
#include "cpu.h"
#include "funcs.h"
#include "wide.h"

/* the game's data segment (DGROUP); DS has this value inside game code,
   but not in interrupt handlers or the frame tick */
#define DGROUP ((uint16_t)(LOADSEG + 0x2A1B))

/* DGROUP variables of the game (via DS) */
#define GW(off) RW(DS, (off))
#define GB(off) RB(DS, (off))
#define VIEW_X ((int16_t)GW(0xe3ec))
#define VIEW_Y ((int16_t)GW(0xe67e))

/* far call of a lifted function in root segment `seg` with word args */
void call_lifted(uint16_t seg, func_t fn, int nargs, ...);
/* argument n of the current far function (before it pushed bp) */
#define FARG(n) RW(SS, (uint16_t)(SP + 4 + 2 * (n)))
