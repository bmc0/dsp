#ifndef _CROSSFEED_HRTF_H
#define _CROSSFEED_HRTF_H

#include "dsp.h"
#include "effect.h"

struct effect * crossfeed_hrtf_effect_init(struct effect_info *, struct stream_info *, char *, int, char **);

#endif
