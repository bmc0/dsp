/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_WINDOW_H
#define DSP_WINDOW_H

#include <math.h>

enum window_id {
	WINDOW_RECT = 0,
	WINDOW_HANN,
	WINDOW_HAMMING,
	WINDOW_BLACKMAN,
	WINDOW_NUTTALL4C1,
	WINDOW_ALBRECHT9C3,
	WINDOW_KAISER,
};

static inline double norm_sinc(double x, double fc)
{
	if (fabs(x) < 1e-9) return fc;
	return sin(M_PI*fc*x) / (M_PI*x);
}

double window_hann(double);
double window_hamming(double);
double window_blackman(double);
double window_cosine_sum(double, const double *, int);
double window_nuttall4c1(double);
double window_albrecht9c3(double);
double window_kaiser(double, double);

double window(enum window_id, double, double);
double window_width(enum window_id, double);  /* main lobe width in bins */

#endif
