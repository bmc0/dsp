/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "delay.h"
#include "util.h"

struct delay_state {
	sample_t **bufs;
	ssize_t len, p;
};

sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		for (ssize_t k = 0; k < e->istream.channels; ++k) {
			if (state->bufs[k] && state->len > 0) {
				obuf[i * e->istream.channels + k] = state->bufs[k][state->p];
				state->bufs[k][state->p] = ibuf[i * e->istream.channels + k];
			}
			else
				obuf[i * e->istream.channels + k] = ibuf[i * e->istream.channels + k];
		}
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
	}
	return obuf;
}

void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->bufs[k] && state->len > 0)
			memset(state->bufs[k], 0, state->len * sizeof(sample_t));
	state->p = 0;
}

void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->bufs[k])
			printf("H%d_%d(w)=exp(-j*w*%zd)\n", k, i, state->len);
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void delay_effect_destroy(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		free(state->bufs[k]);
	free(state->bufs);
	free(state);
}

struct effect * delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct delay_state *state;

	if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	ssize_t samples = parse_len(argv[1], istream->fs, &endptr);
	CHECK_ENDPTR(argv[1], endptr, "delay", return NULL);
	CHECK_RANGE(samples >= 0, "delay", return NULL);
	LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd sample%s)", argv[0], (double) samples / istream->fs, samples, (samples == 1) ? "" : "s");
	state = calloc(1, sizeof(struct delay_state));
	state->len = samples;
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int k = 0; k < istream->channels; ++k)
		if (GET_BIT(channel_selector, k) && state->len > 0)
			state->bufs[k] = calloc(state->len, sizeof(sample_t));

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->run = delay_effect_run;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->destroy = delay_effect_destroy;
	e->data = state;
	return e;
}
