/*
 * This file is part of dsp.
 *
 * Copyright (c) 2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "sinc.h"
#include "window.h"
#include "fir_util.h"
#include "util.h"

#define KAISER_BETA_DEFAULT 12.0

struct effect * sinc_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	enum conv_id conv = CONV_ID_FIR;
	enum window_id wn = WINDOW_KAISER;
	int do_align = 0, highpass = 0, opt;
	double tbw = 0.5, param = KAISER_BETA_DEFAULT;

	while ((opt = dsp_getopt(&g, argc-1, argv, "hpzct:w:")) != -1) {
		switch (opt) {
		case 'h': highpass = 1; break;
		case 'p': conv = CONV_ID_FIR_P; break;
		case 'z': conv = CONV_ID_ZITA_CONVOLVER; break;
		case 'c': do_align = 1; break;
		case 't':
			tbw = strtod(g.arg, &endptr);
			CHECK_ENDPTR(g.arg, endptr, "width", return NULL);
			CHECK_RANGE(tbw >= 0.001 && tbw <= 2.0, "width", return NULL);
			break;
		case 'w':
			if (strcmp(g.arg, "rect") == 0) wn = WINDOW_RECT;
			else if (strcmp(g.arg, "hann") == 0) wn = WINDOW_HANN;
			else if (strcmp(g.arg, "hamming") == 0) wn = WINDOW_HAMMING;
			else if (strcmp(g.arg, "blackman") == 0) wn = WINDOW_BLACKMAN;
			else if (strcmp(g.arg, "nuttall4c1") == 0) wn = WINDOW_NUTTALL4C1;
			else if (strcmp(g.arg, "albrecht9c3") == 0) wn = WINDOW_ALBRECHT9C3;
			else if (strncmp(g.arg, "kaiser", 6) == 0) {
				wn = WINDOW_KAISER;
				if (g.arg[6] == ':') {
					const char *subarg = g.arg+7;
					param = strtod(subarg, &endptr);
					CHECK_ENDPTR(subarg, endptr, "beta", return NULL);
					CHECK_RANGE(param >= 0.0 && param <= 32.0, "beta", return NULL);
				}
				else param = KAISER_BETA_DEFAULT;
			}
			else {
				LOG_FMT(LL_ERROR, "%s: error: unknown window: %s", ei->name, g.arg);
				return NULL;
			}
			break;
		default:
			dsp_getopt_print_error(&g, opt, ei->name);
			goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}
	double f0 = parse_freq(argv[g.ind], &endptr);
	CHECK_ENDPTR(argv[g.ind], endptr, "f0", return NULL);
	CHECK_FREQ(f0, istream->fs*0.9995, "f0", return NULL);

	if (f0*(2.0+tbw) > istream->fs) {
		tbw = (istream->fs-2.0*f0)/f0;
		LOG_FMT(LL_VERBOSE, "%s: adjusting width to %g", ei->name, tbw);
	}
	const ssize_t m = lround(window_width(wn, param)/(2.0*tbw*f0/istream->fs))*2, taps = m+1;
	const double fc = 2.0*f0/istream->fs;

	sample_t *h = calloc(taps, sizeof(sample_t));
	if (check_alloc(ei->name, h)) return NULL;
	for (ssize_t i = 0; i < taps; ++i)
		h[i] = norm_sinc((i*2 - m)/2.0, fc) * window(wn, (double) i / m, param);
	if (highpass) {
		for (ssize_t i = 0; i < taps; ++i) h[i] = -h[i];
		h[taps/2] += 1.0;
	}

	const ssize_t ref = (do_align) ? taps/2 : 0;
	e = init_convolver(conv, ei, istream, channel_selector, h, 1, taps, ref);
	free(h);
	return e;
}
