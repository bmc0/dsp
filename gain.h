#ifndef _EFFECTS_GAIN_H
#define _EFFECTS_GAIN_H

#include "dsp.h"
#include "effect.h"

struct effect * gain_effect_init(struct effect_info *, struct stream_info *, char *, int, char **);

#endif
