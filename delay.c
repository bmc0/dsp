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
#include <string.h>
#include <float.h>
#include <math.h>
#include "delay.h"
#include "allpass.h"
#include "util.h"

struct delay_channel_state {
	void (*run)(struct delay_channel_state *, ssize_t, sample_t *, int);
	sample_t *buf;
	union {
		struct ap1_state first;
		struct thiran_ap_state *nth;
	} fd_ap;
	double samples_frac;
	ssize_t samples_int, p;
	int fd_ap_n;
};

struct delay_state {
	struct delay_channel_state *cs;
	ssize_t offset;
};

#define DELAY_MIN_FRAC 0.1
#define DELAY_FD_AP_N_DEFAULT 5

#define DELAY_RUN_CHANNEL_BODY(C) \
	if (cs->buf) { \
		for (ssize_t i = frames; i > 0; --i) { \
			const sample_t s = *ibuf_p; \
			*ibuf_p = cs->buf[cs->p]; \
			C \
			cs->buf[cs->p] = s; \
			ibuf_p += stride; \
			cs->p = (cs->p + 1 >= cs->samples_int) ? 0 : cs->p + 1; \
		} \
	}

#define DELAY_RUN_CHANNEL_BODY_FRAC(C) \
	DELAY_RUN_CHANNEL_BODY(C) \
	else { \
		for (ssize_t i = frames; i > 0; --i) { \
			C \
			ibuf_p += stride; \
		} \
	}

static void delay_run_channel(struct delay_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride)
{
	DELAY_RUN_CHANNEL_BODY()
}

static void delay_run_channel_frac_1(struct delay_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride)
{
	DELAY_RUN_CHANNEL_BODY_FRAC(
		*ibuf_p = ap1_run(&cs->fd_ap.first, *ibuf_p);
	)
}

static void delay_run_channel_frac_n(struct delay_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride)
{
	DELAY_RUN_CHANNEL_BODY_FRAC(
		*ibuf_p = thiran_ap_run(cs->fd_ap.nth, *ibuf_p);
	)
}

sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		cs->run(cs, *frames, &ibuf[k], e->istream.channels);
	}
	return ibuf;
}

sample_t * delay_effect_run_noop(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	return ibuf;
}

ssize_t delay_effect_delay(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	return -state->offset;
}

void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		cs->p = 0;
		if (cs->buf)
			memset(cs->buf, 0, cs->samples_int * sizeof(sample_t));
		if (cs->fd_ap_n > 1)
			thiran_ap_reset(cs->fd_ap.nth);
		else if (cs->fd_ap_n == 1)
			ap1_reset(&cs->fd_ap.first);
	}
}

void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		printf("H%d_%d(w)=exp(-j*w*%zd)", k, i, state->offset + cs->samples_int);
		if (cs->fd_ap_n > 1) {
			putchar('*');
			thiran_ap_plot(cs->fd_ap.nth);
		}
		else if (cs->fd_ap_n == 1) {
			printf("*((abs(w)<=pi)?(%.15e+1.0*exp(-j*w))/(1.0+%.15e*exp(-j*w)):0/0)",
				cs->fd_ap.first.c0, cs->fd_ap.first.c0);
		}
		putchar('\n');
	}
}

void delay_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		drain_samples[k] += state->cs[k].samples_int + state->cs[k].fd_ap_n;
}

void delay_effect_destroy(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		free(cs->buf);
		if (cs->fd_ap_n > 1) free(cs->fd_ap.nth);
	}
	free(state->cs);
	free(state);
}

int delay_effect_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge == src->merge) {
		struct delay_state *dest_state = (struct delay_state *) dest->data;
		struct delay_state *src_state = (struct delay_state *) src->data;
		for (int k = 0; k < dest->istream.channels; ++k) {
			struct delay_channel_state *dest_cs = &dest_state->cs[k], *src_cs = &src_state->cs[k];
			dest_cs->samples_int += src_cs->samples_int;
			dest_cs->samples_frac += src_cs->samples_frac;
			dest_cs->fd_ap_n = MAXIMUM(dest_cs->fd_ap_n, src_cs->fd_ap_n);
		}
		return 1;
	}
	return 0;
}

