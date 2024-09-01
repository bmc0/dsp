#ifndef DSP_RESAMPLE_H
#define DSP_RESAMPLE_H

#include "dsp.h"
#include "effect.h"

struct effect * resample_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
