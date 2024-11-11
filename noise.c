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
#include <math.h>
#include "noise.h"
#include "util.h"

struct noise_state {
	sample_t mult;
};

sample_t * noise_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct noise_state *state = (struct noise_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels)
		for (k = 0; k < e->ostream.channels; ++k)
			if (GET_BIT(e->channel_selector, k))
				ibuf[i + k] += tpdf_noise(state->mult);
	return ibuf;
}

void noise_effect_plot(struct effect *e, int i)
{
	struct noise_state *state = (struct noise_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		printf("H%d_%d(w)=Ht%d_%d(w*%d/2.0/pi)", k, i, k, i, e->ostream.fs);
		if (GET_BIT(e->channel_selector, k))
			printf("+%.15e*((rand(0)-rand(0))+j*(rand(0)-rand(0)))/sqrt(2.0)", state->mult*PM_RAND_MAX);
		putchar('\n');
	}
}

void noise_effect_destroy(struct effect *e)
{
	free(e->data);
	free(e->channel_selector);
}

struct effect * noise_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	double mult;
	char *endptr;
	struct effect *e;
	struct noise_state *state;

	if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage %s", argv[0], ei->usage);
		return NULL;
	}

	mult = pow(10.0, strtod(argv[1], &endptr) / 20.0) / PM_RAND_MAX;
	CHECK_ENDPTR(argv[1], endptr, "level", return NULL);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->plot_info |= PLOT_INFO_MIX;
	e->run = noise_effect_run;
	e->plot = noise_effect_plot;
	e->destroy = noise_effect_destroy;
	state = calloc(1, sizeof(struct noise_state));
	state->mult = mult;
	e->data = state;
	return e;
}