int delay_effect_prepare(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	int is_zero = 1;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->fd_ap_n < 1)
			cs->fd_ap_n = DELAY_FD_AP_N_DEFAULT;
		/* ignore extremely small fractional delay */
		if (fabs(cs->samples_frac - rint(cs->samples_frac)) >= DBL_EPSILON) {
			const ssize_t adj = (cs->fd_ap_n-1) - ((ssize_t) floor(cs->samples_frac-DELAY_MIN_FRAC));
			cs->samples_int -= adj;
			cs->samples_frac += adj;
		}
		else {
			cs->samples_int += lrint(cs->samples_frac);
			cs->samples_frac = 0.0;
			cs->fd_ap_n = 0;
		}
		if (cs->samples_int < state->offset)
			state->offset = cs->samples_int;
	}
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		cs->samples_int -= state->offset;
		if (cs->fd_ap_n > 0) {
			if (cs->fd_ap_n > 1) {
				cs->run = delay_run_channel_frac_n;
				if ((cs->fd_ap.nth = thiran_ap_new(cs->fd_ap_n, fabs(cs->samples_frac))) == NULL) {
					LOG_FMT(LL_ERROR, "%s: error: thiran_ap_new() failed", e->name);
					return 1;
				}
			}
			else {
				cs->run = delay_run_channel_frac_1;
				cs->fd_ap.first.c0 = (1.0-fabs(cs->samples_frac)) / (1.0+fabs(cs->samples_frac));
			}
			LOG_FMT(LL_VERBOSE, "%s: info: channel %d: total delay is %gs (%zd%+g samples)",
				e->name, k, (cs->samples_int+cs->samples_frac) / e->istream.fs,
				cs->samples_int, cs->samples_frac);
		}
		else {
			cs->run = delay_run_channel;
			LOG_FMT(LL_VERBOSE, "%s: info: channel %d: total delay is %gs (%zd sample%s)",
				e->name, k, (double) cs->samples_int / e->istream.fs, cs->samples_int,
				(cs->samples_int == 1) ? "" : "s");
		}
		if (cs->samples_int > 0)
			cs->buf = calloc(cs->samples_int, sizeof(sample_t));
		if (cs->samples_int != 0 || cs->samples_frac != 0.0)
			is_zero = 0;
	}
	if (is_zero)
		e->run = delay_effect_run_noop;  /* nothing to do */

	return 0;
}

static struct effect * delay_effect_init_common(const char *name, const struct stream_info *istream, const char *channel_selector, ssize_t samples_int, double samples_frac, int fd_ap_n)
{
	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = name;
	if (samples_int == 0 && samples_frac == 0.0)
		return e;  /* nothing to do */
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->prepare = delay_effect_prepare;
	e->run = delay_effect_run;
	e->delay = delay_effect_delay;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->drain_samples = delay_effect_drain_samples;
	e->destroy = delay_effect_destroy;
	e->merge = delay_effect_merge;

	struct delay_state *state = calloc(1, sizeof(struct delay_state));
	e->data = state;
	state->cs = calloc(e->istream.channels, sizeof(struct delay_channel_state));
	for (int k = 0; k < e->istream.channels; ++k) {
		if (GET_BIT(channel_selector, k)) {
			state->cs[k].samples_int = samples_int;
			state->cs[k].samples_frac = samples_frac;
			state->cs[k].fd_ap_n = fd_ap_n;
		}
	}

	return e;
}

struct effect * delay_effect_init_int(const char *name, const struct stream_info *istream, const char *channel_selector, ssize_t samples_int)
{
	return delay_effect_init_common(name, istream, channel_selector, samples_int, 0.0, 0);
}

struct effect * delay_effect_init_frac(const char *name, const struct stream_info *istream, const char *channel_selector, double samples_frac, int fd_ap_n)
{
	return delay_effect_init_common(name, istream, channel_selector, 0, samples_frac, fd_ap_n);
}

struct effect * delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int do_frac = 0, fd_ap_n = 0, opt;

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

	if (do_frac) return delay_effect_init_frac(ei->name, istream, channel_selector, samples, fd_ap_n);
	return delay_effect_init_int(ei->name, istream, channel_selector, lrint(samples));
}
