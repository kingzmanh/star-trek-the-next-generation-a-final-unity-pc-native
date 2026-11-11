/* Native replacements for game / library functions that talk to PC hardware
 * or modify their own code.  Each one stands in for the whole original
 * function (listed in tools/native_<module>.txt as "<hex addr> <C name>",
 * or "<hex addr> <C name> wrap" to keep the original callable through
 * rt_wrap_orig) and works on the guest register file like the original:
 * Watcom register convention (eax, edx, ebx, ecx), result in eax, and it pops
 * its own return address.  See libs/examples for real ones. */
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))   /* return, popping n bytes of arguments */
