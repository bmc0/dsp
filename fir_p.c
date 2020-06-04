#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <complex.h>
#include <fftw3.h>
#include "fir_p.h"
#include "util.h"
#include "codec.h"

#define MIN_PART_LEN 1
#define MAX_PART_LEN INT_MAX
#define MAX_DIRECT_LEN 32

#define DEFAULT_MIN_PART_LEN 16
#define DEFAULT_MAX_PART_LEN 16384

struct partition {
	ssize_t len, delay, in_pos, pos;
	union {
		struct {
			ssize_t fr_len;
			fftw_complex **filter_fr;
			fftw_plan *r2c_plan, *c2r_plan;
		} fft;
		struct {
			sample_t **filter;
		} direct;
	} m;
	sample_t **input, **output, **overlap;
	int has_output;
};

struct fir_p_state {
	ssize_t nparts, in_len, in_pos, impulse_len, drain_frames, drain_pos;
	fftw_complex *tmp_fr;
	sample_t **input;
	struct partition *part;
	int is_draining;
};

sample_t * fir_p_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	ssize_t i, k, j, l, iframes = 0, oframes = 0;

	while (iframes < *frames) {
		while (state->part[0].pos < state->part[0].len && iframes < *frames) {
			for (i = 0; i < e->ostream.channels; ++i) {
				if (state->input[i])
					state->input[i][state->in_pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
				#ifdef __SYMMETRIC_IO__
					obuf[oframes * e->ostream.channels + i] = 0;
				#else
					if (state->part[0].has_output)
						obuf[oframes * e->ostream.channels + i] = 0;
				#endif
			}
			for (k = 0; k < state->nparts; ++k) {
				for (i = 0; i < e->ostream.channels; ++i) {
					obuf[oframes * e->ostream.channels + i] += state->part[k].output[i][state->part[k].pos];
					if (state->input[i])
						state->part[k].input[i][state->part[k].pos] = state->input[i][state->part[k].in_pos];
				}
			}
			for (i = 0; i < e->ostream.channels; ++i)
				if (!state->input[i])
					state->part[0].output[i][state->part[0].pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
			#ifdef __SYMMETRIC_IO__
				++oframes;
			#else
				if (state->part[0].has_output)
					++oframes;
			#endif
			++iframes;
			if (++state->in_pos == state->in_len)
				state->in_pos = 0;
			for (k = 0; k < state->nparts; ++k) {
				if (++state->part[k].in_pos == state->in_len)
					state->part[k].in_pos = 0;
				++state->part[k].pos;
			}
		}

		for (j = 0; j < state->nparts; ++j) {
			if (state->part[j].pos == state->part[j].len) {
				if (state->part[j].len > MAX_DIRECT_LEN) {
					/* FFT convolution */
					for (i = 0; i < e->ostream.channels; ++i) {
						if (state->part[j].input[i]) {
							/* FFT convolution */
							fftw_execute(state->part[j].m.fft.r2c_plan[i]);
							for (k = 0; k < state->part[j].m.fft.fr_len; ++k)
								state->tmp_fr[k] *= state->part[j].m.fft.filter_fr[i][k];
							fftw_execute(state->part[j].m.fft.c2r_plan[i]);
							for (k = 0; k < state->part[j].len * 2; ++k)
								state->part[j].output[i][k] /= state->part[j].len * 2;
							for (k = 0; k < state->part[j].len; ++k) {
								state->part[j].output[i][k] += state->part[j].overlap[i][k];
								state->part[j].overlap[i][k] = state->part[j].output[i][k + state->part[j].len];
							}
						}
					}
				}
				else {
					/* Direct convolution */
					for (i = 0; i < e->ostream.channels; ++i) {
						if (state->part[j].input[i]) {
							memset(state->part[j].output[i], 0, state->part[j].len * 2 * sizeof(sample_t));
							for (k = 0; k < state->part[j].len; ++k)
								for (l = 0; l < state->part[j].len; ++l)
									state->part[j].output[i][k + l] += state->part[j].input[i][k] * state->part[j].m.direct.filter[i][l];
							for (k = 0; k < state->part[j].len; ++k) {
								state->part[j].output[i][k] += state->part[j].overlap[i][k];
								state->part[j].overlap[i][k] = state->part[j].output[i][k + state->part[j].len];
							}
						}
					}
				}
				state->part[j].pos = 0;
				state->part[j].has_output = 1;
			}
		}
	}
	*frames = oframes;
	return obuf;
}

ssize_t fir_p_effect_delay(struct effect *e)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	return (state->part[0].has_output) ? state->part[0].len : state->part[0].pos;
}

