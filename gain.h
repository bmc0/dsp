#ifndef _GAIN_H
#define _GAIN_H

#include "dsp.h"
#include "effect.h"

enum {
	GAIN_EFFECT_NUMBER_GAIN = 1,
	GAIN_EFFECT_NUMBER_MULT,
	GAIN_EFFECT_NUMBER_ADD,
};

struct effect * gain_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
