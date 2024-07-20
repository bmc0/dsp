#ifndef _MATRIX4_MB_H
#define _MATRIX4_MB_H

#include "dsp.h"
#include "effect.h"

struct effect * matrix4_mb_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
