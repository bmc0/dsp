#ifndef DSP_MATRIX4_H
#define DSP_MATRIX4_H

#include "dsp.h"
#include "effect.h"

struct effect * matrix4_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
