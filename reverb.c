/*
 * Ported from libSoX reverb.c
 *
 * Copyright (c) 2007 robs@users.sourceforge.net
 * Filter design based on freeverb by Jezar at Dreampoint.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "reverb.h"
#include "util.h"

#define zalloc(var, n) var = calloc(n, sizeof(*var))
#define filter_advance(p) if (--(p)->ptr < (p)->buffer) (p)->ptr += (p)->size
#define filter_delete(p) free((p)->buffer)
#define FIFO_MIN 0x4000
#define fifo_read_ptr(f) fifo_read(f, (size_t)0, NULL)

typedef struct {
	char * data;
	size_t allocation;  /* Number of bytes allocated for data. */
	size_t item_size;   /* Size of each item in data */
	size_t begin;       /* Offset of the first byte to read. */
	size_t end;         /* 1 + Offset of the last byte byte to read. */
} fifo_t;

static void fifo_clear(fifo_t *f)
{
	f->end = f->begin = 0;
}

static void * fifo_reserve(fifo_t *f, size_t n)
{
	n *= f->item_size;

	if (f->begin == f->end)
		fifo_clear(f);

	while (1) {
		if (f->end + n <= f->allocation) {
			void *p = f->data + f->end;

			f->end += n;
			return p;
		}
		if (f->begin > FIFO_MIN) {
			memmove(f->data, f->data + f->begin, f->end - f->begin);
			f->end -= f->begin;
			f->begin = 0;
			continue;
		}
		f->allocation += n;
		f->data = realloc(f->data, f->allocation);
	}
}

static void * fifo_write(fifo_t *f, size_t n, void const *data)
{
	void *s = fifo_reserve(f, n);
	if (data)
		memcpy(s, data, n * f->item_size);
	return s;
}

static void * fifo_read(fifo_t *f, size_t n, void *data)
{
	char *ret = f->data + f->begin;
	n *= f->item_size;
	if (n > (size_t)(f->end - f->begin))
		return NULL;
	if (data)
		memcpy(data, ret, (size_t)n);
	f->begin += n;
	return ret;
}

static void fifo_delete(fifo_t *f)
{
	free(f->data);
}

static void fifo_create(fifo_t *f, size_t item_size)
{
	f->item_size = item_size;
	f->allocation = FIFO_MIN;
	f->data = malloc(f->allocation);
	fifo_clear(f);
}

typedef struct {
	size_t	size;
	sample_t *buffer, *ptr;
	sample_t store;
} filter_t;

static sample_t comb_process(filter_t *p, sample_t const *input, sample_t const *feedback, sample_t const *hf_damping)
{
	sample_t output = *p->ptr;
	p->store = output + (p->store - output) * *hf_damping;
	*p->ptr = *input + p->store * *feedback;
	filter_advance(p);
	return output;
}

static sample_t allpass_process(filter_t *p, sample_t const *input)
{
	sample_t output = *p->ptr;
	*p->ptr = *input + output * .5;
	filter_advance(p);
	return output - *input;
}

static const size_t /* Filter delay lengths in samples (44100Hz sample-rate) */
	comb_lengths[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617},
	allpass_lengths[] = {225, 341, 441, 556};
#define stereo_adjust 12

typedef struct {
	filter_t comb[LENGTH(comb_lengths)];
	filter_t allpass[LENGTH(allpass_lengths)];
} filter_array_t;

static void filter_array_create(filter_array_t * p, double rate, double scale, double offset)
{
	size_t i;
	double r = rate * (1 / 44100.); /* Compensate for actual sample-rate */

	for (i = 0; i < LENGTH(comb_lengths); ++i, offset = -offset)
	{
		filter_t *pcomb = &p->comb[i];
		pcomb->size = (size_t)(scale * r * (comb_lengths[i] + stereo_adjust * offset) + .5);
		pcomb->ptr = zalloc(pcomb->buffer, pcomb->size);
	}
	for (i = 0; i < LENGTH(allpass_lengths); ++i, offset = -offset)
	{
		filter_t *pallpass = &p->allpass[i];
		pallpass->size = (size_t)(r * (allpass_lengths[i] + stereo_adjust * offset) + .5);
		pallpass->ptr = zalloc(pallpass->buffer, pallpass->size);
	}
}

