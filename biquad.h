/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2024 Michael Barbour <barbour.michael.0@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef DSP_BIQUAD_H
#define DSP_BIQUAD_H

#include "dsp.h"
#include "effect.h"

/* See http://musicdsp.org/files/Audio-EQ-Cookbook.txt */

/* Use the transposed direct form 2 implementation instead of the direct form 1 implementation */
#define BIQUAD_USE_TDF_2 1

enum {
	/* For biquad_init_using_type() and effect_info->effect_number */
	BIQUAD_LOWPASS_1 = 1,
	BIQUAD_HIGHPASS_1,
	BIQUAD_ALLPASS_1,
	BIQUAD_LOWSHELF_1,
	BIQUAD_HIGHSHELF_1,
	BIQUAD_LOWPASS_1P,
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
struct effect * biquad_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

static inline sample_t biquad(struct biquad_state *state, sample_t s)
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
