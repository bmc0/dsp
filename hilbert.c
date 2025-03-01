/*
 * This file is part of dsp.
 *
 * Copyright (c) 2020-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "hilbert.h"
#include "fir.h"
#include "fir_p.h"
#include "zita_convolver.h"
#include "util.h"

struct effect * hilbert_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int conv = 0, opt;
	double angle = -M_PI_2;

	while ((opt = dsp_getopt(&g, argc-1, argv, "pza:")) != -1) {
		switch (opt) {
		case 'p': conv = 1; break;
		case 'z': conv = 2; break;
		case 'a':
			angle = strtod(g.arg, &endptr)/180.0*M_PI;
			CHECK_ENDPTR(g.arg, endptr, "angle", return NULL);
			break;
		case ':':
			LOG_FMT(LL_ERROR, "%s: error: expected argument to option '%c'", argv[0], g.opt);
			return NULL;
		default: goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	const ssize_t taps = strtol(argv[g.ind], &endptr, 10);
	CHECK_ENDPTR(argv[g.ind], endptr, "taps", return NULL);
	if (taps <= 3) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be > 3", argv[0]);
		return NULL;
	}
	if (taps % 2 == 0) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be odd", argv[0]);
		return NULL;
	}
	sample_t *h = calloc(taps, sizeof(sample_t));
	const double w_h = sin(-angle), w_d = cos(-angle);
	for (ssize_t i = 0, k = -taps / 2; i < taps; ++i, ++k) {
		if (k == 0)
			h[i] = w_d;
		else if (k%2 == 0)
			h[i] = 0;
		else {
			double x = 2.0*M_PI*i/(taps-1);
			h[i] = w_h * 2.0/(M_PI*k) * (0.42 - 0.5*cos(x) + 0.08*cos(2.0*x));
		}
	}
	if (conv == 1)
		e = fir_p_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0);
	else if (conv == 2) {
		#ifdef HAVE_ZITA_CONVOLVER
			e = zita_convolver_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0, 0);
		#else
			LOG_FMT(LL_ERROR, "%s: warning: zita_convolver not available; using fir_p instead", argv[0]);
			e = fir_p_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0);
		#endif
	}
	else e = fir_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0);
	free(h);
	return e;
}
