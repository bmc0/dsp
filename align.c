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
#include "align.h"
#include "util.h"
#include "list_util.h"

struct align_channel_state {
	sample_t *buf;
	ssize_t len, p;
};

struct align_state {
	struct align_channel_state *cs;
	ssize_t frames, discard_frames;
};

static inline void align_channel_run(struct align_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride)
{
	for (ssize_t i = frames; i > 0; --i) {
		const sample_t s = *ibuf_p;
		*ibuf_p = cs->buf[cs->p];
		cs->buf[cs->p] = s;
		ibuf_p += stride;
		cs->p = (cs->p + 1 >= cs->len) ? 0 : cs->p + 1;
	}
}

sample_t * align_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct align_state *state = (struct align_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct align_channel_state *cs = &state->cs[k];
		if (cs->buf) align_channel_run(cs, *frames, &ibuf[k], e->istream.channels);
	}
#ifndef SYMMETRIC_IO
	if (state->frames < 0) {
		sample_t *ibuf_end = ibuf + *frames*e->istream.channels;
		state->frames += *frames;
		*frames = MAXIMUM(state->frames, 0);
		const ssize_t samples = *frames*e->istream.channels;
		memmove(ibuf, ibuf_end-samples, samples*sizeof(sample_t));
	}
	else state->frames += *frames;
#endif
	return ibuf;
}

ssize_t align_effect_delay(struct effect *e)
{
	struct align_state *state = (struct align_state *) e->data;
	return state->discard_frames;
}

void align_effect_reset(struct effect *e)
{
	struct align_state *state = (struct align_state *) e->data;
	state->frames = -state->discard_frames;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct align_channel_state *cs = &state->cs[k];
		cs->p = 0;
		if (cs->buf) memset(cs->buf, 0, cs->len * sizeof(sample_t));
	}
}

void align_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct align_state *state = (struct align_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		drain_samples[k] += state->cs[k].len;
}

void align_effect_destroy(struct effect *e)
{
	struct align_state *state = (struct align_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		free(state->cs[k].buf);
	free(state->cs);
	free(state);
}

int align_effect_insert(struct effects_chain *chain, struct effect *prev, ssize_t *offsets, ssize_t *align_refs)
{
	int do_align = 0;
	const char *next_name = (prev->next) ? prev->next->name : "[end of chain]";
	if (align_refs) {
		for (int k = 0; k < prev->ostream.channels; ++k)
			if (offsets[k] != align_refs[k]) { do_align = 1; break; }
	}
	else {
		for (int k = 0; k < prev->ostream.channels; ++k)
			if (offsets[k] != 0) { do_align = 1; break; }
	}
	if (!do_align) {
		LOG_FMT(LL_VERBOSE, "info: no alignment needed: %s", next_name);
		return 0;
	}

	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = "align";
	e->istream.fs = e->ostream.fs = prev->ostream.fs;
	e->istream.channels = e->ostream.channels = prev->ostream.channels;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->run = align_effect_run;
	e->delay = align_effect_delay;
	e->reset = align_effect_reset;
	e->plot = effect_plot_noop;
	e->drain_samples = align_effect_drain_samples;
	e->destroy = align_effect_destroy;

	struct align_state *state = calloc(1, sizeof(struct align_state));
	e->data = state;
	state->cs = calloc(e->istream.channels, sizeof(struct align_channel_state));
	ssize_t max_offset = offsets[0];
	for (int k = 1; k < e->istream.channels; ++k)
		max_offset = MAXIMUM(max_offset, offsets[k]);
	ssize_t min_ref = max_offset;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct align_channel_state *cs = &state->cs[k];
		const ssize_t ref = (align_refs) ? align_refs[k] : max_offset;
		min_ref = MINIMUM(min_ref, ref);
		if (offsets[k] != ref) {
			cs->len = ref-offsets[k];
			cs->buf = calloc(cs->len, sizeof(sample_t));
			LOG_FMT(LL_VERBOSE, "%s (%s): info: channel %d: %zd", e->name, next_name, k, cs->len);
		}
		else cs->len = 0;
		offsets[k] = ref;
	}
	if (min_ref > 0) {
		for (int k = 0; k < e->istream.channels; ++k)
			offsets[k] -= min_ref;
		state->discard_frames = min_ref;
		LOG_FMT(LL_VERBOSE, "%s (%s): info: discarding %zd frames", e->name, next_name, state->discard_frames);
	}
	state->frames = -state->discard_frames;

	LIST_INSERT(chain, e, prev);
	return 0;
}
