/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_CAPN_H
#define DSP_CAPN_H

#include <complex.h>
#include "dsp.h"
#include "allpass.h"

#define CAPN_MAX_ORDER 7
#define CAPN_MAX_AP (CAPN_MAX_ORDER/2+1)

enum capn_filter_type {
	CAPN_FILTER_BUTTERWORTH = 0,
	CAPN_FILTER_CHEBYSHEV1,
	CAPN_FILTER_CHEBYSHEV2,
	CAPN_FILTER_ELLIPTIC,
};

struct ap3_state {
	struct ap2_state ap2;
	struct ap1_state ap1;
};

struct ap4_state {
	struct ap2_state ap2[2];
};

struct cap5_state {
	struct ap2_state a1;
	struct ap3_state a2;
};

struct cap7_state {
	struct ap3_state a1;
	struct ap4_state a2;
};

void ap3_reset(struct ap3_state *);
void ap4_reset(struct ap4_state *);

static inline sample_t ap3_run(struct ap3_state *state, sample_t s)
{
	return ap1_run(&state->ap1, ap2_run(&state->ap2, s));
}

static inline sample_t ap4_run(struct ap4_state *state, sample_t s)
{
	return ap2_run(&state->ap2[1], ap2_run(&state->ap2[0], s));
}

int capn_ap(enum capn_filter_type, int, const double [2], double complex *);

#define CAPN_DEF_H_FUNCS(N, N_A1, N_A2) \
	void cap ## N ## _reset(struct cap ## N ## _state *); \
	void cap ## N ## _init(struct cap ## N ## _state *, double, double, const double complex [(N)/2+1]); \
	static inline void cap ## N ## _run(struct cap ## N ## _state *state, sample_t s, sample_t *lp, sample_t *hp) \
	{ \
		sample_t a1 = ap ## N_A1 ## _run(&state->a1, s); \
		sample_t a2 = ap ## N_A2 ## _run(&state->a2, s); \
		*lp = (a1+a2)*0.5; \
		*hp = (a1-a2)*0.5; \
	}

CAPN_DEF_H_FUNCS(5, 2, 3)
CAPN_DEF_H_FUNCS(7, 3, 4)

#endif
