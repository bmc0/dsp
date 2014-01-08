#ifndef _EFFECTS_MONO_H
#define _EFFECTS_MONO_H

#include "../dsp.h"
#include "../effect.h"

struct effect * mono_effect_init(struct effect_info *, struct stream_info *, int, char **);

#endif
