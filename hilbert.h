#ifndef DSP_HILBERT_H
#define DSP_HILBERT_H

#include "dsp.h"
#include "effect.h"

struct effect * hilbert_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