void fir_p_effect_reset(struct effect *e)
{
	int i, k;
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	for (i = 0; i < e->ostream.channels; ++i)
		if (state->input[i])
			memset(state->input[i], 0, state->in_len * sizeof(sample_t));
	for (k = 0; k < state->nparts; ++k) {
		state->part[k].pos = 0;
		state->part[k].has_output = 0;
		for (i = 0; i < e->ostream.channels; ++i) {
			memset(state->part[k].output[i], 0, state->part[k].len * 2 * sizeof(sample_t));
			if (state->part[k].overlap[i])
				memset(state->part[k].overlap[i], 0, state->part[k].len * sizeof(sample_t));
		}
	}
}

void fir_p_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	if (!state->part[0].has_output && state->part[0].pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->impulse_len;
			if (state->part[0].has_output)
				state->drain_frames += state->part[0].len - state->part[0].pos;
			state->drain_frames += state->part[0].pos;
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			fir_p_effect_run(e, frames, NULL, obuf);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
}

void fir_p_effect_destroy(struct effect *e)
{
	int i, k;
	struct fir_p_state *state = (struct fir_p_state *) e->data;
	for (k = 0; k < state->nparts; ++k) {
		for (i = 0; i < e->ostream.channels; ++i) {
			fftw_free(state->part[k].input[i]);
			fftw_free(state->part[k].output[i]);
			fftw_free(state->part[k].overlap[i]);
			if (state->part[k].len > MAX_DIRECT_LEN) {
				fftw_free(state->part[k].m.fft.filter_fr[i]);
				fftw_destroy_plan(state->part[k].m.fft.r2c_plan[i]);
				fftw_destroy_plan(state->part[k].m.fft.c2r_plan[i]);
			}
			else {
				free(state->part[k].m.direct.filter[i]);
			}
		}
		free(state->part[k].input);
		free(state->part[k].output);
		free(state->part[k].overlap);
		if (state->part[k].len > MAX_DIRECT_LEN) {
			free(state->part[k].m.fft.filter_fr);
			free(state->part[k].m.fft.r2c_plan);
			free(state->part[k].m.fft.c2r_plan);
		}
		else {
			free(state->part[k].m.direct.filter);
		}
	}
	for (i = 0; i < e->ostream.channels; ++i)
		free(state->input[i]);
	free(state->input);
	fftw_free(state->tmp_fr);
	free(state->part);
	free(state);
}

