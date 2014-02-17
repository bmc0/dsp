#ifndef _EFFECTS_REMIX_H
#define _EFFECTS_REMIX_H

#include "../dsp.h"
#include "../effect.h"

struct effect * remix_effect_init(struct effect_info *, struct stream_info *, char *, int, char **);

#endif
