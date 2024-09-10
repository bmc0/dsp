/*
 * This file is part of dsp.
 *
 * Copyright (c) 2020-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "util.h"

struct effect * hilbert_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	ssize_t taps, i = 1, k;
	sample_t *h;
	char *endptr;
	int use_fir_p = 0;
	struct effect *e;

	if (argc > 3 || argc < 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (strcmp(argv[i], "-p") == 0) {
		use_fir_p = 1;
		++i;
	}
	taps = strtol(argv[i], &endptr, 10);
	CHECK_ENDPTR(argv[i], endptr, "taps", return NULL);
	if (taps < 3) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be > 3", argv[0]);
		return NULL;
	}
	if (taps%2 == 0) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be odd", argv[0]);
		return NULL;
	}
	h = calloc(taps, sizeof(sample_t));
	for (i = 0, k = -taps/2; i < taps; ++i, ++k) {
		if (k%2 == 0)
			h[i] = 0;
		else {
			double x = 2.0*M_PI*i/(taps-1);
			h[i] = 2.0/(M_PI*k) * (0.42 - 0.5*cos(x) + 0.08*cos(2.0*x));
		}
	}
	e = (use_fir_p) ?
		fir_p_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0) :
		fir_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0);
	free(h);
	return e;
}
