#ifndef _MATRIX4_H
#define _MATRIX4_H

#include "dsp.h"
#include "effect.h"

struct effect * matrix4_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
