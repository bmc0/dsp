#ifndef _STATS_H
#define _STATS_H

#include "dsp.h"
#include "effect.h"

struct effect * stats_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
