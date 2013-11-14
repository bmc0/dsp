#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "gain.h"
#include "../util.h"

struct gain_state {
	int channel;
	double mult;
};

static sample_t gain(struct gain_state *state, sample_t s)
{
	return s * state->mult;
}

void gain_effect_run(struct effect *e, sample_t *s)
{
	int i;
	struct gain_state *state = (struct gain_state *) e->data;
	if (state->channel == -1) {
		for (i = 0; i < dsp_globals.channels; ++i)
			s[i] = gain(state, s[i]);
	}
	else
		s[state->channel] = gain(state, s[state->channel]);
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
	int channel = -1;
	double mult;

	if (argc != 2 && argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (argc == 3) {
		channel = atoi(argv[1]);
		CHECK_RANGE(channel >= 0 && channel < dsp_globals.channels, "channel");
		mult = pow(10, atof(argv[2]) / 20);
	}
	else
		mult = pow(10, atof(argv[1]) / 20);
	e = malloc(sizeof(struct effect));
	e->run = gain_effect_run;
	e->plot = gain_effect_plot;
	e->destroy = gain_effect_destroy;
	state = malloc(sizeof(struct gain_state));
	state->channel = channel;
	state->mult = mult;
	e->data = state;
	return e;
}
