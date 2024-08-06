#ifndef _NOISE_H
#define _NOISE_H

#include "dsp.h"
#include "effect.h"

struct effect * noise_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
