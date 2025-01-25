/*
 * This file is part of dsp.
 *
 * Copyright (c) 2020-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <limits.h>
#include <complex.h>
#include <fftw3.h>
#include "fir_p.h"
#include "fir.h"
#include "util.h"

#define DIRECT_LEN           (1<<5)
#define MAX_PART_LEN_LIMIT   INT_MAX
#define MAX_PART_LEN_DEFAULT (1<<14)

struct direct_part {
	ssize_t p;
	sample_t **filter, **buf;
};

struct fft_part {
	ssize_t len, fr_len, mask, p, in_p, delay;
	fftw_complex **filter_fr;
	fftw_plan *r2c_plan, *c2r_plan;
	sample_t **ibuf, **obuf, **olap;
};

struct fir_p_state {
	ssize_t len, mask, p, nparts, filter_frames, drain_frames;
	fftw_complex *tmp_fr;
	sample_t **ibuf;
	struct direct_part part0;
	struct fft_part *part;
	int has_output, is_draining;
};

sample_t * fir_p_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;

	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->part0.buf[k]) {
				const sample_t s = ibuf[i*e->istream.channels + k];
				for (ssize_t n = state->part0.p, m = 0; m < DIRECT_LEN; ++m) {
					state->part0.buf[k][n] += s * state->part0.filter[k][m];
					n = (n+1) & (DIRECT_LEN-1);
				}
				ibuf[i*e->istream.channels + k] = state->part0.buf[k][state->part0.p];
				state->part0.buf[k][state->part0.p] = 0.0;
				for (ssize_t j = 0; j < state->nparts; ++j) {
					struct fft_part *part = &state->part[j];
					ibuf[i*e->istream.channels + k] += part->obuf[k][part->p];
					part->ibuf[k][part->p] = (part->delay > 0) ? state->ibuf[k][part->in_p] : s;
				}
				if (state->ibuf)
					state->ibuf[k][state->p] = s;
			}
		}
		state->part0.p = (state->part0.p+1) & (DIRECT_LEN-1);
		for (ssize_t j = 0; j < state->nparts; ++j) {
			struct fft_part *part = &state->part[j];
			part->in_p = (part->in_p+1 >= state->len) ? 0 : part->in_p+1;
			++part->p;
		}
		state->p = (state->p+1 >= state->len) ? 0 : state->p+1;

		/* All partition lengths must be some multiple of DIRECT_LEN */
		if (state->part0.p == 0) {
			for (ssize_t j = 0; j < state->nparts; ++j) {
				struct fft_part *part = &state->part[j];
				if (part->p == part->len) {
					for (int k = 0; k < e->istream.channels; ++k) {
						if (part->ibuf[k]) {
							fftw_execute(part->r2c_plan[k]);
							for (ssize_t l = 0; l < part->fr_len; ++l)
								state->tmp_fr[l] *= part->filter_fr[k][l];
							fftw_execute(part->c2r_plan[k]);
							for (ssize_t l = 0; l < part->len * 2; ++l)
								part->obuf[k][l] /= part->len * 2;
							for (ssize_t l = 0; l < part->len; ++l) {
								part->obuf[k][l] += part->olap[k][l];
								part->olap[k][l] = part->obuf[k][l + part->len];
							}
						}
					}
					part->p = 0;
				}
			}
		}
	}
	if (*frames > 0)
		state->has_output = 1;

	return ibuf;
}

void fir_p_effect_reset(struct effect *e)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	if (state->ibuf) {
		for (int k = 0; k < e->ostream.channels; ++k)
			if (state->ibuf[k])
				memset(state->ibuf[k], 0, state->len * sizeof(sample_t));
	}
	state->part0.p = 0;
	for (ssize_t j = 0; j < state->nparts; ++j) {
		struct fft_part *part = &state->part[j];
		part->p = 0;
		for (int k = 0; k < e->ostream.channels; ++k) {
			if (state->part0.buf[k]) {
				memset(part->obuf[k], 0, part->len * 2 * sizeof(sample_t));
				memset(part->olap[k], 0, part->len * sizeof(sample_t));
			}
		}
	}
}

void fir_p_effect_plot(struct effect *e, int i)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->part0.buf[k]) {
			printf("H%d_%d(w)=(abs(w)<=pi)?0.0", k, i);
			for (ssize_t m = 0; m < DIRECT_LEN; ++m)
				printf("+exp(-j*w*%zd)*%.15e", m, state->part0.filter[k][m]);
			ssize_t z = DIRECT_LEN;
			for (ssize_t j = 0; j < state->nparts; ++j) {
				struct fft_part *part = &state->part[j];
				for (ssize_t l = 0; l < part->fr_len; ++l)
					state->tmp_fr[l] = part->filter_fr[k][l];
				fftw_execute(part->c2r_plan[k]);
				for (ssize_t l = 0; l < part->len * 2; ++l)
					part->obuf[k][l] /= part->len * 2;
				for (ssize_t l = 0; l < part->len; ++l, ++z)
					printf("+exp(-j*w*%zd)*%.15e", z, part->obuf[k][l]);
			}
			puts(":0/0");
		}
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void fir_p_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	if (!state->has_output)
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
			fir_p_effect_run(e, frames, obuf, NULL);
		}
		else
			*frames = -1;
	}
}

