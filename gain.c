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
#include <math.h>
#include "gain.h"
#include "util.h"

sample_t * gain_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	sample_t *state = (sample_t *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels)
		for (int k = 0; k < e->ostream.channels; ++k)
			ibuf[i + k] *= state[k];
	return ibuf;
}

sample_t * add_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	sample_t *state = (sample_t *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels)
		for (int k = 0; k < e->ostream.channels; ++k)
			ibuf[i + k] += state[k];
	return ibuf;
}

void gain_effect_plot(struct effect *e, int i)
{
	sample_t *state = (sample_t *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		printf("H%d_%d(w)=%.15e\n", k, i, state[k]);
}

void add_effect_plot(struct effect *e, int i)
{
	sample_t *state = (sample_t *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		printf("H%d_%d(w)=(w==0.0)?1.0+%.15e:1.0\n", k, i, state[k]);
}

void gain_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * gain_effect_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge == src->merge) {
		sample_t *dest_state = (sample_t *) dest->data;
		sample_t *src_state = (sample_t *) src->data;
		for (int k = 0; k < dest->ostream.channels; ++k)
			dest_state[k] *= src_state[k];
		return dest;
	}
	return NULL;
}

struct effect * add_effect_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge == src->merge) {
		sample_t *dest_state = (sample_t *) dest->data;
		sample_t *src_state = (sample_t *) src->data;
		for (int k = 0; k < dest->ostream.channels; ++k)
			dest_state[k] += src_state[k];
		return dest;
	}
	return NULL;
}

struct effect * gain_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	sample_t *state;
	double v;
	char *endptr;

	if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	const char *arg = argv[argc - 1];
	switch (ei->effect_number) {
	case GAIN_EFFECT_NUMBER_GAIN:
		v = pow(10.0, strtod(arg, &endptr) / 20.0);
		CHECK_ENDPTR(arg, endptr, "gain", return NULL);
		break;
	case GAIN_EFFECT_NUMBER_MULT:
		v = strtod(arg, &endptr);
		CHECK_ENDPTR(arg, endptr, "multiplier", return NULL);
		break;
	case GAIN_EFFECT_NUMBER_ADD:
		v = strtod(arg, &endptr);
		CHECK_ENDPTR(arg, endptr, "value", return NULL);
		break;
	default:
		LOG_FMT(LL_ERROR, "%s: BUG: unknown effect: %s (%d)", __FILE__, argv[0], ei->effect_number);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	sample_t v_noop;
	if (ei->effect_number == GAIN_EFFECT_NUMBER_ADD) {
		v_noop = 0.0;
		e->run = add_effect_run;
		e->plot = add_effect_plot;
		e->merge = add_effect_merge;
	}
	else {
		v_noop = 1.0;
		e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
		e->run = gain_effect_run;
		e->plot = gain_effect_plot;
		e->merge = gain_effect_merge;
	}
	e->destroy = gain_effect_destroy;
	state = calloc(istream->channels, sizeof(sample_t));
	for (int k = 0; k < istream->channels; ++k)
		state[k] = (GET_BIT(channel_selector, k)) ? v : v_noop;
	e->data = state;
	return e;
}