static void filter_array_process(filter_array_t *p, size_t length, sample_t const *input, sample_t *output, sample_t const *feedback, sample_t const *hf_damping, sample_t const *gain)
{
	while (length--) {
		sample_t out = 0, in = *input++;

		size_t i = LENGTH(comb_lengths) - 1;
		do out += comb_process(p->comb + i, &in, feedback, hf_damping);
		while (i--);

		i = LENGTH(allpass_lengths) - 1;
		do out = allpass_process(p->allpass + i, &out);
		while (i--);

		*output++ = out * *gain;
	}
}

static void filter_array_delete(filter_array_t *p)
{
	size_t i;
	for (i = 0; i < LENGTH(allpass_lengths); ++i)
		filter_delete(&p->allpass[i]);
	for (i = 0; i < LENGTH(comb_lengths); ++i)
		filter_delete(&p->comb[i]);
}

typedef struct {
	sample_t feedback;
	sample_t hf_damping;
	sample_t gain;
	fifo_t input_fifo;
	filter_array_t chan[2];
	sample_t *out[2];
} reverb_t;

static void reverb_create(reverb_t *p, double sample_rate_Hz,
		double wet_gain_dB,
		double room_scale,   /* % */
		double reverberance, /* % */
		double hf_damping,   /* % */
		double pre_delay_ms,
		double stereo_depth,
		size_t buffer_size,
		sample_t **out)
{
	size_t i, delay = pre_delay_ms / 1000 * sample_rate_Hz + .5;
	double scale = room_scale / 100 * .9 + .1;
	double depth = stereo_depth / 100;
	double a = -1 / log(1 - /**/.3 /**/);             /* Set minimum feedback */
	double b = 100 / (log(1 - /**/.98/**/) * a + 1);  /* Set maximum feedback */

	memset(p, 0, sizeof(*p));
	p->feedback = 1 - exp((reverberance - b) / (a * b));
	p->hf_damping = hf_damping / 100 * .3 + .2;
	p->gain = pow(10.0, wet_gain_dB / 20.0) * .015;
	fifo_create(&p->input_fifo, sizeof(sample_t));
	memset(fifo_write(&p->input_fifo, delay, 0), 0, delay * sizeof(sample_t));
	for (i = 0; i <= ceil(depth); ++i) {
		filter_array_create(p->chan + i, sample_rate_Hz, scale, i * depth);
		out[i] = zalloc(p->out[i], buffer_size);
	}
}

static void reverb_process(reverb_t *p, size_t length)
{
	size_t i;
	for (i = 0; i < 2 && p->out[i]; ++i)
		filter_array_process(p->chan + i, length, (sample_t *) fifo_read_ptr(&p->input_fifo), p->out[i], &p->feedback, &p->hf_damping, &p->gain);
	fifo_read(&p->input_fifo, length, NULL);
}

static void reverb_delete(reverb_t *p)
{
	size_t i;
	for (i = 0; i < 2 && p->out[i]; ++i) {
		free(p->out[i]);
		filter_array_delete(p->chan + i);
	}
	fifo_delete(&p->input_fifo);
}

/*------------------------------- dsp wrapper --------------------------------*/

struct reverb_channel {
	reverb_t reverb;
	sample_t *dry, *wet[2];
};

struct reverb_state {
	int n_channels, c1, c2, wet_only;
	/* sample_t in_signal_mult; */
	size_t buf_size;
	struct reverb_channel *chan;
};

void reverb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, c, f = 0, len;
	struct reverb_state *state = (struct reverb_state *) e->data;

	while (f < *frames) {
		len = (*frames - f > state->buf_size) ? state->buf_size : *frames - f;
		for (c = 0; c < e->istream.channels; ++c)
			if (GET_BIT(e->channel_selector, c))
				state->chan[c].dry = fifo_write(&state->chan[c].reverb.input_fifo, len, 0);
		for (i = 0; i < len; ++i) {
			for (c = 0; c < e->istream.channels; ++c) {
				if (GET_BIT(e->channel_selector, c))
					state->chan[c].dry[i] = ibuf[(f + i) * e->istream.channels + c] /* * state->in_signal_mult */;
				else
					obuf[(f + i) * e->istream.channels + c] = ibuf[(f + i) * e->istream.channels + c];
			}
		}
		for (c = 0; c < e->istream.channels; ++c)
			if (GET_BIT(e->channel_selector, c))
				reverb_process(&state->chan[c].reverb, len);
		if (state->n_channels == 2) {
			for (i = 0; i < len; ++i) {
				obuf[(f + i) * e->istream.channels + state->c1] = (1 - state->wet_only) * state->chan[state->c1].dry[i] + 0.5 * (state->chan[state->c1].wet[0][i] + state->chan[state->c2].wet[0][i]);
				obuf[(f + i) * e->istream.channels + state->c2] = (1 - state->wet_only) * state->chan[state->c2].dry[i] + 0.5 * (state->chan[state->c1].wet[1][i] + state->chan[state->c2].wet[1][i]);
			}
		}
		else {
			for (i = 0; i < len; ++i)
				for (c = 0; c < e->istream.channels; ++c)
					if (GET_BIT(e->channel_selector, c))
						obuf[(f + i) * e->istream.channels + c] = (1 - state->wet_only) * state->chan[c].dry[i] + state->chan[c].wet[0][i];
		}
		f += len;
	}
}