void fir_p_effect_destroy(struct effect *e)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	for (ssize_t j = 0; j < state->nparts; ++j) {
		struct fft_part *part = &state->part[j];
		for (int k = 0; k < e->ostream.channels; ++k) {
			if (part->ibuf[k]) {
				fftw_free(part->ibuf[k]);
				fftw_free(part->obuf[k]);
				fftw_free(part->olap[k]);
				break;
			}
		}
		for (int k = 0; k < e->ostream.channels; ++k) {
			fftw_free(part->filter_fr[k]);
			fftw_destroy_plan(part->r2c_plan[k]);
			fftw_destroy_plan(part->c2r_plan[k]);
		}
		free(part->ibuf);
		free(part->obuf);
		free(part->olap);
		free(part->filter_fr);
		free(part->r2c_plan);
		free(part->c2r_plan);
	}
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->part0.buf[k]) {
			free(state->part0.filter[k]);
			free(state->part0.buf[k]);
			if (state->ibuf)
				free(state->ibuf[k]);
			break;
		}
	}
	free(state->part0.filter);
	free(state->part0.buf);
	free(state->ibuf);
	fftw_free(state->tmp_fr);
	free(state->part);
	free(state);
}

struct effect * fir_p_effect_init_with_filter(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, sample_t *filter_data, int filter_channels, ssize_t filter_frames, ssize_t max_part_len)
{
	ssize_t i, k, j, l;
	ssize_t filter_pos = DIRECT_LEN, delay = 0, max_delay = 0;
	struct effect *e;
	struct fir_p_state *state;
	sample_t *tmp_buf = NULL;

