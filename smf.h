/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_SMF_H
#define DSP_SMF_H

#include <math.h>

/*
 * Self-modulating filter based on:
 * Simper, Andrew, "Dynamic Smoothing Using Self-Modulating Filter," Dec. 2016.
*/

struct smf_state {
	double g0, m0, m1, c0, c1;
};

#define SMF_RISE_TIME(x) (0.349/((x)/1000.0))

static inline void smf_reset(struct smf_state *state)
{
	state->m0 = state->m1 = 0.0;
}

static inline void smf_asym_init(struct smf_state *state, double fs, double f0, double sens_rise, double sens_fall)
{
	const double gc = tan(M_PI*(f0/fs));
	state->g0 = 2.0*gc/(1.0+gc);
	state->c0 = sens_rise*4.0;
	state->c1 = sens_fall*4.0;
	smf_reset(state);
}

static inline void smf_init(struct smf_state *state, double fs, double f0, double sens)
{
	smf_asym_init(state, fs, f0, sens, sens);
}

static inline double smf_run_c(struct smf_state *state, double s, const double c)
{
	double g = state->g0 + c*fabs(state->m0 - state->m1);
	if (g > 1.0) g = 1.0;
	state->m0 = state->m0 + g*(s - state->m0);
	state->m1 = state->m1 + g*(state->m0 - state->m1);
	return state->m1;
}

static inline double smf_asym_run(struct smf_state *state, double s)
{
	const double c = (s > state->m1) ? state->c0 : state->c1;
	return smf_run_c(state, s, c);
}

static inline double smf_run(struct smf_state *state, double s)
{
	return smf_run_c(state, s, state->c0);
}

#endif
