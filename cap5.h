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

#ifndef DSP_CAP5_H
#define DSP_CAP5_H

#include "dsp.h"

struct ap1_state {
	sample_t c0;
	sample_t i0, o0;
};

struct ap2_state {
	sample_t c0, c1;
	sample_t i0, i1, o0, o1;
};

struct ap3_state {
	struct ap2_state ap2;
	struct ap1_state ap1;
};

struct cap5_state {
	struct ap2_state a1;
	struct ap3_state a2;
};

void ap1_reset(struct ap1_state *);
void ap2_reset(struct ap2_state *);
void ap3_reset(struct ap3_state *);
void cap5_reset(struct cap5_state *);
void cap5_init(struct cap5_state *, double, double);

static inline sample_t ap1_run(struct ap1_state *state, sample_t s)
{
	sample_t r = state->c0 * (s - state->o0)
		+ state->i0;

	state->i0 = s;
	state->o0 = r;

	return r;
}

static inline sample_t ap2_run(struct ap2_state *state, sample_t s)
{
	sample_t r = state->c1 * (s - state->o1)
		+ state->c0 * (state->i0 - state->o0)
		+ state->i1;

	state->i1 = state->i0;
	state->i0 = s;

	state->o1 = state->o0;
	state->o0 = r;

	return r;
}

static inline sample_t ap3_run(struct ap3_state *state, sample_t s)
{
	return ap1_run(&state->ap1, ap2_run(&state->ap2, s));
}

static inline void cap5_run(struct cap5_state *state, sample_t s, sample_t *lp, sample_t *hp)
{
	sample_t a1 = ap2_run(&state->a1, s);
	sample_t a2 = ap3_run(&state->a2, s);
	*lp = (a1+a2)*0.5;
	*hp = (a1-a2)*0.5;
}

#endif
