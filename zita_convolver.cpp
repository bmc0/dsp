/*
 * This file is part of dsp.
 *
 * Copyright (c) 2016-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <cstdio>
#include <cstdlib>
#include <zita-convolver.h>
#include "zita_convolver.h"

extern "C" {
	#include "util.h"
	#include "codec.h"
	#include "sampleconv.h"
	#include "fir_util.h"
}

struct zita_convolver_state {
	ssize_t filter_frames, len, pos, drain_frames, drain_pos;
	sample_t **output;
	Convproc *cproc;
	int has_output, is_draining;
};

sample_t * zita_convolver_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	ssize_t i, k, iframes = 0, oframes = 0;
	while (iframes < *frames) {
		while (state->pos < state->len && iframes < *frames) {
			for (i = k = 0; i < e->ostream.channels; ++i) {
				#ifdef SYMMETRIC_IO
					obuf[oframes * e->ostream.channels + i] = (state->has_output) ? state->output[i][state->pos] : 0.0;
				#else
					if (state->has_output)
						obuf[oframes * e->ostream.channels + i] = state->output[i][state->pos];
				#endif
				if (GET_BIT(e->channel_selector, i)) {
					state->cproc->inpdata(k)[state->pos] = ibuf[iframes * e->ostream.channels + i];
					++k;
				}
				else
					state->output[i][state->pos] = ibuf[iframes * e->ostream.channels + i];
			}
			#ifdef SYMMETRIC_IO
				++oframes;
			#else
				if (state->has_output)
					++oframes;
			#endif
			++iframes;
			++state->pos;
		}
		if (state->pos == state->len) {
			state->cproc->process(true);
			for (i = k = 0; i < e->ostream.channels; ++i) {
				if (GET_BIT(e->channel_selector, i)) {
					read_buf_float((char *) state->cproc->outdata(k), state->output[i], state->len);
					++k;
				}
			}
			state->pos = 0;
			state->has_output = 1;
		}
	}
	*frames = oframes;
	return obuf;
}

ssize_t zita_convolver_effect_delay(struct effect *e)
{
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	return (state->has_output) ? state->len : state->pos;
}

void zita_convolver_effect_reset(struct effect *e)
{
	/* Note: This doesn't reset zita_convolver's internal state */
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	state->pos = 0;
	state->has_output = 0;
}

sample_t * zita_convolver_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	sample_t *rbuf = buf1;
	if (!state->has_output && state->pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->filter_frames;
			#ifdef SYMMETRIC_IO
				state->drain_frames += state->len - state->pos;
			#else
				if (state->has_output)
					state->drain_frames += state->len - state->pos;
			#endif
			state->drain_frames += state->pos;
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			memset(buf1, 0, *frames * e->ostream.channels * sizeof(sample_t));
			rbuf = zita_convolver_effect_run(e, frames, buf1, buf2);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
	return rbuf;
}

void zita_convolver_effect_destroy(struct effect *e)
{
	int i;
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	if (!state->cproc->check_stop())
		state->cproc->stop_process();
	state->cproc->cleanup();
	delete state->cproc;
	for (i = 0; i < e->ostream.channels; ++i)
		free(state->output[i]);
	free(state->output);
	free(state);
	free(e->channel_selector);
}

static void write_buf_floatp(sample_t *in, float **out, int channels, ssize_t s)
{
	int c = channels;
	ssize_t in_s = s * channels;
	while (s-- > 0) {
		while(c-- > 0)
			out[c][s] = SAMPLE_TO_FLOAT(in[--in_s]);
		c = channels;
	}
}

struct effect * zita_convolver_effect_init_with_filter(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, sample_t *filter_data, int filter_channels, ssize_t filter_frames, int min_part_len, int max_part_len)
{
	struct effect *e;
	struct zita_convolver_state *state;

	const int n_channels = num_bits_set(channel_selector, istream->channels);
	if (n_channels > MINIMUM(Convproc::MAXINP, Convproc::MAXOUT)) {
		LOG_FMT(LL_ERROR, "%s: error: number of channels must not exceed %d", ei->name, MINIMUM(Convproc::MAXINP, Convproc::MAXOUT));
		return NULL;
	}
	if (filter_channels != 1 && filter_channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channel mismatch: channels=%d filter_channels=%d", ei->name, n_channels, filter_channels);
		return NULL;
	}
	if (filter_frames < 1) {
		LOG_FMT(LL_ERROR, "%s: error: filter length must be >= 1", ei->name);
		return NULL;
	}

