#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "gain.h"

struct gain_state {
	double mult;
};

void gain_init(struct gain_state *state, double gain)
{
	state->mult = pow(10, gain / 20);
}

sample_t gain(struct gain_state *state, sample_t s)
{
	return s * state->mult;
}

void gain_effect_run(struct effect *e, sample_t *s)
{
	int i;
	struct gain_state *state = (struct gain_state *) e->data;
	for (i = 0; i < dsp_globals.channels; ++i)
		s[i] = gain(state, s[i]);
}

void gain_effect_plot(struct effect *e, int i)
{
	struct gain_state *state = (struct gain_state *) e->data;
	printf("H%d(f)=%.15e\n", i, 20 * log10(state->mult));
}

void gain_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * gain_effect_init(struct effect_info *ei, int argc, char **argv)
{
	struct effect *e;
	struct gain_state *state;

	if (argc != 2) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	e = malloc(sizeof(struct effect));
	e->run = gain_effect_run;
	e->plot = gain_effect_plot;
	e->destroy = gain_effect_destroy;
	state = malloc(sizeof(struct gain_state));
	gain_init(state, atof(argv[1]));
	e->data = state;
	return e;
}
