#ifndef DSP_DELAY_H
#define DSP_DELAY_H

#include "dsp.h"
#include "effect.h"

struct effect * delay_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
