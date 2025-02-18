/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2024 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_EWMA_H
#define DSP_EWMA_H

#include <math.h>

struct ewma_state {
	double g0, m0;
};

#define EWMA_RISE_TIME(x) ((x)/1000.0/2.1972)  /* 10%-90% rise time in ms */

/* note: tc is the time constant in seconds */
static inline void ewma_init(struct ewma_state *state, double fs, double tc)
{
	state->g0 = 1.0-exp(-1.0/(fs*tc));
	state->m0 = 0.0;
}

static inline double ewma_run(struct ewma_state *state, double s)
{
	state->m0 = state->g0*(s - state->m0) + state->m0;
	return state->m0;
}

/* note: sf > 1.0 means a faster rise time */
static inline double ewma_run_scale(struct ewma_state *state, double s, double sf)
{
	double g = state->g0*sf;
	if (g > 0.39) g = 0.39;
	state->m0 = g*(s - state->m0) + state->m0;
	return state->m0;
}

static inline double ewma_run_scale_asym(struct ewma_state *state, double s, double rise_sf, double fall_sf)
{
	return (s >= state->m0) ? ewma_run_scale(state, s, rise_sf) : ewma_run_scale(state, s, fall_sf);
}

static inline double ewma_run_set_max(struct ewma_state *state, double s)
{
	if (s >= state->m0) s = ewma_run(state, s);
	else state->m0 = s;
	return s;
}

static inline double ewma_set(struct ewma_state *state, double s)
{
	state->m0 = s;
	return s;
}

static inline double ewma_get_last(struct ewma_state *state)
{
	return state->m0;
}

#endif
