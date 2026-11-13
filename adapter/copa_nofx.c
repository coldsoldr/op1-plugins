/* A file without effects: what copa_host.c asks of copa_fx.c. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the
 * OP-1 firmware permission (LICENSE). */
#include "copa.h"

int copa_fx_add(const copa_def *d, int index)
{
    (void)d;
    (void)index;
    return -1;
}

int copa_fx_buffers(uint32_t bytes)
{
    (void)bytes;
    return -1;
}
