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

#include <complex.h>
#include <math.h>
#include "cap5.h"

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

void cap5_butterworth_ap(double complex ap[3])
{
	for (int i = 0; i < 3; ++i) {
		const double theta = (2*i+1)*M_PI/10.0;
		ap[i] = -sin(theta) + cos(theta)*I;  /* normalized pole in s-plane */
	}
}

void cap5_chebyshev_ap(int gen_type2, double stop_dB, double complex ap[3])
{
	if (stop_dB > 100.0) {
		cap5_butterworth_ap(ap);
		return;
	}
	const double epsilon = sqrt(pow(10.0, stop_dB/10.0) - 1.0);
	const double sigma = asinh(epsilon)/5.0;
	for (int i = 0; i < 3; ++i) {
		const double theta = (2*i+1)*M_PI/10.0;
		ap[i] = -sinh(sigma)*sin(theta) + cosh(sigma)*cos(theta)*I;  /* normalized pole in s-plane */
		ap[i] = ap[i] / cosh(acosh(epsilon)/5.0);  /* scale so H(1) = sqrt(0.5) */
		if (gen_type2) ap[i] = 1.0/ap[i];
	}
}

void cap5_elliptic_ap(double complex ap[3])
{
	/* 35dB stopband attenuation for low pass; 45dB for high pass */
	ap[0] = -0.185287191997037 + 0.990129317340409*I;
	ap[1] = -0.686015538373767 + 0.810354587786414*I;
	ap[2] = -1.118174003343493;
}

void cap5_init(struct cap5_state *state, double fs, double fc, const double complex ap[3])
{
	const double fc_w = 2.0*fs*tan(M_PI*fc/fs);  /* pre-warped corner frequency */
	double complex p[3];
	for (int i = 0; i < 3; ++i) {
		p[i] = ap[i]*fc_w;  /* scale */
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