void reverb_effect_destroy(struct effect *e)
{
	int i;
	struct reverb_state *state = (struct reverb_state *) e->data;
	for (i = 0; i < e->istream.channels; ++i)
		reverb_delete(&state->chan[i].reverb);
	free(state->chan);
	free(state);
	free(e->channel_selector);
}

struct effect * reverb_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct reverb_state *state;
	double reverberance = 50.0, hf_damping = 50.0, pre_delay_ms = 0.0;
	double stereo_depth = 100.0, wet_gain_dB = 0, room_scale = 100.0;
	int i, wet_only = 0;

	i = 0;
	if (argc > i + 1 && strcmp(argv[i + 1], "-w") == 0) {
		wet_only = 1;
		++i;
	}
	if (argc > ++i) {
		reverberance = atof(argv[i]);
		CHECK_RANGE(reverberance >= 0.0 && reverberance <= 100.0, "reverberance", return NULL);
	}
	if (argc > ++i) {
		hf_damping = atof(argv[i]);
		CHECK_RANGE(hf_damping >= 0.0 && hf_damping <= 100.0, "hf_damping", return NULL);
	}
	if (argc > ++i) {
		room_scale = atof(argv[i]);
		CHECK_RANGE(room_scale > 0.0 && room_scale <= 100.0, "room_scale", return NULL);
	}
	if (argc > ++i) {
		stereo_depth = atof(argv[i]);
		CHECK_RANGE(stereo_depth > 0.0 && stereo_depth <= 100.0, "stereo_depth", return NULL;);
	}
	if (argc > ++i) {
		pre_delay_ms = atof(argv[i]) * 1000.0;
		CHECK_RANGE(pre_delay_ms >= 0.0 && pre_delay_ms <= 500.0, "pre_delay", return NULL);
	}
	if (argc > ++i) {
		wet_gain_dB = atof(argv[i]);
		CHECK_RANGE(wet_gain_dB >= -20.0 && wet_gain_dB <= 10.0, "wet_gain", return NULL);
	}
	if (argc > 8 || (!wet_only && argc > 7)) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}

	LOG(LL_VERBOSE, "dsp: %s: info: wet_only=%s reverberance=%.1f hf_damping=%.1f room_scale=%.1f stereo_depth=%.1f pre_delay=%.4f wet_gain=%.2f\n", argv[0], (wet_only) ? "true" : "false", reverberance, hf_damping, room_scale, stereo_depth, pre_delay_ms / 1000.0, wet_gain_dB);

	state = calloc(1, sizeof(struct reverb_state));
	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++state->n_channels;
	if (state->n_channels != 2 && stereo_depth != 0.0) {
		LOG(LL_NORMAL, "dsp: %s: warning: stereo_depth not applicable when channels != 2\n", argv[0]);
		stereo_depth = 0.0;
	}
	if (state->n_channels == 2) {
		state->c1 = state->c2 = -1;
		for (i = 0; i < istream->channels; ++i) {
			if (GET_BIT(channel_selector, i)) {
				if (state->c1 == -1)
					state->c1 = i;
				else
					state->c2 = i;
			}
		}
	}
	state->buf_size = dsp_globals.buf_frames;  /* input will be processed in blocks */
	state->chan = calloc(istream->channels, sizeof(struct reverb_channel));
	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			reverb_create(&state->chan[i].reverb, istream->fs, wet_gain_dB, room_scale, reverberance, hf_damping, pre_delay_ms, stereo_depth, state->buf_size, state->chan[i].wet);
	/* state->in_signal_mult = 1.0 / ((1 - state->wet_only) + 2.0 * pow(10.0, wet_gain_dB / 20.0)); */
	state->wet_only = wet_only;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = reverb_effect_run;
	e->destroy = reverb_effect_destroy;
	e->data = state;

	return e;
}
