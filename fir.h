#ifndef DSP_FIR_H
#define DSP_FIR_H

#include "dsp.h"
#include "effect.h"

struct effect * fir_effect_init_with_filter(const struct effect_info *, const struct stream_info *, const char *, sample_t *, int, ssize_t, int);
sample_t * fir_read_filter(const struct effect_info *, const char *, const char *, int, int *, ssize_t *);
struct effect * fir_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
