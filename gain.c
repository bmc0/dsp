#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "gain.h"
#include "util.h"

struct gain_state {
	int channel;
	sample_t mult;
};

void gain_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct gain_state *state = (struct gain_state *) e->data;
	if (state->channel == -1) {
		for (i = 0; i < samples; i += e->ostream.channels) {
			for (k = 0; k < e->ostream.channels; ++k) {
				if (GET_BIT(e->channel_selector, k))
					obuf[i + k] = ibuf[i + k] * state->mult;
				else
					obuf[i + k] = ibuf[i + k];
			}
		}
	}
	else {
		for (i = 0; i < samples; i += e->ostream.channels) {
			for (k = 0; k < e->ostream.channels; ++k) {
				if (k == state->channel)
					obuf[i + k] = ibuf[i + k] * state->mult;
				else
					obuf[i + k] = ibuf[i + k];
			}
		}
	}
}

void gain_effect_plot(struct effect *e, int i)
{
	struct gain_state *state = (struct gain_state *) e->data;
	int k;
	if (state->channel == -1) {
		for (k = 0; k < e->ostream.channels; ++k) {
			if (GET_BIT(e->channel_selector, k))
				printf("H%d_%d(f)=%.15e\n", k, i, 20 * log10(fabs(state->mult)));
			else
				printf("H%d_%d(f)=0\n", k, i);
		}
	}
	else {
		for (k = 0; k < e->ostream.channels; ++k) {
			if (k == state->channel)
				printf("H%d_%d(f)=%.15e\n", k, i, 20 * log10(fabs(state->mult)));
			else
				printf("H%d_%d(f)=0\n", k, i);
		}
	}
}

void gain_effect_destroy(struct effect *e)
{
	free(e->data);
	free(e->channel_selector);
}

struct effect * gain_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct gain_state *state;
	int channel = -1;
	char *g;

	if (argc != 2 && argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (argc == 3) {
		channel = atoi(argv[1]);
		CHECK_RANGE(channel >= 0 && channel < istream->channels, "channel", return NULL);
		g = argv[2];
	}
	else
		g = argv[1];
	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = gain_effect_run;
	e->plot = gain_effect_plot;
	e->destroy = gain_effect_destroy;
	state = calloc(1, sizeof(struct gain_state));
	state->channel = channel;
	if (strcmp(argv[0], "mult") == 0)
		state->mult = atof(g);
	else
		state->mult = pow(10, atof(g) / 20);
	e->data = state;
	return e;
}
