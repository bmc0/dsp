#ifndef _ST2MS_H
#define _ST2MS_H

#include "dsp.h"
#include "effect.h"

enum {
	ST2MS_EFFECT_NUMBER_ST2MS = 1,
	ST2MS_EFFECT_NUMBER_MS2ST,
};

struct effect * st2ms_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
