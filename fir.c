/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
	sample_t *lbuf, **filter, **buf;
	int has_output, is_draining;
};

struct fir_state {
	ssize_t len, fr_len, p, filter_frames;
	ssize_t drain_pos, drain_frames;
	fftw_complex **filter_fr, *tmp_fr, *filter_fr_1ch;
	sample_t **ibuf, **obuf, **olap;
	fftw_plan r2c_plan, c2r_plan;
	int has_output, is_draining;
};

sample_t * fir_direct_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->istream.channels; ++k) {
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
	struct fir_direct_state *state = (struct fir_direct_state *) e->data;
	state->p = 0;
	for (int k = 0; k < e->ostream.channels; ++k)
		if (state->buf[k]) memset(state->buf[k], 0, state->len * sizeof(sample_t));
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
	free(state->lbuf);
	free(state->filter);
	free(state->buf);
	free(state);
}

sample_t * fir_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	ssize_t iframes = 0, oframes = 0;

	while (iframes < *frames) {
		while (state->p < state->len && iframes < *frames) {
			for (int k = 0; k < e->ostream.channels; ++k) {
				/* Note: If channel k is not selected, state->ibuf[k] and state->obuf[k] are aliases. */
				if (state->has_output)
					obuf[oframes*e->ostream.channels + k] = state->obuf[k][state->p];
				#ifdef SYMMETRIC_IO
					else obuf[oframes*e->ostream.channels + k] = 0.0;
				#endif
				state->ibuf[k][state->p] = ibuf[iframes*e->ostream.channels + k];
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
			const sample_t out_norm = 1.0 / (state->len * 2.0);
			for (int k = 0; k < e->ostream.channels; ++k) {
				if (state->olap[k]) {
					fftw_complex *filter_fr_p = state->filter_fr[k];
					sample_t *obuf_p = state->obuf[k], *olap_p = state->olap[k];
					fftw_execute_dft_r2c(state->r2c_plan, state->ibuf[k], state->tmp_fr);
					for (ssize_t j = 0; j < state->fr_len; j += 2) {
						state->tmp_fr[j+0] *= filter_fr_p[j+0];
						state->tmp_fr[j+1] *= filter_fr_p[j+1];
					}
					fftw_execute_dft_c2r(state->c2r_plan, state->tmp_fr, obuf_p);
					for (ssize_t j = 0; j < state->len * 2; j += 2) {
						obuf_p[j+0] *= out_norm;
						obuf_p[j+1] *= out_norm;
					}
					sample_t *obuf_olap_p = &obuf_p[state->len];
					for (ssize_t j = 0; j < state->len; ++j) {
						obuf_p[j] += olap_p[j];
						olap_p[j] = obuf_olap_p[j];
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
	struct fir_state *state = (struct fir_state *) e->data;
	state->p = 0;
	state->has_output = 0;
	for (int k = 0; k < e->ostream.channels; ++k) {
		memset(state->obuf[k], 0, state->len * sizeof(sample_t));
		if (state->olap[k]) memset(state->olap[k], 0, state->len * sizeof(sample_t));
	}
}

void fir_effect_plot(struct effect *e, int i)
{
	struct fir_state *state = (struct fir_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->olap[k]) {
			for (ssize_t j = 0; j < state->fr_len; ++j)
				state->tmp_fr[j] = state->filter_fr[k][j];
			fftw_execute_dft_c2r(state->c2r_plan, state->tmp_fr, state->obuf[k]);
			printf("H%d_%d(w)=(abs(w)<=pi)?0.0", k, i);
			for (ssize_t j = 0; j < state->len; ++j)
				printf("+exp(-j*w*%zd)*%.15e", j, state->obuf[k][j] / (state->len * 2));
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
	struct fir_state *state = (struct fir_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->ibuf[k] != state->obuf[k])
			fftw_free(state->ibuf[k]);
		fftw_free(state->obuf[k]);
		fftw_free(state->olap[k]);
		if (state->filter_fr_1ch == NULL)
			fftw_free(state->filter_fr[k]);
	}
	free(state->ibuf);
	free(state->obuf);
	free(state->olap);
	free(state->filter_fr);
	fftw_free(state->filter_fr_1ch);
	fftw_free(state->tmp_fr);
	fftw_destroy_plan(state->r2c_plan);
	fftw_destroy_plan(state->c2r_plan);
	free(state);
}

struct effect * fir_effect_init_with_filter(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, sample_t *filter_data, int filter_channels, ssize_t filter_frames, int force_direct)
{
	struct effect *e;

	const int n_channels = num_bits_set(channel_selector, istream->channels);
	if (filter_channels != 1 && filter_channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channels mismatch: channels=%d filter_channels=%d", ei->name, n_channels, filter_channels);
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
		sample_t *l_filter_p = state->lbuf = calloc(state->len * (filter_channels + n_channels), sizeof(sample_t));
		sample_t *l_buf_p = l_filter_p + (state->len * filter_channels);
		state->filter = calloc(e->ostream.channels, sizeof(sample_t *));
		state->buf = calloc(e->ostream.channels, sizeof(sample_t *));
		if (filter_channels == 1)
			memcpy(l_filter_p, filter_data, filter_frames * sizeof(sample_t));
		for (int i = 0, k = 0; i < e->ostream.channels; ++i) {
			if (GET_BIT(channel_selector, i)) {
				state->filter[i] = l_filter_p;
				state->buf[i] = l_buf_p;
				if (filter_channels > 1) {
					for (ssize_t j = 0; j < filter_frames; ++j)
						state->filter[i][j] = filter_data[j*filter_channels + k];
					++k;
					l_filter_p += state->len;
				}
				l_buf_p += state->len;
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

		struct fir_state *state = calloc(1, sizeof(struct fir_state));
		e->data = state;

		state->filter_frames = filter_frames;
		state->len = next_fast_fftw_len(filter_frames);
		LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd fft_len=%zd", ei->name, filter_frames, state->len);
		state->fr_len = state->len + ((state->len&1)?1:2);
		state->tmp_fr = fftw_malloc(state->fr_len * sizeof(fftw_complex));
		state->ibuf = calloc(e->ostream.channels, sizeof(sample_t *));
		state->obuf = calloc(e->ostream.channels, sizeof(sample_t *));
		state->olap = calloc(e->ostream.channels, sizeof(sample_t *));
		state->filter_fr = calloc(e->ostream.channels, sizeof(fftw_complex *));

		if (filter_channels == 1)
			state->filter_fr_1ch = fftw_malloc(state->fr_len * sizeof(fftw_complex));
		for (int k = 0; k < e->ostream.channels; ++k) {
			state->obuf[k] = fftw_malloc(state->len * 2 * sizeof(sample_t));
			if (GET_BIT(channel_selector, k)) {
				state->ibuf[k] = fftw_malloc(state->len * 2 * sizeof(sample_t));
				state->olap[k] = fftw_malloc(state->len * sizeof(sample_t));
				state->filter_fr[k] = (filter_channels == 1) ?
					state->filter_fr_1ch : fftw_malloc(state->fr_len * sizeof(fftw_complex));
			}
			else state->ibuf[k] = state->obuf[k];
		}

		dsp_fftw_acquire();
		const int planner_flags = (dsp_fftw_load_wisdom()) ? FFTW_MEASURE : FFTW_ESTIMATE;
		state->r2c_plan = fftw_plan_dft_r2c_1d(state->len * 2, state->obuf[0], state->tmp_fr, planner_flags);
		state->c2r_plan = fftw_plan_dft_c2r_1d(state->len * 2, state->tmp_fr, state->obuf[0], planner_flags);
		dsp_fftw_release();
		for (int k = 0; k < e->ostream.channels; ++k) {
			memset(state->obuf[k], 0, state->len * 2 * sizeof(sample_t));
			if (GET_BIT(channel_selector, k)) {
				memset(state->ibuf[k], 0, state->len * 2 * sizeof(sample_t));
				memset(state->olap[k], 0, state->len * sizeof(sample_t));
			}
		}
		if (filter_channels == 1) {
			memcpy(state->obuf[0], filter_data, filter_frames * sizeof(sample_t));
			fftw_execute(state->r2c_plan);
			memcpy(state->filter_fr_1ch, state->tmp_fr, state->fr_len * sizeof(fftw_complex));
		}
		else {
			for (int k = 0, l = 0; k < e->ostream.channels; ++k) {
				if (GET_BIT(channel_selector, k)) {
					for (ssize_t j = 0; j < filter_frames; ++j)
						state->obuf[0][j] = filter_data[j*filter_channels + l];
					fftw_execute(state->r2c_plan);
					memcpy(state->filter_fr[k], state->tmp_fr, state->fr_len * sizeof(fftw_complex));
					++l;
				}
			}
		}
		memset(state->obuf[0], 0, state->len * 2 * sizeof(sample_t));
	}

	return e;
}

struct effect * fir_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int filter_channels;
	ssize_t filter_frames;
	struct effect *e;
	sample_t *filter_data;
	struct fir_config config;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;

	int err = fir_parse_opts(ei, istream, &config, &g, argc, argv, NULL, NULL, NULL);
	if (err || g.ind != argc-1) {
		print_effect_usage(ei);
		return NULL;
	}
	config.p.path = argv[g.ind];
	filter_data = fir_read_filter(ei, istream, dir, &config.p, &filter_channels, &filter_frames);
	if (filter_data == NULL)
		return NULL;
	e = fir_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, 0);
	e->next = fir_init_align(ei, istream, channel_selector, &config, filter_data, filter_channels, filter_frames);
	free(filter_data);
	return e;
}
