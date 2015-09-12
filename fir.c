#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "fir.h"
#include "util.h"
#include "codec.h"

struct partition {
	ssize_t len, fr_len, delay, in_pos, pos;
	fftw_complex **filter_fr;
	sample_t **input, **output, **overlap;
	fftw_plan *r2c_plan, *c2r_plan;
	int has_output;
};

struct fir_state {
	ssize_t nparts, in_len, in_pos, impulse_len, drain_frames, drain_pos;
	fftw_complex *tmp_fr;
	sample_t **input;
	struct partition *part;
	int is_draining;
};

void fir_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	ssize_t i, k, j, iframes = 0, oframes = 0;

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
				for (i = 0; i < e->ostream.channels; ++i) {
					if (state->part[j].input[i]) {
						fftw_execute(state->part[j].r2c_plan[i]);
						for (k = 0; k < state->part[j].fr_len; ++k)
							state->tmp_fr[k] *= state->part[j].filter_fr[i][k];
						fftw_execute(state->part[j].c2r_plan[i]);
						for (k = 0; k < state->part[j].len * 2; ++k)
							state->part[j].output[i][k] /= state->part[j].len * 2;
						for (k = 0; k < state->part[j].len; ++k) {
							state->part[j].output[i][k] += state->part[j].overlap[i][k];
							state->part[j].overlap[i][k] = state->part[j].output[i][k + state->part[j].len];
						}
					}
				}
				state->part[j].pos = 0;
				state->part[j].has_output = 1;
			}
		}
	}
	*frames = oframes;
}

void fir_effect_reset(struct effect *e)
{
	int i, k;
	struct fir_state *state = (struct fir_state *) e->data;
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

void fir_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	if (!state->part[0].has_output && state->part[0].pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->impulse_len;
			if (state->part[0].has_output)
				state->drain_frames += state->part[0].len;
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			fir_effect_run(e, frames, NULL, obuf);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
}

void fir_effect_destroy(struct effect *e)
{
	int i, k;
	struct fir_state *state = (struct fir_state *) e->data;
	for (k = 0; k < state->nparts; ++k) {
		for (i = 0; i < e->ostream.channels; ++i) {
			fftw_free(state->part[k].input[i]);
			fftw_free(state->part[k].output[i]);
			fftw_free(state->part[k].overlap[i]);
			fftw_free(state->part[k].filter_fr[i]);
			fftw_destroy_plan(state->part[k].r2c_plan[i]);
			fftw_destroy_plan(state->part[k].c2r_plan[i]);
		}
		free(state->part[k].input);
		free(state->part[k].output);
		free(state->part[k].overlap);
		free(state->part[k].filter_fr);
		free(state->part[k].r2c_plan);
		free(state->part[k].c2r_plan);
	}
	for (i = 0; i < e->ostream.channels; ++i)
		free(state->input[i]);
	free(state->input);
	fftw_free(state->tmp_fr);
	free(state->part);
	free(state);
}

struct effect * fir_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, int argc, char **argv)
{
	int i, k, j, p, n_channels;
	ssize_t filter_pos = 0, delay = 0, max_delay = 0, min_part_len = dsp_globals.buf_frames, max_part_len = SSIZE_MAX;
	struct effect *e;
	struct fir_state *state;
	struct codec *c_filter;
	sample_t *tmp_buf = NULL, *filter;
	fftw_plan filter_plan;

	if (argc > 4 || argc < 2) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (argc > 2) {
		min_part_len = atol(argv[1]);
		if (min_part_len < 1) {
			LOG(LL_ERROR, "dsp: %s: error minimum partition length cannot be < 1\n", argv[0]);
			return NULL;
		}
	}
	if (argc > 3) {
		max_part_len = atol(argv[2]);
		if (max_part_len < 1) {
			LOG(LL_ERROR, "dsp: %s: error maxmimum partition length cannot be less than minimum partition length\n", argv[0]);
			return NULL;
		}
	}

	min_part_len = pow(2, floor(log(min_part_len) / log(2)));  /* find nearest power of two that meets or exceeds latency requirement */
	if (max_part_len < SSIZE_MAX)
		max_part_len = pow(2, floor(log(max_part_len) / log(2)));

