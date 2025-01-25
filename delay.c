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
#include <float.h>
#include <math.h>
#include "delay.h"
#include "allpass.h"
#include "util.h"

struct delay_channel_state {
	sample_t *buf;
	struct ap1_state ap1;
	int has_frac;
};

struct delay_state {
	struct delay_channel_state *cs;
	ssize_t len, p, drain_frames;
	double samples;
	char frac, is_draining, buf_full;
};

#define DELAY_MIN_FRAC 0.1

sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	if (!state->buf_full && state->p + *frames >= state->len)
		state->buf_full = 1;
	sample_t *ibuf_p = ibuf;
	for (ssize_t i = *frames; i > 0; --i) {
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->cs[k].buf) {
				const sample_t s = ibuf_p[k];
				ibuf_p[k] = state->cs[k].buf[state->p];
				state->cs[k].buf[state->p] = s;
			}
		}
		ibuf_p += e->istream.channels;
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
	}
	return ibuf;
}

sample_t * delay_effect_run_frac(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	sample_t *ibuf_p = ibuf;
	if (!state->buf_full && state->p + *frames >= state->len)
		state->buf_full = 1;
	for (ssize_t i = *frames; i > 0; --i) {
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->cs[k].buf) {
				const sample_t s = ibuf_p[k];
				ibuf_p[k] = state->cs[k].buf[state->p];
				state->cs[k].buf[state->p] = s;
			}
			if (state->cs[k].has_frac)
				ibuf_p[k] = ap1_run(&state->cs[k].ap1, ibuf_p[k]);
		}
		ibuf_p += e->istream.channels;
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
	}
	return ibuf;
}

/* FIXME: Not quite right, but probably close enough... */
ssize_t delay_effect_delay(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	return (state->samples < 0.0) ? (state->buf_full) ? state->len : state->p : 0;
}

void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	state->p = 0;
	state->buf_full = 0;
	for (int k = 0; k < e->istream.channels; ++k) {
		if (state->cs[k].buf)
			memset(state->cs[k].buf, 0, state->len * sizeof(sample_t));
		ap1_reset(&state->cs[k].ap1);
	}
}

void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		printf("H%d_%d(w)=1.0", k, i);
		if (state->samples < 0.0)
			printf("*exp(-j*w*%g)", state->samples);
		if (fabs(state->samples) < DELAY_MIN_FRAC)
			printf("*exp(-j*w*-1)");
		if (state->cs[k].buf)
			printf("*exp(-j*w*%zd)", state->len);
		if (state->cs[k].has_frac)
			printf("*((abs(w)<=pi)?(%.15e+1.0*exp(-j*w))/(1.0+%.15e*exp(-j*w)):0/0)",
				state->cs[k].ap1.c0, state->cs[k].ap1.c0);
		putchar('\n');
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
			memset(obuf, 0, *frames * e->istream.channels * sizeof(sample_t));
			e->run(e, frames, obuf, NULL);
		}
		else *frames = -1;
	}
}

void delay_effect_destroy(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		free(state->cs[k].buf);
	free(state->cs);
	free(state);
}

struct effect * delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct delay_state *state = NULL;

	int arg_i = 1, do_frac = 0;
	if (argc == 3 && strcmp(argv[arg_i], "-f") == 0) {
		do_frac = 1;
		++arg_i;
	}
	else if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	double samples = parse_len_frac(argv[arg_i], istream->fs, &endptr);
	CHECK_ENDPTR(argv[arg_i], endptr, "delay", return NULL);
	ssize_t samples_int;
	double samples_frac;
	if (do_frac && fabs(samples - rint(samples)) >= DBL_EPSILON) {  /* ignore extremely small fractional delay */
		samples_int = lrint(floor(fabs(samples)));
		samples_frac = fabs(samples) - floor(fabs(samples));
		if (samples_frac < DELAY_MIN_FRAC) {
			samples_int -= 1;
			samples_frac += 1.0;
		}
		if (samples < 0.0) {
			samples_int = -samples_int;
			samples_frac = -samples_frac;
		}
	}
	else {
		do_frac = 0;
		samples_int = lround(samples);
		samples_frac = 0.0;
		samples = (double) samples_int;
	}
	e = calloc(1, sizeof(struct effect));
	if (samples_int == 0 && samples_frac == 0.0) {
		LOG_FMT(LL_VERBOSE, "%s: info: delay is zero; no proccessing will be done", argv[0]);
		return e;
	}
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->run = (do_frac) ? delay_effect_run_frac : delay_effect_run;
	e->delay = delay_effect_delay;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->drain = delay_effect_drain;
	e->destroy = delay_effect_destroy;

	state = calloc(1, sizeof(struct delay_state));
	state->len = (samples_int < 0) ? -samples_int : samples_int;
	state->frac = do_frac;
	state->samples = samples;
	state->cs = calloc(istream->channels, sizeof(struct delay_channel_state));
	for (int k = 0; k < istream->channels; ++k) {
		if (state->len > 0 && !TEST_BIT(channel_selector, k, samples_int < 0))
			state->cs[k].buf = calloc(state->len, sizeof(sample_t));
		state->cs[k].has_frac = (do_frac && !TEST_BIT(channel_selector, k, samples < 0.0));
		state->cs[k].ap1.c0 = (1.0-fabs(samples_frac)) / (1.0+fabs(samples_frac));
	}

	if (do_frac) {
		LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd%+g samples)",
			argv[0], samples / istream->fs, samples_int, samples_frac);
	}
	else {
		LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd sample%s)",
			argv[0], (double) samples_int / istream->fs, samples_int, (state->len == 1) ? "" : "s");
	}

	e->data = state;
	return e;
}
