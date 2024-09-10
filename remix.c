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

struct remix_state {
	char **channel_selectors;
};

sample_t * remix_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, j;
	struct remix_state *state = (struct remix_state *) e->data;
	for (i = 0; i < *frames; ++i) {
		for (k = 0; k < e->ostream.channels; ++k) {
			obuf[i * e->ostream.channels + k] = 0;
			for (j = 0; j < e->istream.channels; ++j) {
				if (GET_BIT(state->channel_selectors[k], j))
					obuf[i * e->ostream.channels + k] += ibuf[i * e->istream.channels + j];
			}
		}
	}
	return obuf;
}

void remix_effect_destroy(struct effect *e)
{
	int i;
	struct remix_state *state = (struct remix_state *) e->data;
	for (i = 0; i < e->ostream.channels; ++i)
		free(state->channel_selectors[i]);
	free(state->channel_selectors);
	free(state);
}

struct effect * remix_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	struct remix_state *state;
	int i, out_channels;

	if (argc <= 1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	out_channels = argc - 1;
	state = calloc(1, sizeof(struct remix_state));
	state->channel_selectors = calloc(out_channels, sizeof(char *));
	for (i = 0; i < out_channels; ++i) {
		state->channel_selectors[i] = NEW_SELECTOR(istream->channels);
		if (strcmp(argv[i + 1], ".") != 0 && parse_selector(argv[i + 1], state->channel_selectors[i], istream->channels))
			goto fail;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = out_channels;
	e->run = remix_effect_run;
	e->destroy = remix_effect_destroy;
	e->data = state;
	return e;

	fail:
	if (state->channel_selectors)
		for (i = 0; i < out_channels; ++i)
			free(state->channel_selectors[i]);
	free(state->channel_selectors);
	free(state);
	return NULL;
}
