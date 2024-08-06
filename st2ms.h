#ifndef _ST2MS_H
#define _ST2MS_H

#include "dsp.h"
#include "effect.h"

enum {
	ST2MS_EFFECT_NUMBER_ST2MS = 1,
	ST2MS_EFFECT_NUMBER_MS2ST,
};

struct effect * st2ms_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