	if (filter_frames < DIRECT_LEN)
		return fir_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, 1);

	const int n_channels = num_bits_set(channel_selector, istream->channels);
	if (filter_channels != 1 && filter_channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channel mismatch: channels=%d filter_channels=%d", ei->name, n_channels, filter_channels);
		return NULL;
	}
	if (filter_frames < 1) {
		LOG_FMT(LL_ERROR, "%s: error: filter length must be >= 1", ei->name);
		return NULL;
	}
	max_part_len = (max_part_len == 0) ? MAX_PART_LEN_DEFAULT : max_part_len;
	if (!IS_POWER_OF_2(max_part_len)) {
		LOG_FMT(LL_ERROR, "%s: error: max_part_len must be a power of two", ei->name);
		return NULL;
	}
	if (max_part_len < DIRECT_LEN || max_part_len > MAX_PART_LEN_LIMIT) {
		LOG_FMT(LL_ERROR, "%s: error: max_part_len must be within [%d,%d] or 0 for default", ei->name, DIRECT_LEN, MAX_PART_LEN_LIMIT);
		return NULL;
	}

	LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd", ei->name, filter_frames);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->run = fir_p_effect_run;
	e->reset = fir_p_effect_reset;
	e->plot = fir_p_effect_plot;
	e->drain = fir_p_effect_drain;
	e->destroy = fir_p_effect_destroy;

	state = calloc(1, sizeof(struct fir_p_state));
	e->data = state;

	state->filter_frames = filter_frames;
	state->part0.filter = calloc(e->ostream.channels, sizeof(sample_t *));
	state->part0.buf = calloc(e->ostream.channels, sizeof(sample_t *));
	LOG_FMT(LL_VERBOSE, "%s: info: partition 0: len=%d delay=0 total=%d (direct)", ei->name, DIRECT_LEN, DIRECT_LEN);

	k = j = DIRECT_LEN;
	for (i = 0; j < filter_frames; ++i) {
		++state->nparts;
		state->part = realloc(state->part, sizeof(struct fft_part) * state->nparts);
		struct fft_part *part = &state->part[i];
		memset(part, 0, sizeof(struct fft_part));
		part->len = k;
		part->ibuf = calloc(e->ostream.channels, sizeof(sample_t *));
		part->obuf = calloc(e->ostream.channels, sizeof(sample_t *));
		part->olap = calloc(e->ostream.channels, sizeof(sample_t *));
		part->fr_len = part->len + 1;
		part->filter_fr = calloc(e->ostream.channels, sizeof(fftw_complex *));
		part->r2c_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		part->c2r_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		part->delay = delay;
		max_delay = MAXIMUM(delay, max_delay);
		j += part->len;
		if (k < max_part_len && j + k < filter_frames)
			k *= 2;
		else
			delay += state->part[i].len;
		LOG_FMT(LL_VERBOSE, "%s: info: partition %zd: len=%zd delay=%zd total=%zd", ei->name, i+1, state->part[i].len, state->part[i].delay, j);
	}
	if (max_delay > 0) {
		state->len = max_delay;
		state->ibuf = calloc(e->ostream.channels, sizeof(sample_t *));
		sample_t *lbuf = calloc(state->len * n_channels, sizeof(sample_t));
		for (i = 0; i < e->ostream.channels; ++i) {
			if (GET_BIT(channel_selector, i)) {
				state->ibuf[i] = lbuf;
				lbuf += state->len;
			}
		}
	}

	sample_t *lbuf_filter = calloc(DIRECT_LEN * filter_channels, sizeof(sample_t));
	sample_t *lbuf = calloc(DIRECT_LEN * n_channels, sizeof(sample_t));
	if (filter_channels == 1)
		memcpy(lbuf_filter, filter_data, DIRECT_LEN * sizeof(sample_t));
	for (i = k = 0; i < e->ostream.channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			state->part0.filter[i] = lbuf_filter;
			state->part0.buf[i] = lbuf;
			if (filter_channels > 1) {
				for (j = 0; j < DIRECT_LEN; ++j)
					state->part0.filter[i][j] = filter_data[j*filter_channels + k];
				++k;
				lbuf_filter += DIRECT_LEN;
			}
			lbuf += DIRECT_LEN;
		}
	}

	if (state->nparts > 0) {
		struct fft_part *part = &state->part[state->nparts - 1];
		tmp_buf = fftw_malloc(part->len * 2 * sizeof(sample_t));
		state->tmp_fr = fftw_malloc(part->fr_len * sizeof(fftw_complex));
	}
	for (k = 0; k < state->nparts; ++k) {
		struct fft_part *part = &state->part[k];
		memset(tmp_buf, 0, part->len * 2 * sizeof(sample_t));
		fftw_plan tmp_plan = fftw_plan_dft_r2c_1d(part->len * 2, tmp_buf, state->tmp_fr, FFTW_ESTIMATE);
		if (filter_channels == 1) {
			memcpy(tmp_buf, &filter_data[filter_pos], MINIMUM(filter_frames-filter_pos, part->len) * sizeof(sample_t));
			fftw_execute(tmp_plan);
		}
		sample_t *lbuf_in = fftw_malloc(part->len * 2 * n_channels * sizeof(sample_t));
		memset(lbuf_in, 0, part->len * 2 * n_channels * sizeof(sample_t));
		sample_t *lbuf_out = fftw_malloc(part->len * 2 * n_channels * sizeof(sample_t));
		memset(lbuf_out, 0, part->len * 2 * n_channels * sizeof(sample_t));
		sample_t *lbuf_olap = fftw_malloc(part->len * n_channels * sizeof(sample_t));
		memset(lbuf_olap, 0, part->len * n_channels * sizeof(sample_t));
		for (i = l = 0; i < e->ostream.channels; ++i) {
			if (GET_BIT(channel_selector, i)) {
				part->ibuf[i] = lbuf_in;
				part->obuf[i] = lbuf_out;
				part->olap[i] = lbuf_olap;
				part->filter_fr[i] = fftw_malloc(part->fr_len * sizeof(fftw_complex));
				part->r2c_plan[i] = fftw_plan_dft_r2c_1d(part->len * 2, part->ibuf[i], state->tmp_fr, FFTW_ESTIMATE);
				part->c2r_plan[i] = fftw_plan_dft_c2r_1d(part->len * 2, state->tmp_fr, part->obuf[i], FFTW_ESTIMATE);
				if (filter_channels > 1) {
					for (j = 0; j < part->len && j + filter_pos < filter_frames; ++j)
						tmp_buf[j] = filter_data[(j + filter_pos) * filter_channels + l];
					fftw_execute(tmp_plan);
					memcpy(part->filter_fr[i], state->tmp_fr, part->fr_len * sizeof(fftw_complex));
					++l;
				}
				else {
					memcpy(part->filter_fr[i], state->tmp_fr, part->fr_len * sizeof(fftw_complex));
				}
				lbuf_in += part->len * 2;
				lbuf_out += part->len * 2;
				lbuf_olap += part->len;
			}
		}
		fftw_destroy_plan(tmp_plan);
		filter_pos += part->len;
		part->in_p = (part->delay == 0) ? 0 : state->len - part->delay;
	}
	fftw_free(tmp_buf);

	return e;
}

struct effect * fir_p_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int filter_channels;
	ssize_t filter_frames, max_part_len = 0;
	struct effect *e;
	sample_t *filter_data;
	char *endptr;

	if (argc > 3 || argc < 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (argc > 2) {
		max_part_len = strtol(argv[1], &endptr, 10);
		CHECK_ENDPTR(argv[1], endptr, "max_part_len", return NULL);
	}
	filter_data = fir_read_filter(ei, dir, argv[argc - 1], istream->fs, &filter_channels, &filter_frames);
	if (filter_data == NULL)
		return NULL;
	e = fir_p_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, max_part_len);
	free(filter_data);
	return e;
}
