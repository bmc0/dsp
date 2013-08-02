#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "crossfeed.h"
#include "biquad.h"

struct crossfeed_state {
	int type;
	double direct_gain;
	double cross_gain;
	struct biquad_state f0_c0;  /* lowpass for channel 0 */
	struct biquad_state f0_c1;  /* lowpass for channel 1 */
	struct biquad_state f1_c0;  /* highpass for channel 0 */
	struct biquad_state f1_c1;  /* highpass for channel 1 */
};

void crossfeed_init(struct crossfeed_state *state, double freq, double sep_db)
{
	double sep = pow(10, sep_db / 20);
	state->direct_gain = sep / (1 + sep);
	state->cross_gain = 1 / (1 + sep);

	biquad_init_using_type(&state->f0_c0, BIQUAD_LOWPASS_1, dsp_globals.fs, freq, 0, 0);
	biquad_init_using_type(&state->f0_c1, BIQUAD_LOWPASS_1, dsp_globals.fs, freq, 0, 0);
	biquad_init_using_type(&state->f1_c0, BIQUAD_HIGHPASS_1, dsp_globals.fs, freq, 0, 0);
	biquad_init_using_type(&state->f1_c1, BIQUAD_HIGHPASS_1, dsp_globals.fs, freq, 0, 0);
}

void crossfeed_effect_run(struct effect *e, sample_t s[2])
{
	if (dsp_globals.channels != 2)
		return;

	sample_t cross[2] = { s[1], s[0] };
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;

	s[0] = (s[0] * state->direct_gain)
		+ (biquad(&state->f0_c0, cross[0]) * state->cross_gain)
		+ (biquad(&state->f1_c0, s[0]) * state->cross_gain);
	s[0] *= state->direct_gain;
	s[1] = (s[1] * state->direct_gain)
		+ (biquad(&state->f0_c1, cross[1]) * state->cross_gain)
		+ (biquad(&state->f1_c1, s[1]) * state->cross_gain);
	s[1] *= state->direct_gain;
}

void crossfeed_effect_plot(struct effect *e, int i)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	printf("H%d(f)=%.15e\n", i, 20 * log10(state->direct_gain));
}

void crossfeed_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * crossfeed_effect_init(struct effect_info *ei, int argc, char **argv)
{
	struct effect *e;
	struct crossfeed_state *state;
	double freq, sep_db;

	if (argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	freq = atof(argv[1]);
	sep_db = atof(argv[2]);

	if (dsp_globals.channels != 2)
		LOG(LL_ERROR, "dsp: %s: warning: channels != 2; crossfeed will have no effect\n", argv[0]);
	if (freq < 0.0 || freq > (double) dsp_globals.fs / 2.0) {
		LOG(LL_ERROR, "dsp: %s: error: freq out of range\n", argv[0]);
		return NULL;
	}
	if (sep_db < 0.0) {
		LOG(LL_ERROR, "dsp: %s: error: separation out of range\n", argv[0]);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->run = crossfeed_effect_run;
	e->plot = crossfeed_effect_plot;
	e->destroy = crossfeed_effect_destroy;
	state = calloc(1, sizeof(struct crossfeed_state));
	crossfeed_init(state, freq, sep_db);
	e->data = state;
	return e;
}
