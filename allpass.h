/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_ALLPASS_H
#define DSP_ALLPASS_H

#include "dsp.h"

struct ap1_state {
	sample_t c0;
	sample_t i0, o0;
};

struct ap2_state {
	sample_t c0, c1;
	sample_t i0, o0, i1, o1;
};

static inline void ap1_reset(struct ap1_state *state)
{
	state->i0 = 0.0;
	state->o0 = 0.0;
}

static inline void ap2_reset(struct ap2_state *state)
{
	state->i0 = state->i1 = 0.0;
	state->o0 = state->o1 = 0.0;
}

static inline sample_t ap1_run(struct ap1_state *state, sample_t s)
{
	sample_t r = state->i0
		+ state->c0 * (s - state->o0);

	state->i0 = s;
	state->o0 = r;

	return r;
}

static inline sample_t ap2_run(struct ap2_state *state, sample_t s)
{
	sample_t r = state->i1
		+ state->c0 * (state->i0 - state->o0)
		+ state->c1 * (s - state->o1);

	state->i1 = state->i0;
	state->i0 = s;

	state->o1 = state->o0;
	state->o0 = r;

	return r;
}

/*
 * Thiran fractional delay filters based on:
 * Koshita, et al., "A Simple Ladder Realization of Maximally Flat Allpass
 * Fractional Delay Filters," IEEE Transactions on Circuits and Systems II:
 * Express Briefs, vol. 61, no. 3, pp. 203-207, March 2014
 * DOI:10.1109/TCSII.2013.2296131
*/

struct thiran_ap_state {
	int n;
	struct {
		sample_t c0, c1, c2;
		sample_t m0, m1;
	} fb[];
};

static inline void thiran_ap_reset(struct thiran_ap_state *state)
{
	for (int k = 0; k < state->n; ++k)
		state->fb[k].m0 = 0.0;
}

static inline sample_t thiran_ap_run(struct thiran_ap_state *state, sample_t s)
{
	sample_t u = s;
	for (int k = 0; k < state->n; ++k) {
		u = u*state->fb[k].c0 + state->fb[k].m0;
		u *= state->fb[k].c1;
		state->fb[k].m1 = u;
	}
	sample_t y = 0.0;
	for (int k = state->n-1; k >= 0; --k) {
		y += 2.0*state->fb[k].m1;
		state->fb[k].m0 += y*state->fb[k].c2;
	}
	return s+y;
}

struct thiran_ap_state * thiran_ap_new(int, double);
void thiran_ap_plot(struct thiran_ap_state *);

#endif
