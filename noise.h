#ifndef _NOISE_H
#define _NOISE_H

#include "dsp.h"
#include "effect.h"

struct effect * noise_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
