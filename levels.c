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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "levels.h"
#include "ewma.h"
#include "util.h"

struct levels_ch_state {
	double block_peak;
	struct ewma_state avg, peak;
	struct statusline_state statusline;
	char bar[61];
};

struct levels_state {
	struct levels_ch_state **cs;
	int statuslines_registered;
};

static void draw_levels_bar(char *s, double avg, double peak)
{
	memset(s, ' ', 60);
	if (isinf(avg))  avg  = -200.0;
	if (isinf(peak)) peak = -200.0;
	for (int i = 4; i < 59; i+=5) s[i] = '.';
	int idx_avg = 59+lrint(avg);
	if (idx_avg >= 0) memset(s, '#', MINIMUM(idx_avg, 59)+1);
	int idx_peak = 59+lrint(peak);
	if (idx_peak >= 0) s[MINIMUM(idx_peak, 59)] = '|';
	s[60] = '\0';
}

sample_t * levels_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct levels_state *state = (struct levels_state *) e->data;
	const int stride = e->istream.channels;
	sample_t *const ibuf_end = ibuf + *frames*stride;
	for (sample_t *ibuf_p = ibuf; ibuf_p < ibuf_end; ibuf_p += stride) {
		for (int k = 0; k < stride; ++k) {
			if (state->cs[k]) {
				struct levels_ch_state *cs = state->cs[k];
				const double s2 = ibuf_p[k]*ibuf_p[k];
				ewma_run(&cs->avg, s2);
				const double peak = ewma_run_set_min(&cs->peak, s2);
				if (cs->block_peak < peak) cs->block_peak = peak;
			}
		}
	}
	dsp_statuslines_acquire();
	if (!state->statuslines_registered) {
		for (int k = 0; k < stride; ++k) {
			if (state->cs[k])
				dsp_statusline_register(&state->cs[k]->statusline);
		}
		state->statuslines_registered = 1;
	}
	for (int k = 0; k < stride; ++k) {
		if (state->cs[k]) {
			struct levels_ch_state *cs = state->cs[k];
			const double avg = 10.0*log10(ewma_get_last(&cs->avg));
			const double peak = 10.0*log10(cs->block_peak);
			draw_levels_bar(cs->bar, avg, peak);
			snprintf(cs->statusline.s, DSP_STATUSLINE_MAX_LEN,
				"%s: channel %*d: [%s]  avg:%+6.1f; peak:%+6.1f",
				e->name, (stride>10)?2:1, k, cs->bar, avg, peak);
			cs->block_peak = 0.0;
		}
	}
	dsp_statuslines_release();
	return ibuf;
}

void levels_effect_destroy(struct effect *e)
{
	struct levels_state *state = (struct levels_state *) e->data;
	if (state->statuslines_registered) {
		dsp_statuslines_acquire();
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->cs[k])
				dsp_statusline_unregister(&state->cs[k]->statusline);
		}
		dsp_statuslines_release();
	}
	for (int k = 0; k < e->istream.channels; ++k) {
		if (state->cs[k]) {
			free(state->cs[k]);
			break;
		}
	}
	free(state->cs);
	free(state);
}

struct effect * levels_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	int opt;
	double tc = 0.3;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;

	while ((opt = dsp_getopt(&g, argc, argv, "t:")) != -1) {
		switch (opt) {
		case 't':
			tc = strtod(g.arg, &endptr);
			CHECK_ENDPTR(g.arg, endptr, "time constant", return NULL);
			CHECK_RANGE(tc >= 0.01 && tc <= 10.0, "time constant", return NULL);
			break;
		default:
			dsp_getopt_print_error(&g, opt, argv[0]);
			goto print_usage;
		}
	}
	if (g.ind != argc) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}

	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_NO_DITHER;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->flags |= EFFECT_FLAG_ALIGN_BARRIER;
	e->run = levels_effect_run;
	e->plot = effect_plot_noop;
	e->destroy = levels_effect_destroy;

	struct levels_state *state = calloc(1, sizeof(struct levels_state));
	e->data = state;
	state->cs = calloc(istream->channels, sizeof(struct levels_ch_state *));
	const int n_ch = num_bits_set(channel_selector, istream->channels);
	struct levels_ch_state *cs_all = calloc(n_ch, sizeof(struct levels_ch_state));
	for (int k = 0; k < istream->channels; ++k) {
		if (GET_BIT(channel_selector, k)) {
			struct levels_ch_state *cs = state->cs[k] = cs_all++;
			ewma_init(&cs->avg, e->istream.fs, tc);
			ewma_init(&cs->peak, e->istream.fs, tc);
		}
	}

	return e;
}
