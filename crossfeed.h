#ifndef _EFFECTS_CROSSFEED_H
#define _EFFECTS_CROSSFEED_H

#include "dsp.h"
#include "effect.h"

struct effect * crossfeed_effect_init(struct effect_info *, struct stream_info *, char *, int, char **);

#endif
