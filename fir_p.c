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
#include <errno.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <complex.h>
#include <fftw3.h>
#include <pthread.h>
#include <semaphore.h>
#include "fir_p.h"
#include "fir.h"
#include "fir_util.h"
#include "util.h"
#include "codec.h"

#define DIRECT_LEN           (1<<5)  /* must be >= 1<<4 */
#define FFT_LEN_STEP_DEFAULT (1<<2)
#define MAX_FFT_GROUPS       4
#define MAX_PART_LEN_LIMIT   INT_MAX
#define MAX_PART_LEN_DEFAULT (1<<14)
#define FORCE_SINGLE_THREAD  0  /* for testing */

struct direct_part {
	sample_t *lbuf, **filter, **buf;
	int p;
};

struct fft_part_group {
	fftw_complex **filter_fr, **fdl, *tmp_fr, *filter_fr_1ch;
	fftw_plan r2c_plan, c2r_plan;
	sample_t **fft_ibuf, **fft_obuf, **fft_olap;
	sample_t **ibuf, **obuf;
	int n, len, fr_len, p, fdl_p, delay;
	int fft_channels, has_thread;
	pthread_t thread;
	sem_t start, sync;
};

struct fir_p_state {
	struct direct_part part0;
	struct fft_part_group group[MAX_FFT_GROUPS];
	ssize_t filter_frames, drain_frames;
	int n, has_output, is_draining;
};

static inline void fft_part_group_compute(struct fft_part_group *group)
{
	const sample_t out_norm = 1.0 / (group->len * 2.0);
	for (int k = 0; k < group->fft_channels; ++k) {
		fftw_complex *fdl_p = group->fdl[k] + group->fr_len*group->fdl_p;
		fftw_complex *filter_fr_p = group->filter_fr[k];
		sample_t *fft_obuf_p = group->fft_obuf[k], *fft_olap_p = group->fft_olap[k];

		fftw_execute_dft_r2c(group->r2c_plan, group->fft_ibuf[k], group->tmp_fr);
		memcpy(fdl_p, group->tmp_fr, group->fr_len * sizeof(fftw_complex));
		for (int l = 0; l < group->fr_len; l += 2) {
			group->tmp_fr[l+0] *= filter_fr_p[l+0];
			group->tmp_fr[l+1] *= filter_fr_p[l+1];
		}
		for (int q = 1; q < group->n; ++q) {
			filter_fr_p += group->fr_len;
			if (fdl_p == group->fdl[k]) fdl_p += group->fr_len*(group->n-1);
			else fdl_p -= group->fr_len;
			for (int l = 0; l < group->fr_len; l += 2) {
				group->tmp_fr[l+0] += fdl_p[l+0] * filter_fr_p[l+0];
				group->tmp_fr[l+1] += fdl_p[l+1] * filter_fr_p[l+1];
			}
		}
		fftw_execute_dft_c2r(group->c2r_plan, group->tmp_fr, fft_obuf_p);
		for (int l = 0; l < group->len * 2; l += 2) {
			fft_obuf_p[l+0] *= out_norm;
			fft_obuf_p[l+1] *= out_norm;
		}
		sample_t *fft_obuf_olap_p = fft_obuf_p + group->len;
		for (int l = 0; l < group->len; l += 2) {
			fft_obuf_p[l+0] += fft_olap_p[l+0];
			fft_obuf_p[l+1] += fft_olap_p[l+1];
			fft_olap_p[l+0] = fft_obuf_olap_p[l+0];
			fft_olap_p[l+1] = fft_obuf_olap_p[l+1];
		}
	}
	group->fdl_p = (group->fdl_p + 1 < group->n) ? group->fdl_p + 1 : 0;
}

static void * fft_part_group_worker(void *arg)
{
	struct fft_part_group *group = (struct fft_part_group *) arg;
	for (;;) {
		sem_post(&group->sync);
		while (sem_wait(&group->start) != 0);
		fft_part_group_compute(group);
	}
	return NULL;
}