	min_part_len = (min_part_len == 0) ? Convproc::MINPART : min_part_len;
	max_part_len = (max_part_len == 0) ? Convproc::MAXPART : max_part_len;
	if (min_part_len < Convproc::MINPART || min_part_len > Convproc::MAXPART || max_part_len < Convproc::MINPART || max_part_len > Convproc::MAXPART) {
		LOG_FMT(LL_ERROR, "%s: error: partition lengths must be within [%d,%d] or 0 for default", ei->name, Convproc::MINPART, Convproc::MAXPART);
		return NULL;
	}
	if (max_part_len < min_part_len) {
		LOG_FMT(LL_ERROR, "%s: warning: max_part_len < min_part_len", ei->name);
		max_part_len = min_part_len;
	}

	Convproc *cproc = new Convproc;
#if ZITA_CONVOLVER_MAJOR_VERSION >= 4
	if (cproc->configure(n_channels, n_channels, filter_frames, min_part_len, min_part_len, max_part_len, 0.0f)) {
#else
	if (cproc->configure(n_channels, n_channels, filter_frames, min_part_len, min_part_len, max_part_len)) {
#endif
		LOG_FMT(LL_ERROR, "%s: error: failed to configure convolution engine", ei->name);
		delete cproc;
		return NULL;
	}
	LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd min_part_len=%d max_part_len=%d", ei->name, filter_frames, min_part_len, max_part_len);

	e = (struct effect *) calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = (char *) NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->run = zita_convolver_effect_run;
	e->delay = zita_convolver_effect_delay;
	e->reset = zita_convolver_effect_reset;
	e->drain2 = zita_convolver_effect_drain2;
	e->destroy = zita_convolver_effect_destroy;

	state = (struct zita_convolver_state *) calloc(1, sizeof(struct zita_convolver_state));
	state->filter_frames = filter_frames;
	state->len = min_part_len;
	state->cproc = cproc;
	state->output = (sample_t **) calloc(istream->channels, sizeof(sample_t *));
	for (int i = 0; i < istream->channels; ++i)
		state->output[i] = (sample_t *) calloc(state->len, sizeof(sample_t));
	e->data = (void *) state;

	float **buf_planar = (float **) calloc(filter_channels, sizeof(float *));
	for (int i = 0; i < filter_channels; ++i)
		buf_planar[i] = (float *) calloc(filter_frames, sizeof(float));
	write_buf_floatp(filter_data, buf_planar, filter_channels, filter_frames);
	for (int i = 0, k = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (filter_channels == 1 && k > 0) cproc->impdata_link(0, 0, k, k);
			else cproc->impdata_create(k, k, 1, buf_planar[k], 0, filter_frames);
			++k;
		}
	}
	for (int i = 0; i < filter_channels; ++i)
		free(buf_planar[i]);
	free(buf_planar);

	cproc->start_process(0, 0);
	return e;
}

struct effect * zita_convolver_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int filter_channels, min_part_len = 0, max_part_len = 0;
	ssize_t filter_frames;
	struct effect *e;
	sample_t *filter_data;
	struct codec_params c_params;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	char *endptr;

	int err = fir_parse_opts(ei, istream, &c_params, &g, argc, argv, NULL, NULL, NULL);
	if (err || g.ind < argc-3 || g.ind > argc-1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (g.ind <= argc-2) {
		min_part_len = strtol(argv[g.ind], &endptr, 10);
		CHECK_ENDPTR(argv[g.ind], endptr, "min_part_len", return NULL);
		++g.ind;
	}
	if (g.ind <= argc-2) {
		max_part_len = strtol(argv[g.ind], &endptr, 10);
		CHECK_ENDPTR(argv[g.ind], endptr, "max_part_len", return NULL);
		++g.ind;
	}
	c_params.path = argv[g.ind];
	filter_data = fir_read_filter(ei, istream, dir, &c_params, &filter_channels, &filter_frames);
	if (filter_data == NULL)
		return NULL;
	e = zita_convolver_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, min_part_len, max_part_len);
	free(filter_data);
	return e;
}
