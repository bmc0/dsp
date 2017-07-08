#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "noise.h"
#include "util.h"

struct noise_state {
	sample_t mult;
};

void noise_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	sample_t n1, n2;
	struct noise_state *state = (struct noise_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels) {
		for (k = 0; k < e->ostream.channels; ++k) {
			if (GET_BIT(e->channel_selector, k)) {
				n1 = (sample_t) pm_rand() * state->mult;
				n2 = (sample_t) pm_rand() * state->mult;
				obuf[i + k] = ibuf[i + k] + n1 - n2;
			}
			else
				obuf[i + k] = ibuf[i + k];
		}
	}
}

void noise_effect_destroy(struct effect *e)
{
	free(e->data);
	free(e->channel_selector);
}

struct effect * noise_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct noise_state *state;

	if (argc != 2) {
		LOG(LL_ERROR, "dsp: %s: usage %s\n", argv[0], ei->usage);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = noise_effect_run;
	e->destroy = noise_effect_destroy;
	state = calloc(1, sizeof(struct noise_state));
	state->mult = pow(10, atof(argv[1]) / 20) / PM_RAND_MAX;
	e->data = state;
	return e;
}
