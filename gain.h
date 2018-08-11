#ifndef _GAIN_H
#define _GAIN_H

#include "dsp.h"
#include "effect.h"

enum {
	GAIN_EFFECT_NUMBER_GAIN = 1,
	GAIN_EFFECT_NUMBER_MULT,
	GAIN_EFFECT_NUMBER_ADD,
};

struct effect * gain_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
