#ifndef _GAIN_H
#define _GAIN_H

#include "dsp.h"
#include "effect.h"

struct effect * gain_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
