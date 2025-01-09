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
#include <string.h>
#include <math.h>
#include "delay.h"
#include "util.h"

struct delay_state {
	sample_t **bufs;
	ssize_t len, p, drain_frames;
	char negative, is_draining, buf_full;
};

sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	if (!state->buf_full && state->p + *frames >= state->len)
		state->buf_full = 1;
	sample_t *ibuf_p = ibuf;
	for (ssize_t i = *frames; i > 0; --i) {
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->bufs[k]) {
				const sample_t s = ibuf_p[k];
				ibuf_p[k] = state->bufs[k][state->p];
				state->bufs[k][state->p] = s;
			}
		}
		ibuf_p += e->istream.channels;
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
	}
	return ibuf;
}

ssize_t delay_effect_delay(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	return (state->negative) ? (state->buf_full) ? state->len : state->p : 0;
}

void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	state->p = 0;
	state->buf_full = 0;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->bufs[k])
			memset(state->bufs[k], 0, state->len * sizeof(sample_t));
}

void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		if ((state->negative) ? !state->bufs[k] : !!state->bufs[k])
			printf("H%d_%d(w)=exp(-j*w*%zd)\n", k, i, (state->negative) ? -state->len : state->len);
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void delay_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	if (!state->buf_full && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->len;
			state->is_draining = 1;
		}
		if (state->drain_frames > 0) {
			*frames = MINIMUM(*frames, state->drain_frames);
			state->drain_frames -= *frames;
			sample_t *obuf_p = obuf;
			for (ssize_t i = *frames; i > 0; --i) {
				for (int k = 0; k < e->istream.channels; ++k) {
					if (state->bufs[k]) {
						obuf_p[k] = state->bufs[k][state->p];
						state->bufs[k][state->p] = 0.0;
					}
					else obuf_p[k] = 0.0;
				}
				obuf_p += e->istream.channels;
				state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
			}
		}
		else *frames = -1;
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
	struct delay_state *state = NULL;

	if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	ssize_t samples = parse_len(argv[1], istream->fs, &endptr);
	CHECK_ENDPTR(argv[1], endptr, "delay", return NULL);
	e = calloc(1, sizeof(struct effect));
	if (samples == 0) {
		LOG_FMT(LL_VERBOSE, "%s: info: delay is zero; no proccessing will be done", argv[0]);
		return e;
	}
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->run = delay_effect_run;
	e->delay = delay_effect_delay;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->drain = delay_effect_drain;
	e->destroy = delay_effect_destroy;

	LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd sample%s)",
		argv[0], (double) samples / istream->fs, samples, (samples == 1 || samples == -1) ? "" : "s");
	state = calloc(1, sizeof(struct delay_state));
	if (samples < 0) {
		state->len = -samples;
		state->negative = 1;
	}
	else state->len = samples;
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int k = 0; k < istream->channels; ++k)
		if ((state->negative) ? !GET_BIT(channel_selector, k) : !!GET_BIT(channel_selector, k))
			state->bufs[k] = calloc(state->len, sizeof(sample_t));

	e->data = state;
	return e;
}
