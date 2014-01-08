#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "gain.h"
#include "../util.h"

struct gain_state {
	int channel;
	double mult;
};

void gain_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples = *frames * e->ostream.channels, i;
	struct gain_state *state = (struct gain_state *) e->data;
	if (state->channel == -1) {
		for (i = 0; i < samples; ++i)
			obuf[i] = ibuf[i] * state->mult;
	}
	else {
		for (i = 0; i < samples; ++i) {
			if (i % e->ostream.channels == state->channel)
				obuf[i] = ibuf[i] * state->mult;
			else
				obuf[i] = ibuf[i];
		}
	}
}

void gain_effect_reset(struct effect *e)
{
	/* do nothing */
}

void gain_effect_plot(struct effect *e, int i)
{
	struct gain_state *state = (struct gain_state *) e->data;
	printf("H%d(f)=%.15e\n", i, 20 * log10(state->mult));
}

void gain_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	*frames = 0;
}

void gain_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * gain_effect_init(struct effect_info *ei, struct stream_info *istream, int argc, char **argv)
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
		CHECK_RANGE(channel >= 0 && channel < istream->channels, "channel");
		mult = pow(10, atof(argv[2]) / 20);
	}
	else
		mult = pow(10, atof(argv[1]) / 20);
	e = malloc(sizeof(struct effect));
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->ratio = 1.0;
	e->run = gain_effect_run;
	e->reset = gain_effect_reset;
	e->plot = gain_effect_plot;
	e->drain = gain_effect_drain;
	e->destroy = gain_effect_destroy;
	state = malloc(sizeof(struct gain_state));
	state->channel = channel;
	state->mult = mult;
	e->data = state;
	return e;
}
