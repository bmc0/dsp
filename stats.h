#ifndef _STATS_H
#define _STATS_H

#include "dsp.h"
#include "effect.h"

struct effect * stats_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
