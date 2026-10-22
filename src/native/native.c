/* Support code for native replacements of game functions. */
#include <stdarg.h>
#include "native.h"

void call_lifted(uint16_t seg, func_t fn, int nargs, ...)
{
    uint16_t args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = (uint16_t)va_arg(ap, int);
    va_end(ap);
    for (int i = nargs - 1; i >= 0; i--)
        PUSH(args[i]);
    PUSH(CS);
    PUSH(0xFFF0);
    shadow_push(0xFFF0);
    CS = (uint16_t)(LOADSEG + seg);
    fn();
    shadow_pop();
    SP += 2 * nargs;
}
