#ifndef _BIQUAD_H
#define _BIQUAD_H

#include "dsp.h"
#include "effect.h"

/* See http://musicdsp.org/files/Audio-EQ-Cookbook.txt */

/* Use the transposed direct form 2 implementation instead of the direct form 1 implementation */
#define BIQUAD_USE_TDF_2 1

enum {
	BIQUAD_NONE = 0,  /* dummy value for effect init function; do not use */
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
};

enum {
	BIQUAD_WIDTH_Q = 1,
	BIQUAD_WIDTH_SLOPE,
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
sample_t biquad(struct biquad_state *, sample_t);
struct effect * biquad_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
