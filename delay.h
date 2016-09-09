#ifndef _DELAY_H
#define _DELAY_H

#include "dsp.h"
#include "effect.h"

struct effect * delay_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