	for (i = n_channels = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	c_filter = init_codec(argv[argc - 1], NULL, NULL, istream->fs, n_channels, CODEC_ENDIAN_DEFAULT, CODEC_MODE_READ);
	if (c_filter == NULL) {
		LOG(LL_ERROR, "dsp: %s: error: failed to open impulse file: %s\n", argv[0], argv[argc - 1]);
		return NULL;
	}
	if (c_filter->channels != 1 && c_filter->channels != n_channels) {
		LOG(LL_ERROR, "dsp: %s: error: channel mismatch: channels=%d impulse_channels=%d\n", argv[0], n_channels, c_filter->channels);
		destroy_codec(c_filter);
		return NULL;
	}
	if (c_filter->fs != istream->fs) {
		LOG(LL_ERROR, "dsp: %s: error: sample rate mismatch: fs=%d impulse_fs=%d\n", argv[0], istream->fs, c_filter->fs);
		destroy_codec(c_filter);
		return NULL;
	}
	if (c_filter->frames < 1) {
		LOG(LL_ERROR, "dsp: %s: error: impulse length must be >= 1\n", argv[0]);
		destroy_codec(c_filter);
		return NULL;
	}
	LOG(LL_VERBOSE, "dsp: %s: info: filter_frames=%zd\n", argv[0], c_filter->frames);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = fir_effect_run;
	e->reset = fir_effect_reset;
	e->drain = fir_effect_drain;
	e->destroy = fir_effect_destroy;

	state = calloc(1, sizeof(struct fir_state));
	e->data = state;

	k = (c_filter->frames > min_part_len) ? min_part_len : c_filter->frames;
	j = 0;
	for (i = j = 0; j < c_filter->frames; ++i) {
		state->part = realloc(state->part, sizeof(struct partition) * ++state->nparts);
		memset(&state->part[i], 0, sizeof(struct partition));
		state->part[i].len = k;
		state->part[i].fr_len = state->part[i].len + 1;
		state->part[i].input = calloc(e->ostream.channels, sizeof(sample_t *));
		state->part[i].output = calloc(e->ostream.channels, sizeof(sample_t *));
		state->part[i].overlap = calloc(e->ostream.channels, sizeof(sample_t *));
		state->part[i].filter_fr = calloc(e->ostream.channels, sizeof(fftw_complex *));
		state->part[i].r2c_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		state->part[i].c2r_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
		if (i > 0)
			delay += state->part[i - 1].len - state->part[i].len;
		state->part[i].delay = delay;
		max_delay = MAXIMUM(delay, max_delay);
		delay += state->part[i].len;
		j += state->part[i].len;
		LOG(LL_VERBOSE, "dsp: %s: info: partition %d: len=%zd delay=%zd total=%d\n", argv[0], i, state->part[i].len, state->part[i].delay, j);
		if (k < max_part_len && j + k < c_filter->frames)
			k *= 2;
	}
	state->in_len = max_delay + 1;
	state->input = calloc(e->ostream.channels, sizeof(sample_t *));
	for (i = 0; i < e->ostream.channels; ++i)
		if (GET_BIT(channel_selector, i))
			state->input[i] = calloc(state->in_len, sizeof(sample_t));
	filter = fftw_malloc(j * 2 * sizeof(sample_t));
	memset(filter, 0, j * 2 * sizeof(sample_t));
	state->tmp_fr = fftw_malloc(state->part[state->nparts - 1].fr_len * sizeof(fftw_complex));
	if (c_filter->channels == 1) {
		if (c_filter->read(c_filter, filter, c_filter->frames) != c_filter->frames)
			LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	}
	else {
		tmp_buf = calloc(c_filter->frames * c_filter->channels, sizeof(sample_t));
		if (c_filter->read(c_filter, tmp_buf, c_filter->frames) != c_filter->frames)
			LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	}

	for (p = 0; p < state->nparts; ++p) {
		filter_plan = fftw_plan_dft_r2c_1d(state->part[p].len * 2, &filter[filter_pos], state->tmp_fr, FFTW_ESTIMATE);
		if (c_filter->channels == 1)
			fftw_execute(filter_plan);
		for (i = 0; i < e->ostream.channels; ++i) {
			state->part[p].output[i] = fftw_malloc(state->part[p].len * 2 * sizeof(sample_t));
			memset(state->part[p].output[i], 0, state->part[p].len * 2 * sizeof(sample_t));
			if (GET_BIT(channel_selector, i)) {
				state->part[p].input[i] = fftw_malloc(state->part[p].len * 2 * sizeof(sample_t));
				memset(state->part[p].input[i], 0, state->part[p].len * 2 * sizeof(sample_t));
				state->part[p].overlap[i] = fftw_malloc(state->part[p].len * sizeof(sample_t));
				memset(state->part[p].overlap[i], 0, state->part[p].len * sizeof(sample_t));
				state->part[p].filter_fr[i] = fftw_malloc(state->part[p].fr_len * sizeof(fftw_complex));
				state->part[p].r2c_plan[i] = fftw_plan_dft_r2c_1d(state->part[p].len * 2, state->part[p].input[i], state->tmp_fr, FFTW_ESTIMATE);
				state->part[p].c2r_plan[i] = fftw_plan_dft_c2r_1d(state->part[p].len * 2, state->tmp_fr, state->part[p].output[i], FFTW_ESTIMATE);
				if (c_filter->channels == 1)
					memcpy(state->part[p].filter_fr[i], state->tmp_fr, state->part[p].fr_len * sizeof(fftw_complex));
				else {
					for (j = 0; j < state->part[p].len && j + filter_pos < c_filter->frames; ++j)
						filter[j + filter_pos] = tmp_buf[(j + filter_pos) * c_filter->channels + i];
					fftw_execute(filter_plan);
					memcpy(state->part[p].filter_fr[i], state->tmp_fr, state->part[p].fr_len * sizeof(fftw_complex));
				}
			}
		}
		fftw_destroy_plan(filter_plan);
		filter_pos += state->part[p].len;
		state->part[p].in_pos = (state->part[p].delay == 0) ? 0 : state->in_len - state->part[p].delay;
	}
	state->impulse_len = c_filter->frames;
	destroy_codec(c_filter);
	free(tmp_buf);
	fftw_free(filter);

	return e;
}
