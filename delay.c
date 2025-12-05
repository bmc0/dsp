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
	union {
		struct ap1_state first;
		struct thiran_ap_state *nth;
	} fd_ap;
	int has_frac;
};

struct delay_state {
	void (*run)(struct delay_state *, ssize_t, sample_t *, int);
	struct delay_channel_state *cs;
	ssize_t len, p, frames, drain_frames;
	double samples;
	int fd_ap_n, is_offset, is_draining;
};

#define DELAY_MIN_FRAC 0.1

#define DELAY_RUN_DEFINE_FN(X, C) \
	static void delay_run ## X (struct delay_state *state, ssize_t frames, sample_t *ibuf_p, int channels) \
	{ \
		for (ssize_t i = frames; i > 0; --i) { \
			for (int k = 0; k < channels; ++k) { \
				struct delay_channel_state *cs = &state->cs[k]; \
				if (cs->buf) { \
					const sample_t s = ibuf_p[k]; \
					ibuf_p[k] = cs->buf[state->p]; \
					cs->buf[state->p] = s; \
				} \
				C \
			} \
			ibuf_p += channels; \
			state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1; \
		} \
	}

DELAY_RUN_DEFINE_FN(,)
DELAY_RUN_DEFINE_FN(_frac_1,
	if (cs->has_frac)
		ibuf_p[k] = ap1_run(&cs->fd_ap.first, ibuf_p[k]);
)
DELAY_RUN_DEFINE_FN(_frac_n,
	if (cs->has_frac)
		ibuf_p[k] = thiran_ap_run(cs->fd_ap.nth, ibuf_p[k]);
)

sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	state->frames += *frames;
	state->run(state, *frames, ibuf, e->istream.channels);
	return ibuf;
}

ssize_t delay_effect_delay(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	ssize_t d = 0;
	if (state->samples < 0.0) {
		if (state->is_offset) d = state->fd_ap_n;
		else d = state->len + state->fd_ap_n;
		d = MINIMUM(d, state->frames);
	}
	else if (state->is_offset) {
		d = MINIMUM(state->len, state->frames);
	}
	return d;
}

void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	state->p = 0;
	state->frames = 0;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->buf)
			memset(cs->buf, 0, state->len * sizeof(sample_t));
		if (cs->has_frac) {
			if (state->fd_ap_n > 1) thiran_ap_reset(cs->fd_ap.nth);
			else ap1_reset(&cs->fd_ap.first);
		}
	}
}

void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		printf("H%d_%d(w)=1.0", k, i);
		if (state->samples < 0.0) {
			if (state->fd_ap_n > 0)
				printf("*exp(-j*w*%.15e)", state->samples);
			else
				printf("*exp(-j*w*%zd)", (ssize_t) state->samples);
		}
		if (state->is_offset)
			printf("*exp(-j*w*%zd)", -state->len);
		if (cs->buf)
			printf("*exp(-j*w*%zd)", state->len);
		if (cs->has_frac) {
			if (state->fd_ap_n > 1) {
				putchar('*');
				thiran_ap_plot(cs->fd_ap.nth);
			}
			else {
				printf("*((abs(w)<=pi)?(%.15e+1.0*exp(-j*w))/(1.0+%.15e*exp(-j*w)):0/0)",
					cs->fd_ap.first.c0, cs->fd_ap.first.c0);
			}
		}
		putchar('\n');
	}
}

void delay_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	if (state->frames == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			if (state->is_offset) state->drain_frames = state->fd_ap_n;
			else state->drain_frames = state->len + state->fd_ap_n;
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
	for (int k = 0; k < e->istream.channels; ++k) {
		free(state->cs[k].buf);
		if (state->fd_ap_n > 1) free(state->cs[k].fd_ap.nth);
	}
	free(state->cs);
	free(state);
}

struct effect * delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct delay_state *state = NULL;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int do_frac = 0, fd_ap_n = 5, is_offset = 0, opt;

	while ((opt = dsp_getopt(&g, argc-1, argv, "f::")) != -1) {
		switch (opt) {
		case 'f':
			do_frac = 1;
			if (g.arg) {
				fd_ap_n = strtol(g.arg, &endptr, 10);
				CHECK_ENDPTR(g.arg, endptr, "order", return NULL);
				CHECK_RANGE(fd_ap_n > 0 && fd_ap_n <= 50, "order", return NULL);
			}
			break;
		default: goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}
	double samples = parse_len_frac(argv[g.ind], istream->fs, &endptr);
	CHECK_ENDPTR(argv[g.ind], endptr, "delay", return NULL);
	ssize_t samples_int;
	double samples_frac;
	if (do_frac && fabs(samples - rint(samples)) >= DBL_EPSILON) {  /* ignore extremely small fractional delay */
		samples_int = (ssize_t) floor(fabs(samples)) - (fd_ap_n-1);
		samples_frac = fabs(samples) - floor(fabs(samples));
		if (samples_frac < DELAY_MIN_FRAC) {
			samples_int -= 1;
			samples_frac += 1.0;
		}
		samples_frac += fd_ap_n-1;
		is_offset = (samples_int < 0);
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
	e->run = delay_effect_run;
	e->delay = delay_effect_delay;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->drain = delay_effect_drain;
	e->destroy = delay_effect_destroy;

	e->data = state = calloc(1, sizeof(struct delay_state));
	if (do_frac) {
		if (fd_ap_n > 1) state->run = delay_run_frac_n;
		else state->run = delay_run_frac_1;
	}
	else state->run = delay_run;
	state->len = (samples_int < 0) ? -samples_int : samples_int;
	state->samples = samples;
	state->fd_ap_n = (do_frac) ? fd_ap_n : 0;
	state->is_offset = is_offset;
	if (do_frac) {
		LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd%+g samples)",
			argv[0], samples / istream->fs, samples_int, samples_frac);
	}
	else {
		LOG_FMT(LL_VERBOSE, "%s: info: actual delay is %gs (%zd sample%s)",
			argv[0], (double) samples_int / istream->fs, samples_int, (state->len == 1) ? "" : "s");
	}
	state->cs = calloc(istream->channels, sizeof(struct delay_channel_state));
	for (int k = 0; k < istream->channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (state->len > 0 && !TEST_BIT(channel_selector, k, samples_int < 0))
			cs->buf = calloc(state->len, sizeof(sample_t));
		cs->has_frac = (do_frac && !TEST_BIT(channel_selector, k, samples < 0.0));
		if (cs->has_frac) {
			if (state->fd_ap_n > 1) {
				if ((cs->fd_ap.nth = thiran_ap_new(fd_ap_n, fabs(samples_frac))) == NULL) {
					LOG_FMT(LL_ERROR, "%s: error: thiran_ap_new() failed", argv[0]);
					delay_effect_destroy(e);
					free(e);
					return NULL;
				}
			}
			else cs->fd_ap.first.c0 = (1.0-fabs(samples_frac)) / (1.0+fabs(samples_frac));
		}
	}

	return e;
}
