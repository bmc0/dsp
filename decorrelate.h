#ifndef _DECORRELATE_H
#define _DECORRELATE_H

#include "dsp.h"
#include "effect.h"

struct effect * decorrelate_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
