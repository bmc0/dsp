/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <string.h>
#include <math.h>
#include "crossfeed.h"
#include "biquad.h"
#include "util.h"

struct crossfeed_state {
	int c0, c1;
	sample_t direct_gain, cross_gain;
	struct biquad_state lp[2], hp[2];
};

sample_t * crossfeed_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, samples = *frames * e->ostream.channels;
	sample_t s0, s1;
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;

	for (i = 0; i < samples; i += e->ostream.channels) {
		s0 = ibuf[i + state->c0];
		s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = (s0 * state->direct_gain)
			+ (biquad(&state->lp[0], s1) * state->cross_gain)
			+ (biquad(&state->hp[0], s0) * state->cross_gain);
		ibuf[i + state->c1] = (s1 * state->direct_gain)
			+ (biquad(&state->lp[1], s0) * state->cross_gain)
			+ (biquad(&state->hp[1], s1) * state->cross_gain);
	}
	return ibuf;
}

void crossfeed_effect_reset(struct effect *e)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	biquad_reset(&state->lp[0]);
	biquad_reset(&state->lp[1]);
	biquad_reset(&state->hp[0]);
	biquad_reset(&state->hp[1]);
}

static void crossfeed_plot_channel(struct crossfeed_state *state, int fs, int i, int c, int cc)
{
	printf("H%d_%d(w)=(abs(w)<=pi)?%.15e*Ht%d_%d(w*%d/2.0/pi)",
		c, i, state->direct_gain, c, i, fs);
	printf("+%.15e*Ht%d_%d(w*%d/2.0/pi)*(" BIQUAD_PLOT_FMT ")",
		state->cross_gain, cc, i, fs, BIQUAD_PLOT_FMT_ARGS(&state->lp[0]));
	printf("+%.15e*Ht%d_%d(w*%d/2.0/pi)*(" BIQUAD_PLOT_FMT ")",
		state->cross_gain, c, i, fs, BIQUAD_PLOT_FMT_ARGS(&state->hp[0]));
	puts(":0/0");
}

void crossfeed_effect_plot(struct effect *e, int i)
{
	struct crossfeed_state *state = (struct crossfeed_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (k == state->c0)
			crossfeed_plot_channel(state, e->ostream.fs, i, state->c0, state->c1);
		else if (k == state->c1)
			crossfeed_plot_channel(state, e->ostream.fs, i, state->c1, state->c0);
		else
			printf("H%d_%d(w)=Ht%d_%d(w*%d/2.0/pi)\n", k, i, k, i, e->ostream.fs);
	}
}

void crossfeed_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * crossfeed_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	struct crossfeed_state *state;
	char *endptr;
	double freq, sep_db, sep;
	int i, n_channels = 0;

	if (argc != 3) {
		print_effect_usage(ei);
		return NULL;
	}
	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	if (n_channels != 2) {
		LOG_FMT(LL_ERROR, "%s: error: number of input channels must be 2", argv[0]);
		return NULL;
	}

	freq = parse_freq(argv[1], &endptr);
	CHECK_ENDPTR(argv[1], endptr, "f0", return NULL);
	CHECK_FREQ(freq, istream->fs, "f0", return NULL);
	sep_db = strtod(argv[2], &endptr);
	CHECK_ENDPTR(argv[2], endptr, "separation", return NULL);
	CHECK_RANGE(sep_db >= 0.0, "separation", return NULL);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_PLOT_MIX;
	e->run = crossfeed_effect_run;
	e->reset = crossfeed_effect_reset;
	e->plot = crossfeed_effect_plot;
	e->destroy = crossfeed_effect_destroy;
	state = calloc(1, sizeof(struct crossfeed_state));

	state->c0 = state->c1 = -1;
	for (i = 0; i < istream->channels; ++i) {  /* find input channel numbers */
		if (GET_BIT(channel_selector, i)) {
			if (state->c0 == -1)
				state->c0 = i;
			else
				state->c1 = i;
		}
	}
	sep = pow(10, sep_db / 20);
	state->direct_gain = sep / (1 + sep);
	state->cross_gain = 1 / (1 + sep);

	biquad_init_using_type(&state->lp[0], BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->lp[1], BIQUAD_LOWPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->hp[0], BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&state->hp[1], BIQUAD_HIGHPASS_1, istream->fs, freq, 0, 0, 0, BIQUAD_WIDTH_Q);
	e->data = state;

	return e;
}
