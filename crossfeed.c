#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "crossfeed.h"
#include "biquad.h"
#include "util.h"

struct crossfeed_state {
	int c0, c1;
	sample_t direct_gain, cross_gain;
	struct biquad_state lp[2], hp[2];
};

sample_t * crossfeed_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, samples = *frames * e->ostream.channels;
	sample_t s0, s1;
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;

	for (i = 0; i < samples; i += e->ostream.channels) {
		s0 = ibuf[i + state->c0];
		s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = state->direct_gain * ((s0 * state->direct_gain)
			+ (biquad(&state->lp[0], s1) * state->cross_gain)
			+ (biquad(&state->hp[0], s0) * state->cross_gain));
		ibuf[i + state->c1] = state->direct_gain * ((s1 * state->direct_gain)
			+ (biquad(&state->lp[1], s0) * state->cross_gain)
			+ (biquad(&state->hp[1], s1) * state->cross_gain));
	}
	return ibuf;
}

void crossfeed_effect_reset(struct effect *e)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	biquad_reset(&state->lp[0]);
	biquad_reset(&state->lp[1]);
	biquad_reset(&state->hp[0]);
	biquad_reset(&state->hp[1]);
}

void crossfeed_effect_plot(struct effect *e, int i)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	printf("H0_%d(f)=%.15e\n", i, 20 * log10(state->direct_gain));
	printf("H1_%d(f)=%.15e\n", i, 20 * log10(state->direct_gain));
}

void crossfeed_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * crossfeed_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct crossfeed_state *state;
	char *endptr;
	double freq, sep_db, sep;
	int i, n_channels = 0;

	if (argc != 3) {
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

	freq = parse_freq(argv[1], &endptr);
	CHECK_ENDPTR(argv[1], endptr, "f0", return NULL);
	CHECK_FREQ(freq, istream->fs, "f0", return NULL);
	sep_db = strtod(argv[2], &endptr);
	CHECK_ENDPTR(argv[2], endptr, "separation", return NULL);
	CHECK_RANGE(sep_db >= 0.0, "separation", return NULL);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->run = crossfeed_effect_run;
	e->reset = crossfeed_effect_reset;
	e->plot = crossfeed_effect_plot;
	e->destroy = crossfeed_effect_destroy;
	state = calloc(1, sizeof(struct crossfeed_state));

	state->c0 = state->c1 = -1;
	for (i = 0; i < istream->channels; ++i) {  /* find input channel numbers */
		if (GET_BIT(channel_selector, i)) {
			if (state->c0 == -1)
				state->c0 = i;
			else
				state->c1 = i;
		}
	}
	sep = pow(10, sep_db / 20);
	state->direct_gain = sep / (1 + sep);
	state->cross_gain = 1 / (1 + sep);

	biquad_init_using_type(&state->lp[0], BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->lp[1], BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->hp[0], BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->hp[1], BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	e->data = state;

	return e;
}