struct effect * fir_p_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	int i, k, j, n_channels;
	ssize_t filter_pos = 0, delay = 0, max_delay = 0, min_part_len = 0, max_part_len = 0;
	struct effect *e;
	struct fir_p_state *state;
	struct codec *c_filter;
	sample_t *tmp_buf = NULL, *filter = NULL;
	char *endptr, *p;
	fftw_plan filter_plan;

	if (argc > 4 || argc < 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (argc > 2) {
		min_part_len = strtol(argv[1], &endptr, 10);
		CHECK_ENDPTR(argv[1], endptr, "min_part_len", return NULL);
	}
	if (argc > 3) {
		max_part_len = strtol(argv[2], &endptr, 10);
		CHECK_ENDPTR(argv[2], endptr, "max_part_len", return NULL);
	}
	min_part_len = (min_part_len == 0) ? DEFAULT_MIN_PART_LEN : min_part_len;
	max_part_len = (max_part_len == 0) ? DEFAULT_MAX_PART_LEN : max_part_len;
	if (!(IS_POWER_OF_2(min_part_len) && IS_POWER_OF_2(max_part_len))) {
		LOG_FMT(LL_ERROR, "%s: error: partition lengths must be powers of 2", argv[0]);
		return NULL;
	}
	if (min_part_len < MIN_PART_LEN || min_part_len > MAX_PART_LEN || max_part_len > MAX_PART_LEN) {
		LOG_FMT(LL_ERROR, "%s: error: partition lengths must be within [%d,%d] or 0 for default", argv[0], MIN_PART_LEN, MAX_PART_LEN);
		return NULL;
	}
	if (max_part_len < min_part_len) {
		LOG_FMT(LL_ERROR, "%s: warning: max_part_len < min_part_len", argv[0]);
		max_part_len = min_part_len;
	}

	for (i = n_channels = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	p = construct_full_path(dir, argv[argc - 1]);
	c_filter = init_codec(p, NULL, NULL, istream->fs, n_channels, CODEC_ENDIAN_DEFAULT, CODEC_MODE_READ);
	if (c_filter == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: failed to open impulse file: %s", argv[0], p);
		free(p);
		return NULL;
	}
	free(p);
	if (c_filter->channels != 1 && c_filter->channels != n_channels) {
		LOG_FMT(LL_ERROR, "%s: error: channel mismatch: channels=%d impulse_channels=%d", argv[0], n_channels, c_filter->channels);
		destroy_codec(c_filter);
		return NULL;
	}
	if (c_filter->fs != istream->fs) {
		LOG_FMT(LL_ERROR, "%s: error: sample rate mismatch: fs=%d impulse_fs=%d", argv[0], istream->fs, c_filter->fs);
		destroy_codec(c_filter);
		return NULL;
	}
	if (c_filter->frames < 1) {
		LOG_FMT(LL_ERROR, "%s: error: impulse length must be >= 1", argv[0]);
		destroy_codec(c_filter);
		return NULL;
	}
	LOG_FMT(LL_VERBOSE, "%s: info: filter_frames=%zd", argv[0], c_filter->frames);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->run = fir_p_effect_run;
	e->delay = fir_p_effect_delay;
	e->reset = fir_p_effect_reset;
	e->drain = fir_p_effect_drain;
	e->destroy = fir_p_effect_destroy;

	state = calloc(1, sizeof(struct fir_p_state));
	e->data = state;

	k = (c_filter->frames > min_part_len) ? min_part_len : c_filter->frames;
	for (i = j = 0; j < c_filter->frames; ++i) {
		state->part = realloc(state->part, sizeof(struct partition) * ++state->nparts);
		memset(&state->part[i], 0, sizeof(struct partition));
		state->part[i].len = k;
		state->part[i].input = calloc(e->ostream.channels, sizeof(sample_t *));
		state->part[i].output = calloc(e->ostream.channels, sizeof(sample_t *));
		state->part[i].overlap = calloc(e->ostream.channels, sizeof(sample_t *));
		if (state->part[i].len > MAX_DIRECT_LEN) {
			state->part[i].m.fft.fr_len = state->part[i].len + 1;
			state->part[i].m.fft.filter_fr = calloc(e->ostream.channels, sizeof(fftw_complex *));
			state->part[i].m.fft.r2c_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
			state->part[i].m.fft.c2r_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		}
		else {
			state->part[i].m.direct.filter = calloc(e->ostream.channels, sizeof(sample_t *));
		}
		state->part[i].delay = delay;
		max_delay = MAXIMUM(delay, max_delay);
		j += state->part[i].len;
		if (k < max_part_len && j + k < c_filter->frames)
			k *= 2;
		else
			delay += state->part[i].len;
		LOG_FMT(LL_VERBOSE, "%s: info: partition %d: len=%zd delay=%zd total=%d", argv[0], i, state->part[i].len, state->part[i].delay, j);
	}
	state->in_len = max_delay + 1;
	state->input = calloc(e->ostream.channels, sizeof(sample_t *));
	for (i = 0; i < e->ostream.channels; ++i)
		if (GET_BIT(channel_selector, i))
			state->input[i] = calloc(state->in_len, sizeof(sample_t));
	if (state->part[state->nparts - 1].len > MAX_DIRECT_LEN) {
		filter = fftw_malloc(state->part[state->nparts - 1].len * 2 * sizeof(sample_t));
		memset(filter, 0, state->part[state->nparts - 1].len * 2 * sizeof(sample_t));
		state->tmp_fr = fftw_malloc(state->part[state->nparts - 1].m.fft.fr_len * sizeof(fftw_complex));
	}
	tmp_buf = calloc(c_filter->frames * c_filter->channels, sizeof(sample_t));
	if (c_filter->read(c_filter, tmp_buf, c_filter->frames) != c_filter->frames)
		LOG_FMT(LL_ERROR, "%s: warning: short read", argv[0]);

	for (k = 0; k < state->nparts; ++k) {
		if (state->part[k].len > MAX_DIRECT_LEN) {
			filter_plan = fftw_plan_dft_r2c_1d(state->part[k].len * 2, filter, state->tmp_fr, FFTW_ESTIMATE);
			if (c_filter->channels == 1) {
				if (filter_pos + state->part[k].len > c_filter->frames)
					memcpy(filter, &tmp_buf[filter_pos], (c_filter->frames - filter_pos) * sizeof(sample_t));
				else
					memcpy(filter, &tmp_buf[filter_pos], state->part[k].len * sizeof(sample_t));
				fftw_execute(filter_plan);
			}
		}
		else
			filter_plan = NULL;
		for (i = 0; i < e->ostream.channels; ++i) {
			state->part[k].output[i] = fftw_malloc(state->part[k].len * 2 * sizeof(sample_t));
			memset(state->part[k].output[i], 0, state->part[k].len * 2 * sizeof(sample_t));
			if (GET_BIT(channel_selector, i)) {
				state->part[k].input[i] = fftw_malloc(state->part[k].len * 2 * sizeof(sample_t));
				memset(state->part[k].input[i], 0, state->part[k].len * 2 * sizeof(sample_t));
				state->part[k].overlap[i] = fftw_malloc(state->part[k].len * sizeof(sample_t));
				memset(state->part[k].overlap[i], 0, state->part[k].len * sizeof(sample_t));
				if (state->part[k].len > MAX_DIRECT_LEN) {
					state->part[k].m.fft.filter_fr[i] = fftw_malloc(state->part[k].m.fft.fr_len * sizeof(fftw_complex));
					state->part[k].m.fft.r2c_plan[i] = fftw_plan_dft_r2c_1d(state->part[k].len * 2, state->part[k].input[i], state->tmp_fr, FFTW_ESTIMATE);
					state->part[k].m.fft.c2r_plan[i] = fftw_plan_dft_c2r_1d(state->part[k].len * 2, state->tmp_fr, state->part[k].output[i], FFTW_ESTIMATE);
					if (c_filter->channels == 1)
						memcpy(state->part[k].m.fft.filter_fr[i], state->tmp_fr, state->part[k].m.fft.fr_len * sizeof(fftw_complex));
					else {
						for (j = 0; j < state->part[k].len && j + filter_pos < c_filter->frames; ++j)
							filter[j] = tmp_buf[(j + filter_pos) * c_filter->channels + i];
						fftw_execute(filter_plan);
						memcpy(state->part[k].m.fft.filter_fr[i], state->tmp_fr, state->part[k].m.fft.fr_len * sizeof(fftw_complex));
					}
				}
				else {
					state->part[k].m.direct.filter[i] = calloc(state->part[k].len, sizeof(sample_t));
					if (c_filter->channels == 1) {
						if (filter_pos + state->part[k].len > c_filter->frames)
							memcpy(state->part[k].m.direct.filter[i], &tmp_buf[filter_pos], (c_filter->frames - filter_pos) * sizeof(sample_t));
						else
							memcpy(state->part[k].m.direct.filter[i], &tmp_buf[filter_pos], state->part[k].len * sizeof(sample_t));
					}
					else {
						for (j = 0; j < state->part[k].len && j + filter_pos < c_filter->frames; ++j)
							state->part[k].m.direct.filter[i][j] = tmp_buf[(j + filter_pos) * c_filter->channels + i];
					}
				}
			}
		}
		if (state->part[k].len > MAX_DIRECT_LEN) {
			fftw_destroy_plan(filter_plan);
			memset(filter, 0, state->part[k].len * sizeof(sample_t));
		}
		filter_pos += state->part[k].len;
		state->part[k].in_pos = (state->part[k].delay == 0) ? 0 : state->in_len - state->part[k].delay;
	}
	state->impulse_len = c_filter->frames;
	destroy_codec(c_filter);
	free(tmp_buf);
	fftw_free(filter);

	return e;
}
