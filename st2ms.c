/*
 * This file is part of dsp.
 *
 * Copyright (c) 2018-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "st2ms.h"
#include "util.h"

struct st2ms_state {
	int c0, c1;
};

static sample_t * st2ms_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	struct st2ms_state *state = (struct st2ms_state *) e->data;

	for (ssize_t i = 0; i < samples; i += e->ostream.channels) {
		sample_t s0 = ibuf[i + state->c0];
		sample_t s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = (s0 + s1) * 0.5;
		ibuf[i + state->c1] = (s0 - s1) * 0.5;
	}
	return ibuf;
}

static sample_t * ms2st_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	struct st2ms_state *state = (struct st2ms_state *) e->data;

	for (ssize_t i = 0; i < samples; i += e->ostream.channels) {
		sample_t s0 = ibuf[i + state->c0];
		sample_t s1 = ibuf[i + state->c1];
		ibuf[i + state->c0] = (s0 + s1);
		ibuf[i + state->c1] = (s0 - s1);
	}
	return ibuf;
}

static void st2ms_effect_plot(struct effect *e, int i)
{
	struct st2ms_state *state = (struct st2ms_state *) e->data;
	const int is_ms2st = (e->run == ms2st_effect_run);
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (k == state->c0)
			printf("H%d_%d(w)=(Ht%d_%d(w*%d/2.0/pi)+Ht%d_%d(w*%d/2.0/pi))*%g\n",
				k, i, state->c0, i, e->ostream.fs, state->c1, i, e->ostream.fs, (is_ms2st)?1.0:0.5);
		else if (k == state->c1)
			printf("H%d_%d(w)=(Ht%d_%d(w*%d/2.0/pi)-Ht%d_%d(w*%d/2.0/pi))*%g\n",
				k, i, state->c0, i, e->ostream.fs, state->c1, i, e->ostream.fs, (is_ms2st)?1.0:0.5);
		else
			printf("H%d_%d(w)=Ht%d_%d(w*%d/2.0/pi)\n", k, i, k, i, e->ostream.fs);
	}
}

static void st2ms_effect_destroy(struct effect *e)
{
	free(e->data);
}

static void st2ms_effect_channel_deps(struct effect *e, char **deps)
{
	struct st2ms_state *state = (struct st2ms_state *) e->data;
	SET_BIT(deps[state->c0], state->c1);
	SET_BIT(deps[state->c1], state->c0);
}

struct effect * st2ms_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	if (argc != 1) {
		print_effect_usage(ei);
		return NULL;
	}
	if (num_bits_set(channel_selector, istream->channels) != 2) {
		dsp_perror(DSP_ERANGE, argv[0], "input channels must be 2");
		return NULL;
	}

	struct effect *e = calloc(1, sizeof(struct effect));
	if (check_alloc(ei->name, e)) return NULL;
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_PLOT_MIX;
	switch (ei->effect_number) {
	case ST2MS_EFFECT_NUMBER_ST2MS:
		e->run = st2ms_effect_run;
		break;
	case ST2MS_EFFECT_NUMBER_MS2ST:
		e->run = ms2st_effect_run;
		break;
	default:
		dsp_perror(DSP_ENOEFFNUM, __FILE__, NULL);
		free(e);
		return NULL;
	}
	e->plot = st2ms_effect_plot;
	e->destroy = st2ms_effect_destroy;
	e->channel_deps = st2ms_effect_channel_deps;

	struct st2ms_state *state = calloc(1, sizeof(struct st2ms_state));
	if (check_alloc(ei->name, state)) {
		free(e);
		return NULL;
	}
	e->data = state;
	state->c0 = state->c1 = -1;
	for (int i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (state->c0 == -1) state->c0 = i;
			else state->c1 = i;
		}
	}
	return e;
}
