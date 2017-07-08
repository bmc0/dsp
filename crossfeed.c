#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "crossfeed.h"
#include "biquad.h"
#include "util.h"

struct crossfeed_state {
	int type;
	sample_t direct_gain;
	sample_t cross_gain;
	struct biquad_state f0_c0;  /* lowpass for channel 0 */
	struct biquad_state f0_c1;  /* lowpass for channel 1 */
	struct biquad_state f1_c0;  /* highpass for channel 0 */
	struct biquad_state f1_c1;  /* highpass for channel 1 */
};

void crossfeed_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples, i;
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;

	samples = *frames * 2;
	for (i = 0; i < samples; i += 2) {
		obuf[i + 0] = (ibuf[i + 0] * state->direct_gain)
			+ (biquad(&state->f0_c0, ibuf[i + 1]) * state->cross_gain)
			+ (biquad(&state->f1_c0, ibuf[i + 0]) * state->cross_gain);
		obuf[i + 0] *= state->direct_gain;
		obuf[i + 1] = (ibuf[i + 1] * state->direct_gain)
			+ (biquad(&state->f0_c1, ibuf[i + 0]) * state->cross_gain)
			+ (biquad(&state->f1_c1, ibuf[i + 1]) * state->cross_gain);
		obuf[i + 1] *= state->direct_gain;
	}
}

void crossfeed_effect_reset(struct effect *e)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	biquad_reset(&state->f0_c0);
	biquad_reset(&state->f0_c1);
	biquad_reset(&state->f1_c0);
	biquad_reset(&state->f1_c1);
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
	double freq, sep_db, sep;

	if (argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	freq = parse_freq(argv[1]);
	sep_db = atof(argv[2]);

	if (istream->channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: channels != 2\n", argv[0]);
		return NULL;
	}
	CHECK_RANGE(freq >= 0.0 && freq < (double) istream->fs / 2.0, "f0", return NULL);
	CHECK_RANGE(sep_db >= 0.0, "separation", return NULL);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = crossfeed_effect_run;
	e->reset = crossfeed_effect_reset;
	e->plot = crossfeed_effect_plot;
	e->destroy = crossfeed_effect_destroy;
	state = calloc(1, sizeof(struct crossfeed_state));

	sep = pow(10, sep_db / 20);
	state->direct_gain = sep / (1 + sep);
	state->cross_gain = 1 / (1 + sep);

	biquad_init_using_type(&state->f0_c0, BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->f0_c1, BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->f1_c0, BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->f1_c1, BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	e->data = state;

	return e;
}
