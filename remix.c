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
#include "remix.h"
#include "util.h"

#define DEBUG_CHANNEL_MAPPING 0

struct fast_sel_4 {
	int n, c[4];
};

struct remix_state {
	char **channel_selectors;
	union {
		int *s1;
		struct fast_sel_4 *s4;
	} fast_sel;
};

sample_t * remix_effect_run_generic(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct remix_state *state = (struct remix_state *) e->data;
	sample_t *ibuf_p = ibuf, *obuf_p = obuf;
	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->ostream.channels; ++k) {
			obuf_p[k] = 0.0;
			for (int j = 0; j < e->istream.channels; ++j) {
				if (GET_BIT(state->channel_selectors[k], j))
					obuf_p[k] += ibuf_p[j];
			}
		}
		ibuf_p += e->istream.channels;
		obuf_p += e->ostream.channels;
	}
	return obuf;
}

sample_t * remix_effect_run_1a(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct remix_state *state = (struct remix_state *) e->data;
	sample_t *ibuf_p = ibuf, *obuf_p = obuf;
	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->ostream.channels; ++k)
			obuf_p[k] = ibuf_p[state->fast_sel.s1[k]];
		ibuf_p += e->istream.channels;
		obuf_p += e->ostream.channels;
	}
	return obuf;
}

sample_t * remix_effect_run_4(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct remix_state *state = (struct remix_state *) e->data;
	sample_t *ibuf_p = ibuf, *obuf_p = obuf;
	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->ostream.channels; ++k) {
			struct fast_sel_4 *sel = &state->fast_sel.s4[k];
			obuf_p[k] = 0.0;
			if (sel->n == 1) {
				obuf_p[k] += ibuf_p[sel->c[0]];
			}
			else if (sel->n == 2) {
				obuf_p[k] += ibuf_p[sel->c[0]];
				obuf_p[k] += ibuf_p[sel->c[1]];
			}
			else if (sel->n == 3) {
				obuf_p[k] += ibuf_p[sel->c[0]];
				obuf_p[k] += ibuf_p[sel->c[1]];
				obuf_p[k] += ibuf_p[sel->c[2]];
			}
			else if (sel->n == 4) {
				obuf_p[k] += ibuf_p[sel->c[0]];
				obuf_p[k] += ibuf_p[sel->c[1]];
				obuf_p[k] += ibuf_p[sel->c[2]];
				obuf_p[k] += ibuf_p[sel->c[3]];
			}
		}
		ibuf_p += e->istream.channels;
		obuf_p += e->ostream.channels;
	}
	return obuf;
}

void remix_effect_plot(struct effect *e, int i)
{
	struct remix_state *state = (struct remix_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		printf("H%d_%d(w)=0.0", k, i);
		for (int j = 0; j < e->istream.channels; ++j) {
			if (GET_BIT(state->channel_selectors[k], j))
				printf("+Ht%d_%d(w*%d/2.0/pi)", j, i, e->ostream.fs);
		}
		putchar('\n');
	}
}

void remix_effect_destroy(struct effect *e)
{
	struct remix_state *state = (struct remix_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		free(state->channel_selectors[k]);
	free(state->channel_selectors);
	free(state->fast_sel.s1);
	free(state);
}

struct effect * remix_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	struct remix_state *state;

	if (argc <= 1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	const int n_selectors = argc - 1, mask_bits = num_bits_set(channel_selector, istream->channels);
	const int delta = n_selectors - mask_bits;
	const int out_channels = istream->channels + delta;

	state = calloc(1, sizeof(struct remix_state));
	state->channel_selectors = calloc(out_channels, sizeof(char *));
	int use_run_1a = 1, use_run_4 = 1;
	for (int k = 0, i = 0, ch = 0; k < out_channels; ++k, ++ch) {
		state->channel_selectors[k] = NEW_SELECTOR(istream->channels);
		if (ch >= istream->channels || GET_BIT(channel_selector, ch)) {
			if (i < n_selectors) {
				if (strcmp(argv[i+1], ".") != 0 && parse_selector_masked(argv[i+1], state->channel_selectors[k], channel_selector, istream->channels))
					goto fail;
				const int n_sel = num_bits_set(state->channel_selectors[k], istream->channels);
				if (n_sel != 1) use_run_1a = 0;
				if (n_sel > 4) use_run_4 = 0;
				++i;
			}
			else {
				while (ch < istream->channels && GET_BIT(channel_selector, ch)) ++ch;
				if (ch < istream->channels) SET_BIT(state->channel_selectors[k], ch);
			}
		}
		else SET_BIT(state->channel_selectors[k], ch);

		#if DEBUG_CHANNEL_MAPPING
			fprintf(stderr, "%s: %s: info: channel map: %d <- ", dsp_globals.prog_name, argv[0], k);
			print_selector(state->channel_selectors[k], istream->channels);
			fputc('\n', stderr);
		#endif
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = out_channels;
	e->plot_info |= PLOT_INFO_MIX;
	if (use_run_1a) {
		state->fast_sel.s1 = calloc(out_channels, sizeof(int));
		for (int k = 0; k < out_channels; ++k) {
			int ch = 0;
			while (ch < istream->channels && !GET_BIT(state->channel_selectors[k], ch)) ++ch;
			state->fast_sel.s1[k] = ch;
		}
		e->run = remix_effect_run_1a;
	}
	else if (use_run_4) {
		state->fast_sel.s4 = calloc(out_channels, sizeof(struct fast_sel_4));
		for (int k = 0; k < out_channels; ++k) {
			int n = 0;
			for (int ch = 0; ch < istream->channels; ++ch)
				if (GET_BIT(state->channel_selectors[k], ch))
					state->fast_sel.s4[k].c[n++] = ch;
			state->fast_sel.s4[k].n = n;
		}
		e->run = remix_effect_run_4;
	}
	else e->run = remix_effect_run_generic;
	e->plot = remix_effect_plot;
	e->destroy = remix_effect_destroy;
	e->data = state;
	return e;

	fail:
	if (state->channel_selectors)
		for (int k = 0; k < out_channels; ++k)
			free(state->channel_selectors[k]);
	free(state->channel_selectors);
	free(state->fast_sel.s1);
	free(state);
	return NULL;
}
