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
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "fir.h"
#include "util.h"
#include "codec.h"

#define MAX_DIRECT_LEN (1<<4)

struct fir_direct_state {
	ssize_t len, mask, p, filter_frames, drain_frames;
	sample_t **filter, **buf;
	int has_output, is_draining;
};

struct fir_state {
	ssize_t len, fr_len, p, filter_frames, drain_pos, drain_frames;
	fftw_complex **filter_fr, *tmp_fr;
	sample_t **input, **output, **overlap;
	fftw_plan *r2c_plan, *c2r_plan;
	int has_output, is_draining;
};

sample_t * fir_direct_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	ssize_t i, k;

	for (i = 0; i < *frames; ++i) {
		for (k = 0; k < e->istream.channels; ++k) {
			if (state->buf[k]) {
				const sample_t s = ibuf[i*e->istream.channels + k];
				for (ssize_t n = state->p, m = 0; m < state->len; ++m) {
					state->buf[k][n] += s * state->filter[k][m];
					n = (n+1) & state->mask;
				}
				ibuf[i*e->istream.channels + k] = state->buf[k][state->p];
				state->buf[k][state->p] = 0.0;
			}
		}
		state->p = (state->p+1) & state->mask;
	}
	if (*frames > 0)
		state->has_output = 1;

	return ibuf;
}

void fir_direct_effect_reset(struct effect *e)
{
	int i;
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	state->p = 0;
	for (i = 0; i < e->ostream.channels; ++i)
		if (state->buf[i])
			memset(state->buf[i], 0, state->len * sizeof(sample_t));
}

void fir_direct_effect_plot(struct effect *e, int i)
{
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->buf[k]) {
			printf("H%d_%d(w)=(abs(w)<=pi)?0.0", k, i);
			for (ssize_t j = 0; j < state->len; ++j)
				printf("+exp(-j*w*%zd)*%.15e", j, state->filter[k][j]);
			puts(":0/0");
		}
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void fir_direct_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	if (!state->has_output && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->filter_frames;
			state->is_draining = 1;
		}
		if (state->drain_frames > 0) {
			*frames = MINIMUM(*frames, state->drain_frames);
			state->drain_frames -= *frames;
			memset(obuf, 0, *frames * e->istream.channels * sizeof(sample_t));
			fir_direct_effect_run(e, frames, obuf, NULL);
		}
		else
			*frames = -1;
	}
}

void fir_direct_effect_destroy(struct effect *e)
{
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	for (int i = 0; i < e->ostream.channels; ++i) {
		if (state->buf[i]) {
			free(state->filter[i]);
			free(state->buf[i]);
			break;
		}
	}
	free(state->filter);
	free(state->buf);
	free(state);
}

sample_t * fir_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	ssize_t i, k, iframes = 0, oframes = 0;

	while (iframes < *frames) {
		while (state->p < state->len && iframes < *frames) {
			for (i = 0; i < e->ostream.channels; ++i) {
				#ifdef SYMMETRIC_IO
					obuf[oframes * e->ostream.channels + i] = (state->has_output) ? state->output[i][state->p] : 0.0;
				#else
					if (state->has_output)
						obuf[oframes * e->ostream.channels + i] = state->output[i][state->p];
				#endif
				if (state->input[i])
					state->input[i][state->p] = ibuf[iframes * e->ostream.channels + i];
				else
					state->output[i][state->p] = ibuf[iframes * e->ostream.channels + i];
			}
			#ifdef SYMMETRIC_IO
				++oframes;
			#else
				if (state->has_output)
					++oframes;
			#endif
			++iframes;
			++state->p;
		}

		if (state->p == state->len) {
			for (i = 0; i < e->ostream.channels; ++i) {
				if (state->input[i]) {
					fftw_execute(state->r2c_plan[i]);
					for (k = 0; k < state->fr_len; ++k)
						state->tmp_fr[k] *= state->filter_fr[i][k];
					fftw_execute(state->c2r_plan[i]);
					for (k = 0; k < state->len * 2; ++k)
						state->output[i][k] /= state->len * 2;
					for (k = 0; k < state->len; ++k) {
						state->output[i][k] += state->overlap[i][k];
						state->overlap[i][k] = state->output[i][k + state->len];
					}
				}
			}
			state->p = 0;
			state->has_output = 1;
		}
	}
	*frames = oframes;
	return obuf;
}

ssize_t fir_effect_delay(struct effect *e)
{
	struct fir_state *state = (struct fir_state *) e->data;
	return (state->has_output) ? state->len : state->p;
}

void fir_effect_reset(struct effect *e)
{
	int i;
	struct fir_state *state = (struct fir_state *) e->data;
	state->p = 0;
	state->has_output = 0;
	for (i = 0; i < e->ostream.channels; ++i)
		if (state->overlap[i])
			memset(state->overlap[i], 0, state->len * sizeof(sample_t));
}

