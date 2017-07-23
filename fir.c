#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "fir.h"
#include "util.h"
#include "codec.h"

struct fir_state {
	ssize_t len, fr_len, buf_pos, drain_pos, drain_frames;
	fftw_complex **filter_fr, *tmp_fr;
	sample_t **input, **output, **overlap;
	fftw_plan *r2c_plan, *c2r_plan;
	int has_output, is_draining;
};

void fir_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	ssize_t i, k, iframes = 0, oframes = 0;

	while (iframes < *frames) {
		while (state->buf_pos < state->len && iframes < *frames) {
			for (i = 0; i < e->ostream.channels; ++i) {
#ifdef __SYMMETRIC_IO__
				obuf[oframes * e->ostream.channels + i] = (state->has_output) ? state->output[i][state->buf_pos] : 0;
#else
				if (state->has_output)
					obuf[oframes * e->ostream.channels + i] = state->output[i][state->buf_pos];
#endif
				if (state->input[i])
					state->input[i][state->buf_pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
				else
					state->output[i][state->buf_pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
			}
#ifdef __SYMMETRIC_IO__
			++oframes;
#else
			if (state->has_output)
				++oframes;
#endif
			++iframes;
			++state->buf_pos;
		}

		if (state->buf_pos == state->len) {
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
			state->buf_pos = 0;
			state->has_output = 1;
		}
	}
	*frames = oframes;
}

ssize_t fir_effect_delay(struct effect *e)
{
	struct fir_state *state = (struct fir_state *) e->data;
	return (state->has_output) ? state->len : state->buf_pos;
}

void fir_effect_reset(struct effect *e)
{
	int i;
	struct fir_state *state = (struct fir_state *) e->data;
	state->buf_pos = 0;
	state->has_output = 0;
	for (i = 0; i < e->ostream.channels; ++i)
		if (state->overlap[i])
			memset(state->overlap[i], 0, state->len * sizeof(sample_t));
}

void fir_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct fir_state *state = (struct fir_state *) e->data;
	if (!state->has_output && state->buf_pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->len;
			if (state->has_output)
				state->drain_frames += state->len - state->buf_pos;
			state->drain_frames += state->buf_pos;
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

struct effect * fir_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	int i, k, j, n_channels;
	struct effect *e;
	struct fir_state *state;
	struct codec *c_filter;
	sample_t *tmp_buf = NULL, *filter;
	char *p;
	fftw_plan filter_plan;

	if (argc != 2) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	for (i = n_channels = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	p = construct_full_path(dir, argv[1]);
	c_filter = init_codec(p, NULL, NULL, istream->fs, n_channels, CODEC_ENDIAN_DEFAULT, CODEC_MODE_READ);
	if (c_filter == NULL) {
		LOG(LL_ERROR, "dsp: %s: error: failed to open impulse file: %s\n", argv[0], p);
		free(p);
		return NULL;
	}
	free(p);
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
	LOG(LL_VERBOSE, "dsp: %s: info: filter_frames=%ld\n", argv[0], c_filter->frames);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = fir_effect_run;
	e->delay = fir_effect_delay;
	e->reset = fir_effect_reset;
	e->drain = fir_effect_drain;
	e->destroy = fir_effect_destroy;

	state = calloc(1, sizeof(struct fir_state));
	e->data = state;

	state->len = c_filter->frames;
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
	if (c_filter->channels == 1) {
		if (c_filter->read(c_filter, filter, state->len) != state->len)
			LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
		fftw_execute(filter_plan);
	}
	else {
		tmp_buf = calloc(c_filter->frames * c_filter->channels, sizeof(sample_t));
		if (c_filter->read(c_filter, tmp_buf, state->len) != state->len)
			LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
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
			if (c_filter->channels == 1)
				memcpy(state->filter_fr[i], state->tmp_fr, state->fr_len * sizeof(fftw_complex));
			else {
				for (j = 0; j < state->len; ++j)
					filter[j] = tmp_buf[j * c_filter->channels + k];
				fftw_execute(filter_plan);
				memcpy(state->filter_fr[i], state->tmp_fr, state->fr_len * sizeof(fftw_complex));
				++k;
			}
		}
	}
	destroy_codec(c_filter);
	fftw_destroy_plan(filter_plan);
	free(tmp_buf);
	fftw_free(filter);

	return e;
}
