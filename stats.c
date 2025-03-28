/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
	sample_t sum, sum_sq, min, max, ref;
};

sample_t * stats_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct stats_state *state = (struct stats_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels) {
		for (k = 0; k < e->ostream.channels; ++k) {
			state[k].sum += ibuf[i + k];
			state[k].sum_sq += ibuf[i + k] * ibuf[i + k];
			if (ibuf[i + k] < state[k].min) state[k].min = ibuf[i + k];
			if (ibuf[i + k] > state[k].max) state[k].max = ibuf[i + k];
			if (fabs(ibuf[i + k]) >= MAXIMUM(fabs(state[k].max), fabs(state[k].min))) {
				state[k].peak_frame = state[k].samples;
				state[k].peak_count = 0;
			}
			if (fabs(ibuf[i + k]) == MAXIMUM(fabs(state[k].max), fabs(state[k].min))) ++state[k].peak_count;
			++state[k].samples;
		}
	}
	return ibuf;
}

void stats_effect_plot(struct effect *e, int i)
{
	for (int k = 0; k < e->ostream.channels; ++k)
		printf("H%d_%d(f)=1.0\n", k, i);
}

void stats_effect_destroy(struct effect *e)
{
	ssize_t i;
	struct stats_state *state = (struct stats_state *) e->data;
	dsp_log_acquire();
	dsp_log_printf("\n%-18s", "Channel");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12zd", i);
	dsp_log_printf("\n%-18s", "DC offset");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.8f", state[i].sum / state[i].samples);
	dsp_log_printf("\n%-18s", "Minimum");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.8f", state[i].min);
	dsp_log_printf("\n%-18s", "Maximum");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.8f", state[i].max);
	dsp_log_printf("\n%-18s", "Peak level (dBFS)");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.4f", 20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max))));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "Peak level (dBr)");
		for (i = 0; i < e->ostream.channels; ++i)
			dsp_log_printf(" %12.4f", state->ref + (20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max)))));
	}
	dsp_log_printf("\n%-18s", "RMS level (dBFS)");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.4f", 20 * log10(sqrt(state[i].sum_sq / state[i].samples)));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "RMS level (dBr)");
		for (i = 0; i < e->ostream.channels; ++i)
			dsp_log_printf(" %12.4f", state->ref + (20 * log10(sqrt(state[i].sum_sq / state[i].samples))));
	}
	dsp_log_printf("\n%-18s", "Crest factor (dB)");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.4f", 20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max)) / sqrt(state[i].sum_sq / state[i].samples)));
	dsp_log_printf("\n%-18s", "Peak count");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12zd", state[i].peak_count);
	dsp_log_printf("\n%-18s", "Peak sample");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12zd", state[i].peak_frame);
	dsp_log_printf("\n%-18s", "Samples");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12zd", state[i].samples);
	dsp_log_printf("\n%-18s", "Length (s)");
	for (i = 0; i < e->ostream.channels; ++i)
		dsp_log_printf(" %12.2f", (double) state[i].samples / e->ostream.fs);
	dsp_log_printf("\n");
	dsp_log_release();
	free(state);
}

struct effect * stats_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct stats_state *state;
	sample_t ref = -HUGE_VAL;

	if (argc == 2) {
		ref = strtod(argv[1], &endptr);
		CHECK_ENDPTR(argv[1], endptr, "ref_level", return NULL);
	}
	else if (argc != 1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_NO_DITHER;
	e->run = stats_effect_run;
	e->plot = stats_effect_plot;
	e->destroy = stats_effect_destroy;
	state = calloc(istream->channels, sizeof(struct stats_state));
	state->ref = ref;
	e->data = state;
	return e;
}
