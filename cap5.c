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

#include <complex.h>
#include <math.h>
#include "cap5.h"

void ap1_reset(struct ap1_state *state)
{
	state->i0 = 0.0;
	state->o0 = 0.0;
}

void ap2_reset(struct ap2_state *state)
{
	state->i0 = state->i1 = 0.0;
	state->o0 = state->o1 = 0.0;
}

void ap3_reset(struct ap3_state *state)
{
	ap2_reset(&state->ap2);
	ap1_reset(&state->ap1);
}

void cap5_reset(struct cap5_state *state)
{
	ap2_reset(&state->a1);
	ap3_reset(&state->a2);
}

/* doubly complementary 5th-order butterworth filters implemented as the
   sum (lowpass) and difference (highpass) of two allpass sections */
void cap5_init(struct cap5_state *state, double fs, double fc)
{
	const double fc_w = 2.0*fs*tan(M_PI*fc/fs);  /* pre-warped corner frequency */
	double complex p[3];  /* first two have a complex conjugate (not stored), third is real */

	for (int i = 0; i < 3; ++i) {
		const double theta = (2.0*(i+1)-1.0)*M_PI/10.0;
		p[i] = -sin(theta) + cos(theta)*I;  /* normalized pole in s-plane */
		p[i] = p[i]*fc_w;  /* scale */
		p[i] = (2.0*fs + p[i]) / (2.0*fs - p[i]);  /* bilinear transform */
		//LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: p[%d] = %f%+fi", __func__, fc, i, creal(p[i]), cimag(p[i]));
	}

	state->a2.ap2.c0 = -2.0*creal(p[0]);
	state->a2.ap2.c1 = creal(p[0])*creal(p[0]) + cimag(p[0])*cimag(p[0]);

	state->a1.c0 = -2.0*creal(p[1]);
	state->a1.c1 = creal(p[1])*creal(p[1]) + cimag(p[1])*cimag(p[1]);

	state->a2.ap1.c0 = -creal(p[2]);

	//LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a1: c0=%g  c1=%g", __func__, fc, state->a1.c0, state->a1.c1);
	//LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a2.ap2: c0=%g  c1=%g", __func__, fc, state->a2.ap2.c0, state->a2.ap2.c1);
	//LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a2.ap1: c0=%g", __func__, fc, state->a2.ap1.c0);

	cap5_reset(state);
}
