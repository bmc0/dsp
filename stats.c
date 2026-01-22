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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "stats.h"
#include "util.h"

struct stats_state {
	ssize_t samples, peak_count, peak_frame;
	sample_t sum, sum_sq, min, max, abs_peak, ref;
	int width;
};

sample_t * stats_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct stats_state *state = (struct stats_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels) {
		for (k = 0; k < e->ostream.channels; ++k) {
			const sample_t s = ibuf[i+k];
			const sample_t abs_s = fabs(s);
			state[k].sum += s;
			state[k].sum_sq += s*s;
			if (s < state[k].min) state[k].min = s;
			if (s > state[k].max) state[k].max = s;
			if (abs_s > 0.0 && abs_s == state[k].abs_peak)
				++state[k].peak_count;
			else if (abs_s > state[k].abs_peak) {
				state[k].abs_peak = abs_s;
				state[k].peak_frame = state[k].samples;
				state[k].peak_count = 1;
			}
			++state[k].samples;
		}
	}
	return ibuf;
}

static void stats_print_channels(struct effect *e, int start, int end)
{
	struct stats_state *state = (struct stats_state *) e->data;
	dsp_log_printf("\n%-18s", "Channel");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12d", i);
	dsp_log_printf("\n%-18s", "DC offset");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state[i].sum/state[i].samples);
	dsp_log_printf("\n%-18s", "Minimum");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state[i].min);
	dsp_log_printf("\n%-18s", "Maximum");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state[i].max);
	dsp_log_printf("\n%-18s", "Peak level (dBFS)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(state[i].abs_peak));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "Peak level (dBr)");
		for (int i = start; i < end; ++i)
			dsp_log_printf(" %12.4f", state->ref + 20.0*log10(state[i].abs_peak));
	}
	dsp_log_printf("\n%-18s", "RMS level (dBFS)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(sqrt(state[i].sum_sq/state[i].samples)));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "RMS level (dBr)");
		for (int i = start; i < end; ++i)
			dsp_log_printf(" %12.4f", state->ref + 20.0*log10(sqrt(state[i].sum_sq/state[i].samples)));
	}
	dsp_log_printf("\n%-18s", "Crest factor (dB)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(state[i].abs_peak/sqrt(state[i].sum_sq/state[i].samples)));
	dsp_log_printf("\n%-18s", "Peak count");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state[i].peak_count);
	dsp_log_printf("\n%-18s", "Peak sample");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state[i].peak_frame);
	dsp_log_printf("\n%-18s", "Samples");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state[i].samples);
	dsp_log_printf("\n%-18s", "Length (s)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.2f", (double) state[i].samples / e->ostream.fs);
	dsp_log_printf("\n");
}

void stats_effect_destroy(struct effect *e)
{
	struct stats_state *state = (struct stats_state *) e->data;
	int cols = e->ostream.channels;
	if (state->width > 0)
		cols = MAXIMUM((state->width-18)/13, 1);
	dsp_log_acquire();
	for (int i = 0; i < e->ostream.channels; i+=cols)
		stats_print_channels(e, i, MINIMUM(i+cols, e->ostream.channels));
	dsp_log_release();
	free(e->data);
}

struct effect * stats_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int width = 80, opt;
	sample_t ref = -HUGE_VAL;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	char *endptr;

	while ((opt = dsp_getopt(&g, argc, argv, "w:")) != -1) {
		switch (opt) {
		case 'w':
			width = strtol(g.arg, &endptr, 10);
			CHECK_ENDPTR(g.arg, endptr, "width", return NULL);
			break;
		case ':':
			LOG_FMT(LL_ERROR, "%s: error: expected argument to option '%c'", argv[0], g.opt);
			return NULL;
		default: goto print_usage;
		}
	}

	if (g.ind == argc-1) {
		ref = strtod(argv[1], &endptr);
		CHECK_ENDPTR(argv[1], endptr, "ref_level", return NULL);
	}
	else if (g.ind != argc) {
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
	e->run = stats_effect_run;
	e->plot = effect_plot_noop;
	e->destroy = stats_effect_destroy;
	struct stats_state *state = calloc(istream->channels, sizeof(struct stats_state));
	state->ref = ref;
	state->width = width;
	e->data = state;
	return e;
}
