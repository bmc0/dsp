#ifndef _EFFECTS_GAIN_H
#define _EFFECTS_GAIN_H

#include "../dsp.h"
#include "../effect.h"

struct gain_state {
	double mult;
};

void gain_init(struct gain_state *, double);
sample_t gain(struct gain_state *, sample_t);
struct effect * gain_effect_init(struct effect_info *, int, char **);

#endif