void fir_effect_plot(struct effect *e, int i)
{
	struct fir_state *state = (struct fir_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->input[k]) {
			for (ssize_t j = 0; j < state->fr_len; ++j)
				state->tmp_fr[j] = state->filter_fr[k][j];
			fftw_execute(state->c2r_plan[k]);
			for (ssize_t j = 0; j < state->len * 2; ++j)
				state->output[k][j] /= state->len * 2;
			printf("H%d_%d(w)=(abs(w)<=pi)?0.0", k, i);
			for (ssize_t j = 0; j < state->len; ++j)
				printf("+exp(-j*w*%zd)*%.15e", j, state->output[k][j]);
			puts(":0/0");
		}
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

sample_t * fir_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct fir_state *state = (struct fir_state *) e->data;
	sample_t *rbuf = buf1;
	if (!state->has_output && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->filter_frames;
			#ifdef SYMMETRIC_IO
				state->drain_frames += state->len - state->p;
			#else
				if (state->has_output)
					state->drain_frames += state->len - state->p;
			#endif
			state->drain_frames += state->p;
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			memset(buf1, 0, *frames * e->ostream.channels * sizeof(sample_t));
			rbuf = fir_effect_run(e, frames, buf1, buf2);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
	return rbuf;
}

void fir_effect_destroy(struct effect *e)
{
	int i;
	struct fir_state *state = (struct fir_state *) e->data;
	for (i = 0; i < e->ostream.channels; ++i) {
		fftw_free(state->input[i]);
		fftw_free(state->output[i]);
		fftw_free(state->overlap[i]);
		fftw_free(state->filter_fr[i]);
		fftw_destroy_plan(state->r2c_plan[i]);
		fftw_destroy_plan(state->c2r_plan[i]);
	}
	free(state->input);
	free(state->output);
	free(state->overlap);
	free(state->filter_fr);
	fftw_free(state->tmp_fr);
	free(state->r2c_plan);
	free(state->c2r_plan);
	free(state);
}

struct effect * fir_effect_init_with_filter(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, sample_t *filter_data, int filter_channels, ssize_t filter_frames, int force_direct)
{
	int i, k;
	ssize_t j;
	struct effect *e;

	const int n_channels = num_bits_set(channel_selector, istream->channels);
	if (filter_channels != 1 && filter_channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channel mismatch: channels=%d filter_channels=%d", ei->name, n_channels, filter_channels);
		return NULL;
	}
	if (filter_frames < 1) {
		LOG_FMT(LL_ERROR, "%s: error: filter length must be >= 1", ei->name);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;

	if (filter_frames <= MAX_DIRECT_LEN || force_direct) {
		e->run = fir_direct_effect_run;
		e->reset = fir_direct_effect_reset;
		e->plot = fir_direct_effect_plot;
		e->drain = fir_direct_effect_drain;
		e->destroy = fir_direct_effect_destroy;

		struct fir_direct_state *state = calloc(1, sizeof(struct fir_direct_state));
		e->data = state;

		state->filter_frames = filter_frames;
		state->len = 1;
		while (state->len < filter_frames)
			state->len <<= 1;
		state->mask = state->len - 1;
		LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd direct_len=%zd", ei->name, filter_frames, state->len);
		state->filter = calloc(e->ostream.channels, sizeof(sample_t *));
		state->buf = calloc(e->ostream.channels, sizeof(sample_t *));
		sample_t *lbuf_filter = calloc(state->len * filter_channels, sizeof(sample_t));
		sample_t *lbuf = calloc(state->len * n_channels, sizeof(sample_t));
		if (filter_channels == 1)
			memcpy(lbuf_filter, filter_data, filter_frames * sizeof(sample_t));
		for (i = k = 0; i < e->ostream.channels; ++i) {
			if (GET_BIT(channel_selector, i)) {
				state->filter[i] = lbuf_filter;
				state->buf[i] = lbuf;
				if (filter_channels > 1) {
					for (j = 0; j < filter_frames; ++j)
						state->filter[i][j] = filter_data[j*filter_channels + k];
					++k;
					lbuf_filter += state->len;
				}
				lbuf += state->len;
			}
		}
	}
	else {
		e->run = fir_effect_run;
		e->delay = fir_effect_delay;
		e->reset = fir_effect_reset;
		e->plot = fir_effect_plot;
		e->drain2 = fir_effect_drain2;
		e->destroy = fir_effect_destroy;

		fftw_plan filter_plan;
		sample_t *filter;
		struct fir_state *state = calloc(1, sizeof(struct fir_state));
		e->data = state;

		state->filter_frames = filter_frames;
		state->len = next_fast_fftw_len(filter_frames);
		LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd fft_len=%zd", ei->name, filter_frames, state->len);
		state->fr_len = state->len + 1;
		state->tmp_fr = fftw_malloc(state->fr_len * sizeof(fftw_complex));
		state->input = calloc(e->ostream.channels, sizeof(sample_t *));
		state->output = calloc(e->ostream.channels, sizeof(sample_t *));
		state->overlap = calloc(e->ostream.channels, sizeof(sample_t *));
		state->filter_fr = calloc(e->ostream.channels, sizeof(fftw_complex *));
		state->r2c_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		state->c2r_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		filter = fftw_malloc(state->len * 2 * sizeof(sample_t));
		memset(filter, 0, state->len * 2 * sizeof(sample_t));
		filter_plan = fftw_plan_dft_r2c_1d(state->len * 2, filter, state->tmp_fr, FFTW_ESTIMATE);
		if (filter_channels == 1) {
			memcpy(filter, filter_data, filter_frames * sizeof(sample_t));
			fftw_execute(filter_plan);
		}
		for (i = k = 0; i < e->ostream.channels; ++i) {
			state->output[i] = fftw_malloc(state->len * 2 * sizeof(sample_t));
			memset(state->output[i], 0, state->len * 2 * sizeof(sample_t));
			if (GET_BIT(channel_selector, i)) {
				state->input[i] = fftw_malloc(state->len * 2 * sizeof(sample_t));
				memset(state->input[i], 0, state->len * 2 * sizeof(sample_t));
				state->overlap[i] = fftw_malloc(state->len * sizeof(sample_t));
				memset(state->overlap[i], 0, state->len * sizeof(sample_t));
				state->filter_fr[i] = fftw_malloc(state->fr_len * sizeof(fftw_complex));
				state->r2c_plan[i] = fftw_plan_dft_r2c_1d(state->len * 2, state->input[i], state->tmp_fr, FFTW_ESTIMATE);
				state->c2r_plan[i] = fftw_plan_dft_c2r_1d(state->len * 2, state->tmp_fr, state->output[i], FFTW_ESTIMATE);
				if (filter_channels == 1)
					memcpy(state->filter_fr[i], state->tmp_fr, state->fr_len * sizeof(fftw_complex));
				else {
					for (j = 0; j < filter_frames; ++j)
						filter[j] = filter_data[j * filter_channels + k];
					fftw_execute(filter_plan);
					memcpy(state->filter_fr[i], state->tmp_fr, state->fr_len * sizeof(fftw_complex));
					++k;
				}
			}
		}
		fftw_destroy_plan(filter_plan);
		fftw_free(filter);
	}

	return e;
}

sample_t * fir_read_filter(const struct effect_info *ei, const char *dir, const char *path, int fs, int *channels, ssize_t *frames)
{
	static const char coefs_str_prefix[] = "coefs:";
	static const char file_str_prefix[] = "file:";
	sample_t *data = NULL;

	if (strncmp(path, coefs_str_prefix, LENGTH(coefs_str_prefix)-1) == 0) {
		char *endptr;
		path += LENGTH(coefs_str_prefix)-1;

		int filter_channels = 1;
		ssize_t i = 1, filter_frames = 1;
		for (const char *s = path; *s; ++s) {
			if (*s == ',') ++i;
			else if (*s == '/') {
				++filter_channels;
				if (i > filter_frames) filter_frames = i;
				i = 1;
			}
		}
		if (i > filter_frames) filter_frames = i;

		sample_t *ch_data = data = calloc(filter_frames * filter_channels, sizeof(sample_t));
		char *coefs_str = strdup(path);
		char *ch = coefs_str;
		while (*ch != '\0') {
			char *next_ch = isolate(ch, '/');
			char *coef = ch;
			for (i = 0; *coef != '\0'; ++i) {
				char *next_coef = isolate(coef, ',');
				if (*coef != '\0') {
					ch_data[filter_channels * i] = strtod(coef, &endptr);
					if (check_endptr(ei->name, coef, endptr, "coefficient")) {
						free(data);
						free(coefs_str);
						return NULL;
					}
				}
				coef = next_coef;
			}
			ch_data += 1;
			ch = next_ch;
		}
		free(coefs_str);
		*channels = filter_channels;
		*frames = filter_frames;
	}
	else {
		if (strncmp(path, file_str_prefix, LENGTH(file_str_prefix)-1) == 0)
			path += LENGTH(file_str_prefix)-1;
		char *fp = construct_full_path(dir, path);
		struct codec_params c_params = CODEC_PARAMS_AUTO(fp, CODEC_MODE_READ);
		struct codec *c = init_codec(&c_params);
		if (c == NULL) {
			LOG_FMT(LL_ERROR, "%s: error: failed to open filter file: %s", ei->name, fp);
			free(fp);
			return NULL;
		}
		free(fp);
		*channels = c->channels;
		*frames = c->frames;
		if (c->fs != fs) {
			LOG_FMT(LL_ERROR, "%s: error: sample rate mismatch: fs=%d filter_fs=%d", ei->name, fs, c->fs);
			destroy_codec(c);
			return NULL;
		}
		data = calloc(c->frames * c->channels, sizeof(sample_t));
		if (c->read(c, data, c->frames) != c->frames) {
			LOG_FMT(LL_ERROR, "%s: error: short read", ei->name);
			destroy_codec(c);
			free(data);
			return NULL;
		}
		destroy_codec(c);
	}
	return data;
}

struct effect * fir_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int filter_channels;
	ssize_t filter_frames;
	struct effect *e;
	sample_t *filter_data;

	if (argc != 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	filter_data = fir_read_filter(ei, dir, argv[1], istream->fs, &filter_channels, &filter_frames);
	if (filter_data == NULL)
		return NULL;
	e = fir_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, 0);
	free(filter_data);
	return e;
}
