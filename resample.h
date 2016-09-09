#ifndef _RESAMPLE_H
#define _RESAMPLE_H

#include "dsp.h"
#include "effect.h"

struct effect * resample_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
