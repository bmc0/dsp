#ifndef _REMIX_H
#define _REMIX_H

#include "dsp.h"
#include "effect.h"

struct effect * remix_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
