#ifndef _REVERB_H
#define _REVERB_H

#include "dsp.h"
#include "effect.h"

struct effect * reverb_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
