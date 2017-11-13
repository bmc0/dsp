#ifndef _BIQUAD_H
#define _BIQUAD_H

#include "dsp.h"
#include "effect.h"

/* See http://musicdsp.org/files/Audio-EQ-Cookbook.txt */

/* Use the transposed direct form 2 implementation instead of the direct form 1 implementation */
#define BIQUAD_USE_TDF_2 1

enum {
	/* For biquad_init_using_type() and effect_info->effect_number */
	BIQUAD_LOWPASS_1,
	BIQUAD_HIGHPASS_1,
	BIQUAD_LOWPASS,
	BIQUAD_HIGHPASS,
	BIQUAD_BANDPASS_SKIRT,
	BIQUAD_BANDPASS_PEAK,
	BIQUAD_NOTCH,
	BIQUAD_ALLPASS,
	BIQUAD_PEAK,
	BIQUAD_LOWSHELF,
	BIQUAD_HIGHSHELF,
	BIQUAD_LINKWITZ_TRANSFORM,
	/* Only for effect_info->effect_number */
	BIQUAD_DEEMPH,
	BIQUAD_BIQUAD,
};

enum {
	BIQUAD_WIDTH_Q = 1,
	BIQUAD_WIDTH_SLOPE,
	BIQUAD_WIDTH_SLOPE_DB,
	BIQUAD_WIDTH_BW_OCT,
	BIQUAD_WIDTH_BW_HZ,
};

struct biquad_state {
	sample_t c0, c1, c2, c3, c4;
#if BIQUAD_USE_TDF_2
	sample_t m0, m1;
#else
	sample_t i0, i1, o0, o1;
#endif
};

void biquad_init(struct biquad_state *, double, double, double, double, double, double);
void biquad_reset(struct biquad_state *);
void biquad_init_using_type(struct biquad_state *, int, double, double, double, double, double, int);
struct effect * biquad_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

static __inline__ sample_t biquad(struct biquad_state *state, sample_t s)
{
#if BIQUAD_USE_TDF_2
	sample_t r = (state->c0 * s) + state->m0;
	state->m0 = state->m1 + (state->c1 * s) - (state->c3 * r);
	state->m1 = (state->c2 * s) - (state->c4 * r);
#else
	sample_t r = (state->c0 * s) + (state->c1 * state->i0) + (state->c2 * state->i1) - (state->c3 * state->o0) - (state->c4 * state->o1);

	state->i1 = state->i0;
	state->i0 = s;

	state->o1 = state->o0;
	state->o0 = r;
#endif
	return r;
}

#endif
