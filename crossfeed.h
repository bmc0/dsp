#ifndef _CROSSFEED_H
#define _CROSSFEED_H

#include "dsp.h"
#include "effect.h"

struct effect * crossfeed_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
