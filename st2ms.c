#include <stdio.h>
#include <stdlib.h>
#include "st2ms.h"
#include "util.h"

struct st2ms_state {
	int c0, c1;
};

sample_t * st2ms_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, samples = *frames * e->ostream.channels;
	sample_t s0, s1;
	struct st2ms_state *state = (struct st2ms_state *) e->data;

	for (i = 0; i < samples; i += e->ostream.channels) {
		s0 = ibuf[i + state->c0];
		s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = (s0 + s1) * 0.5;
		ibuf[i + state->c1] = (s0 - s1) * 0.5;
	}
	return ibuf;
}

sample_t * ms2st_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, samples = *frames * e->ostream.channels;
	sample_t s0, s1;
	struct st2ms_state *state = (struct st2ms_state *) e->data;

	for (i = 0; i < samples; i += e->ostream.channels) {
		s0 = ibuf[i + state->c0];
		s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = (s0 + s1);
		ibuf[i + state->c1] = (s0 - s1);
	}
	return ibuf;
}

void st2ms_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * st2ms_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct st2ms_state *state;
	int i, n_channels = 0;

	if (argc != 1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	if (n_channels != 2) {
		LOG_FMT(LL_ERROR, "%s: error: number of input channels must be 2", argv[0]);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	switch (ei->effect_number) {
	case ST2MS_EFFECT_NUMBER_ST2MS:
		e->run = st2ms_effect_run;
		break;
	case ST2MS_EFFECT_NUMBER_MS2ST:
		e->run = ms2st_effect_run;
		break;
	default:
		LOG_FMT(LL_ERROR, "%s: BUG: unknown effect: %s (%d)", __FILE__, argv[0], ei->effect_number);
		free(e);
		return NULL;
	}
	e->destroy = st2ms_effect_destroy;

	state = calloc(1, sizeof(struct st2ms_state));
	state->c0 = state->c1 = -1;
	for (i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (state->c0 == -1)
				state->c0 = i;
			else
				state->c1 = i;
		}
	}
	e->data = state;

	return e;
}