static inline void fft_part_group_transfer_bufs(struct effect *e, struct fft_part_group *group)
{
	for (int k = 0, n = 0; k < e->istream.channels; ++k) {
		if (group->obuf[k]) {
			memcpy(group->obuf[k], group->fft_obuf[n], group->len * sizeof(sample_t));
			memcpy(group->fft_ibuf[n], group->ibuf[k], group->len * sizeof(sample_t));
			++n;
		}
	}
}

sample_t * fir_p_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;

	for (ssize_t i = 0; i < *frames; ++i) {
		for (int k = 0; k < e->istream.channels; ++k) {
			if (state->part0.buf[k]) {
				const sample_t s = ibuf[i*e->istream.channels + k];
				sample_t *p0_buf = state->part0.buf[k], *p0_filter = state->part0.filter[k];
				for (int n = state->part0.p, m = 0; m < DIRECT_LEN;) {
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;

					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
					p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1; p0_buf[n++] += s * p0_filter[m++]; n &= DIRECT_LEN-1;
				}
				ibuf[i*e->istream.channels + k] = state->part0.buf[k][state->part0.p];
				state->part0.buf[k][state->part0.p] = 0.0;
				for (int j = 0; j < state->n; ++j) {
					struct fft_part_group *group = &state->group[j];
					ibuf[i*e->istream.channels + k] += group->obuf[k][group->p + state->part0.p];
					group->ibuf[k][group->p + state->part0.p] = s;
				}
			}
		}
		state->part0.p = (state->part0.p+1) & (DIRECT_LEN-1);

		/* All partition lengths must be some multiple of DIRECT_LEN */
		if (state->part0.p == 0) {
			for (int j = 0; j < state->n; ++j) {
				struct fft_part_group *group = &state->group[j];
				group->p += DIRECT_LEN;
				if (group->p == group->len) {
					group->p = 0;
					if (group->has_thread) {
						while (sem_wait(&group->sync) != 0);
						fft_part_group_transfer_bufs(e, group);
						sem_post(&group->start);
					}
					else {
						if (group->delay > 0) /* should not happen */
							fft_part_group_transfer_bufs(e, group);
						fft_part_group_compute(group);
					}
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
	state->has_output = 0;
	state->part0.p = 0;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->part0.buf[k]) memset(state->part0.buf[k], 0, DIRECT_LEN * sizeof(sample_t));
	for (int j = 0; j < state->n; ++j) {
		struct fft_part_group *group = &state->group[j];
		if (group->has_thread) {
			while (sem_wait(&group->sync) != 0);
			sem_post(&group->sync);
		}
		group->p = 0;
		group->fdl_p = 0;
		for (int k = 0; k < group->fft_channels; ++k) {
			memset(group->fdl[k], 0, group->fr_len * group->n * sizeof(fftw_complex));
			memset(group->fft_obuf[k], 0, group->len * 2 * sizeof(sample_t));
			memset(group->fft_olap[k], 0, group->len * sizeof(sample_t));
		}
		if (group->delay > 0) {
			for (int k = 0; k < e->istream.channels; ++k)
				if (group->obuf[k]) memset(group->obuf[k], 0, group->len * sizeof(sample_t));
		}
	}
}

void fir_p_effect_plot(struct effect *e, int i)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	for (int k = 0, n = 0; k < e->istream.channels; ++k) {
		if (state->part0.buf[k]) {
			printf("H%d_%d(w)=(abs(w)<=pi)?0.0", k, i);
			for (int m = 0; m < DIRECT_LEN; ++m)
				printf("+exp(-j*w*%d)*%.15e", m, state->part0.filter[k][m]);
			ssize_t z = DIRECT_LEN;
			for (int j = 0; j < state->n; ++j) {
				struct fft_part_group *group = &state->group[j];
				for (int q = 0; q < group->n; ++q) {
					memcpy(group->tmp_fr, &group->filter_fr[n][q*group->fr_len], group->fr_len * sizeof(fftw_complex));
					fftw_execute(group->c2r_plan);  /* output is fft_obuf[0] */
					for (int l = 0; l < group->len; ++l, ++z)
						printf("+exp(-j*w*%zd)*%.15e", z, group->fft_obuf[0][l] / (group->len * 2));
				}
			}
			puts(":0/0");
			++n;
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
	for (int j = 0; j < state->n; ++j) {
		struct fft_part_group *group = &state->group[j];
		if (group->has_thread) {
			pthread_cancel(group->thread);
			pthread_join(group->thread, NULL);
			sem_destroy(&group->start);
			sem_destroy(&group->sync);
		}
		for (int i = 0; i < group->fft_channels; ++i) {
			if (group->filter_fr_1ch == NULL)
				fftw_free(group->filter_fr[i]);
			fftw_free(group->fdl[i]);
			fftw_free(group->fft_ibuf[i]);
			fftw_free(group->fft_obuf[i]);
			fftw_free(group->fft_olap[i]);
		}
		fftw_free(group->tmp_fr);
		fftw_free(group->filter_fr_1ch);
		free(group->filter_fr);
		free(group->fdl);
		free(group->fft_ibuf);
		free(group->fft_obuf);
		free(group->fft_olap);
		if (group->delay > 0) {
			for (int i = 0; i < e->istream.channels; ++i) {
				fftw_free(group->ibuf[i]);
				fftw_free(group->obuf[i]);
			}
		}
		free(group->ibuf);
		free(group->obuf);
		fftw_destroy_plan(group->r2c_plan);
		fftw_destroy_plan(group->c2r_plan);
	}
	free(state->part0.lbuf);
	free(state->part0.filter);
	free(state->part0.buf);
	free(state);
}

static void find_partitions(struct fir_p_state *state, int max_part_len, int single_thread)
{
	const int delay_fact = (single_thread) ? 1 : 2;
	int fft_len_step = FFT_LEN_STEP_DEFAULT;
	next_len_step:
	for (ssize_t j = DIRECT_LEN, k = DIRECT_LEN; k < state->filter_frames; ) {
		++state->n;
		if (state->n > MAX_FFT_GROUPS) {
			memset(state->group, 0, MAX_FFT_GROUPS * sizeof(struct fft_part_group));
			state->n = 0;
			fft_len_step <<= 1;
			goto next_len_step;
		}
		struct fft_part_group *group = &state->group[state->n - 1];
		group->len = j;
		group->fr_len = group->len + 2;
		group->n = 1;
		k += group->len;
		while (k < state->filter_frames && k < j * fft_len_step * delay_fact) {
			++group->n;
			k += group->len;
		}
		j *= fft_len_step;
		if (j > max_part_len || k + j * fft_len_step > state->filter_frames) {
			while (k < state->filter_frames) {
				++group->n;
				k += group->len;
			}
			break;
		}
	}
	/* try to optimize a bit */
	for (int k = state->n - 1; k > 0; --k) {
		struct fft_part_group *group = &state->group[k];
		struct fft_part_group *prev_group = &state->group[k-1];
		while (group->len * 2 <= max_part_len) {
			const int new_n = prev_group->n + group->len * delay_fact / prev_group->len;
			if (group->n <= new_n) break;
			prev_group->n = new_n;
			group->len *= 2;
			group->fr_len = group->len + 2;
			group->n -= delay_fact;
			group->n = group->n / 2 + (group->n & 1);
		}
	}
}

static int verify_and_print_partitions(const struct effect_info *ei, struct fir_p_state *state, int single_thread)
{
	LOG_FMT(LL_VERBOSE, "%s: info: partition group 0: n=1 len=%d total=%d (direct)", ei->name, DIRECT_LEN, DIRECT_LEN);
	ssize_t total_len = DIRECT_LEN, last_total_len = DIRECT_LEN;
	for (int k = 0; k < state->n; ++k) {
		struct fft_part_group *group = &state->group[k];
		const int delay = last_total_len - group->len;
		if (((single_thread || k == 0) && delay != 0) || (!single_thread && k > 0 && delay != group->len)) {
			LOG_FMT(LL_ERROR, "%s: BUG: invalid partitioning: group=%d len=%d delay=%d", ei->name, k+1, group->len, delay);
			return 1;
		}
		group->delay = delay;
		total_len += group->len * group->n;
		last_total_len = total_len;
		LOG_FMT(LL_VERBOSE, "%s: info: partition group %d: n=%d len=%d total=%zd", ei->name, k+1, group->n, group->len, total_len);
	}
	if (total_len < state->filter_frames) {
		LOG_FMT(LL_ERROR, "%s: BUG: invalid partitioning: total=%zd filter_frames=%zd", ei->name, total_len, state->filter_frames);
		return 1;
	}
	if (total_len - state->group[state->n - 1].len >= state->filter_frames)
		LOG_FMT(LL_ERROR, "%s: BUG: warning: extra partitions: total=%zd filter_frames=%zd", ei->name, total_len, state->filter_frames);
	return 0;
}

struct effect * fir_p_effect_init_with_filter(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, sample_t *filter_data, int filter_channels, ssize_t filter_frames, int max_part_len)
{
	struct effect *e;
	struct fir_p_state *state;

	if (filter_frames <= DIRECT_LEN)
		return fir_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, 1);

	const int n_channels = num_bits_set(channel_selector, istream->channels);
	if (filter_channels != 1 && filter_channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channels mismatch: channels=%d filter_channels=%d", ei->name, n_channels, filter_channels);
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
	const int use_single_thread = (filter_frames < 4096 || FORCE_SINGLE_THREAD);
	find_partitions(state, max_part_len, use_single_thread);
	if (verify_and_print_partitions(ei, state, use_single_thread)) goto fail;

	sample_t *l_filter_p = state->part0.lbuf = calloc(DIRECT_LEN * (filter_channels + n_channels), sizeof(sample_t));
	sample_t *l_buf_p = l_filter_p + (DIRECT_LEN * filter_channels);
	state->part0.filter = calloc(e->istream.channels, sizeof(sample_t *));
	state->part0.buf = calloc(e->istream.channels, sizeof(sample_t *));
	if (filter_channels == 1)
		memcpy(l_filter_p, filter_data, DIRECT_LEN * sizeof(sample_t));
	for (int i = 0, k = 0; i < e->istream.channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			state->part0.filter[i] = l_filter_p;
			state->part0.buf[i] = l_buf_p;
			if (filter_channels > 1) {
				for (int j = 0; j < DIRECT_LEN; ++j)
					state->part0.filter[i][j] = filter_data[j*filter_channels + k];
				++k;
				l_filter_p += DIRECT_LEN;
			}
			l_buf_p += DIRECT_LEN;
		}
	}

	dsp_fftw_acquire();
	const int planner_flags = (dsp_fftw_load_wisdom()) ? FFTW_MEASURE : FFTW_ESTIMATE;
	dsp_fftw_release();
	ssize_t filter_pos = DIRECT_LEN;
	for (int k = 0; k < state->n; ++k) {
		struct fft_part_group *group = &state->group[k];
		group->fft_channels = n_channels;
		group->filter_fr = calloc(n_channels, sizeof(fftw_complex *));
		group->fdl = calloc(n_channels, sizeof(fftw_complex *));
		group->fft_ibuf = calloc(n_channels, sizeof(sample_t *));
		group->fft_obuf = calloc(n_channels, sizeof(sample_t *));
		group->fft_olap = calloc(n_channels, sizeof(sample_t *));
		group->ibuf = calloc(e->istream.channels, sizeof(sample_t *));
		group->obuf = calloc(e->istream.channels, sizeof(sample_t *));
		group->tmp_fr = fftw_malloc(group->fr_len * sizeof(fftw_complex));
		if (filter_channels == 1)
			group->filter_fr_1ch = fftw_malloc(group->fr_len * group->n * sizeof(fftw_complex));
		for (int i = 0; i < n_channels; ++i) {
			group->filter_fr[i] = (filter_channels == 1) ?
				group->filter_fr_1ch : fftw_malloc(group->fr_len * group->n * sizeof(fftw_complex));
			group->fdl[i] = fftw_malloc(group->fr_len * group->n * sizeof(fftw_complex));
			group->fft_ibuf[i] = fftw_malloc(group->len * 2 * sizeof(sample_t));
			group->fft_obuf[i] = fftw_malloc(group->len * 2 * sizeof(sample_t));
			group->fft_olap[i] = fftw_malloc(group->len * sizeof(sample_t));
		}

		dsp_fftw_acquire();
		group->r2c_plan = fftw_plan_dft_r2c_1d(group->len * 2, group->fft_ibuf[0], group->tmp_fr, planner_flags);
		group->c2r_plan = fftw_plan_dft_c2r_1d(group->len * 2, group->tmp_fr, group->fft_obuf[0], planner_flags);
		dsp_fftw_release();
		for (int i = 0; i < n_channels; ++i) {
			memset(group->fdl[i], 0, group->fr_len * group->n * sizeof(fftw_complex));
			memset(group->fft_ibuf[i], 0, group->len * 2 * sizeof(sample_t));
			memset(group->fft_obuf[i], 0, group->len * 2 * sizeof(sample_t));
			memset(group->fft_olap[i], 0, group->len * sizeof(sample_t));
		}

		for (int q = 0; q < group->n; ++q) {
			if (filter_channels == 1) {
				memcpy(group->fft_ibuf[0], &filter_data[filter_pos], MINIMUM(filter_frames-filter_pos, group->len) * sizeof(sample_t));
				fftw_execute(group->r2c_plan);
				memcpy(&group->filter_fr_1ch[q*group->fr_len], group->tmp_fr, group->fr_len * sizeof(fftw_complex));
			}
			else {
				for (int i = 0; i < n_channels; ++i) {
					for (int l = 0; l < group->len && l + filter_pos < filter_frames; ++l)
						group->fft_ibuf[0][l] = filter_data[(filter_pos+l)*filter_channels + i];
					fftw_execute(group->r2c_plan);
					memcpy(&group->filter_fr[i][q*group->fr_len], group->tmp_fr, group->fr_len * sizeof(fftw_complex));
				}
			}
			filter_pos += group->len;
			memset(group->fft_ibuf[0], 0, group->len * 2 * sizeof(sample_t));
		}
		if (group->delay > 0) {
			for (int i = 0; i < e->istream.channels; ++i) {
				if (GET_BIT(channel_selector, i)) {
					group->ibuf[i] = fftw_malloc(group->len * sizeof(sample_t));
					group->obuf[i] = fftw_malloc(group->len * sizeof(sample_t));
					memset(group->ibuf[i], 0, group->len * sizeof(sample_t));
					memset(group->obuf[i], 0, group->len * sizeof(sample_t));
				}
			}
			sem_init(&group->start, 0, 0);
			sem_init(&group->sync, 0, 0);
			if ((errno = pthread_create(&group->thread, NULL, fft_part_group_worker, group)) != 0) {
				LOG_FMT(LL_ERROR, "%s(): error: pthread_create() failed: %s", __func__, strerror(errno));
				sem_destroy(&group->start);
				sem_destroy(&group->sync);
				goto fail;
			}
			group->has_thread = 1;
		}
		else {
			for (int i = 0, n = 0; i < e->istream.channels; ++i) {
				if (GET_BIT(channel_selector, i)) {
					group->ibuf[i] = group->fft_ibuf[n];
					group->obuf[i] = group->fft_obuf[n];
					++n;
				}
			}
		}
	}

	return e;

	fail:
	fir_p_effect_destroy(e);
	free(e);
	return NULL;
}

struct effect * fir_p_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int filter_channels;
	ssize_t filter_frames, max_part_len = 0;
	struct effect *e;
	sample_t *filter_data;
	struct codec_params c_params;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	char *endptr;

	int err = fir_parse_opts(ei, istream, &c_params, &g, argc, argv, NULL, NULL, NULL);
	if (err || g.ind < argc-2 || g.ind > argc-1) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (g.ind == argc-2) {
		max_part_len = strtol(argv[g.ind], &endptr, 10);
		CHECK_ENDPTR(argv[g.ind], endptr, "max_part_len", return NULL);
		++g.ind;
	}
	c_params.path = argv[g.ind];
	filter_data = fir_read_filter(ei, istream, dir, &c_params, &filter_channels, &filter_frames);
	if (filter_data == NULL)
		return NULL;
	e = fir_p_effect_init_with_filter(ei, istream, channel_selector, filter_data, filter_channels, filter_frames, max_part_len);
	free(filter_data);
	return e;
}
