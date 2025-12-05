/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
	BIQUAD_LOWPASS_TRANSFORM,
	BIQUAD_HIGHPASS_TRANSFORM,
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

#define BIQUAD_PLOT_FMT "%.15e+%.15e*exp(-j*w)+%.15e*exp(-2.0*j*w))/(1.0+%.15e*exp(-j*w)+%.15e*exp(-2.0*j*w)"
#define BIQUAD_PLOT_FMT_ARGS(s) (s)->c0, (s)->c1, (s)->c2, (s)->c3, (s)->c4

#define BIQUAD_EFFECT_INFO \
	{ "lowpass_1",          "f0[k]",                             biquad_effect_init, BIQUAD_LOWPASS_1 }, \
	{ "highpass_1",         "f0[k]",                             biquad_effect_init, BIQUAD_HIGHPASS_1 }, \
	{ "allpass_1",          "f0[k]",                             biquad_effect_init, BIQUAD_ALLPASS_1 }, \
	{ "lowshelf_1",         "f0[k] gain",                        biquad_effect_init, BIQUAD_LOWSHELF_1 }, \
	{ "highshelf_1",        "f0[k] gain",                        biquad_effect_init, BIQUAD_HIGHSHELF_1 }, \
	{ "lowpass_1p",         "f0[k]",                             biquad_effect_init, BIQUAD_LOWPASS_1P }, \
	{ "lowpass",            "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_LOWPASS }, \
	{ "highpass",           "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_HIGHPASS }, \
	{ "bandpass_skirt",     "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_BANDPASS_SKIRT }, \
	{ "bandpass_peak",      "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_BANDPASS_PEAK }, \
	{ "notch",              "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_NOTCH }, \
	{ "allpass",            "f0[k] width[q|o|h|k]",              biquad_effect_init, BIQUAD_ALLPASS }, \
	{ "eq",                 "f0[k] width[q|o|h|k] gain",         biquad_effect_init, BIQUAD_PEAK }, \
	{ "lowshelf",           "f0[k] width[q|s|d|o|h|k] gain",     biquad_effect_init, BIQUAD_LOWSHELF }, \
	{ "highshelf",          "f0[k] width[q|s|d|o|h|k] gain",     biquad_effect_init, BIQUAD_HIGHSHELF }, \
	{ "lowpass_transform",  "fz[k] width_z[q] fp[k] width_p[q]", biquad_effect_init, BIQUAD_LOWPASS_TRANSFORM }, \
	{ "highpass_transform", "fz[k] width_z[q] fp[k] width_p[q]", biquad_effect_init, BIQUAD_HIGHPASS_TRANSFORM }, \
	{ "linkwitz_transform", "fz[k] width_z[q] fp[k] width_p[q]", biquad_effect_init, BIQUAD_HIGHPASS_TRANSFORM }, \
	{ "deemph",             "",                                  biquad_effect_init, BIQUAD_DEEMPH }, \
	{ "biquad",             "b0 b1 b2 a0 a1 a2",                 biquad_effect_init, BIQUAD_BIQUAD }

#endif
