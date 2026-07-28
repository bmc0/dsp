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

#include <math.h>
#include <float.h>
#include "window.h"
#include "util.h"

double window_hann(double x)
{
	if (x >= 1.0 || x <= 0.0) return 0.0;
	return 0.5 - 0.5*cos((2*M_PI)*x);
}

double window_hamming(double x)
{
	if (x > 1.0 || x < 0.0) return 0.0;
	return 0.54 - 0.46*cos((2*M_PI)*x);
}

double window_blackman(double x)
{
	if (x >= 1.0 || x <= 0.0) return 0.0;
	return 0.42 - 0.5*cos((2*M_PI)*x) + 0.08*cos((4*M_PI)*x);
}

double window_cosine_sum(double x, const double *a, int n)
{
	if (x > 1.0 || x < 0.0) return 0.0;
	double w = a[0];
	for (int i = 1; i < n; ++i) {
		const double c = (i&1) ? -a[i] : a[i];
		w += c*cos((2*M_PI)*i*x);
	}
	return w;
}

double window_nuttall4c1(double x)
{
	const double a[] = {0.355768, 0.487396, 0.144232, 0.012604};
	if (x >= 1.0 || x <= 0.0) return 0.0;
	return window_cosine_sum(x, a, LENGTH(a));
}

double window_albrecht9c3(double x)
{
	const double a[] = {
		2.318028013590306028393e-1, 3.932575471789488615081e-1, 2.385434764970747429454e-1,
		1.014370437785239811268e-1, 2.911516061918003918645e-2, 5.280988177252078698806e-3,
		5.382909093381945363528e-4, 2.442086527507867730168e-5, 2.706153764205043532817e-7,
	};
	if (x >= 1.0 || x <= 0.0) return 0.0;
	return window_cosine_sum(x, a, LENGTH(a));
}

static double mod_bessel0(const double x)
{
	const double x_2 = x/2.0;
	double t = 1.0, s = 1.0;
	for (int k = 1; t > DBL_EPSILON*s; ++k) {
		double y = x_2 / k;
		t *= y*y;
		s += t;
	}
	return s;
}

double window_kaiser(double x, double beta)
{
	if (x > 1.0 || x < 0.0) return 0.0;
	const double y = 2.0*x-1.0;
	return mod_bessel0(beta*sqrt(1.0-y*y))/mod_bessel0(beta);
}

double window_width(enum window_id wn, double param)
{
	switch (wn) {
	case WINDOW_HANN:        return 4.0;
	case WINDOW_HAMMING:     return 4.0;
	case WINDOW_BLACKMAN:    return 6.0;
	case WINDOW_NUTTALL4C1:  return 8.0;
	case WINDOW_ALBRECHT9C3: return 18.0;
	case WINDOW_KAISER:      return 2.0*sqrt(1.0+(param/M_PI)*(param/M_PI));
	case WINDOW_RECT:        break;
	}
	return 2.0;
}

double window(enum window_id wn, double x, double param)
{
	switch (wn) {
	case WINDOW_HANN:        return window_hann(x);
	case WINDOW_HAMMING:     return window_hamming(x);
	case WINDOW_BLACKMAN:    return window_blackman(x);
	case WINDOW_NUTTALL4C1:  return window_nuttall4c1(x);
	case WINDOW_ALBRECHT9C3: return window_albrecht9c3(x);
	case WINDOW_KAISER:      return window_kaiser(x, param);
	case WINDOW_RECT:        break;
	}
	return 1.0;
}
