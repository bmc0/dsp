#ifndef _FIR_P_H
#define _FIR_P_H

#include "dsp.h"
#include "effect.h"

struct effect * fir_p_effect_init_with_filter(const struct effect_info *, const struct stream_info *, const char *, sample_t *, int, ssize_t, ssize_t);
struct effect * fir_p_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
