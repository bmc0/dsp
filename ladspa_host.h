#ifndef DSP_LADSPA_HOST_H
#define DSP_LADSPA_HOST_H

#include "dsp.h"
#include "effect.h"

struct effect * ladspa_host_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
