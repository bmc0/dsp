#ifndef DSP_CROSSFEED_H
#define DSP_CROSSFEED_H

#include "dsp.h"
#include "effect.h"

struct effect * crossfeed_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
