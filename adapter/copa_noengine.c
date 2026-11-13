/* A file without engines: what copa_host.c asks of copa_engine.c. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with
 * the OP-1 firmware permission (LICENSE). */
#include "copa.h"

int copa_engine_add(const copa_def *d, int index)
{
    (void)d;
    (void)index;
    return -1;
}
