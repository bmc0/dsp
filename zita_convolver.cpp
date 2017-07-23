#include <cstdio>
#include <cstdlib>
#include <zita-convolver.h>
#include "zita_convolver.h"

extern "C" {
	#include "util.h"
	#include "codec.h"
	#include "sampleconv.h"
}

struct zita_convolver_state {
	ssize_t filter_frames, len, pos, drain_frames, drain_pos;
	sample_t **output;
	Convproc *cproc;
	int has_output, is_draining;
};

void zita_convolver_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	ssize_t i, k, iframes = 0, oframes = 0;
	while (iframes < *frames) {
		while (state->pos < state->len && iframes < *frames) {
			for (i = k = 0; i < e->ostream.channels; ++i) {
#ifdef __SYMMETRIC_IO__
				obuf[oframes * e->ostream.channels + i] = (state->has_output) ? state->output[i][state->pos] : 0;
#else
				if (state->has_output)
					obuf[oframes * e->ostream.channels + i] = state->output[i][state->pos];
#endif
				if (GET_BIT(e->channel_selector, i)) {
					state->cproc->inpdata(k)[state->pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
					++k;
				}
				else
					state->output[i][state->pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
			}
#ifdef __SYMMETRIC_IO__
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

void zita_convolver_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct zita_convolver_state *state = (struct zita_convolver_state *) e->data;
	if (!state->has_output && state->pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->filter_frames;
			if (state->has_output)
				state->drain_frames += state->len - state->pos;
			state->drain_frames += state->pos;
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			zita_convolver_effect_run(e, frames, NULL, obuf);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
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

struct effect * zita_convolver_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	int i, k, n_channels;
	unsigned int min_part_len = 0, max_part_len = 0;
	struct effect *e;
	struct zita_convolver_state *state;
	struct codec *c_filter;
	Convproc *cproc;
	sample_t *buf_interleaved;
	float **buf_planar;
	char *p;

	if (argc > 4 || argc < 2) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (argc > 2)
		min_part_len = atoi(argv[1]);
	if (argc > 3)
		max_part_len = atoi(argv[2]);
	min_part_len = (min_part_len == 0) ? Convproc::MINPART : min_part_len;
	max_part_len = (max_part_len == 0) ? Convproc::MAXPART : max_part_len;
	if (min_part_len < Convproc::MINPART || min_part_len > Convproc::MAXPART || max_part_len < Convproc::MINPART || max_part_len > Convproc::MAXPART) {
		LOG(LL_ERROR, "dsp: %s: error: partition lengths must be within [%d,%d] or 0 for default\n", argv[0], Convproc::MINPART, Convproc::MAXPART);
		return NULL;
	}
	if (max_part_len < min_part_len) {
		LOG(LL_ERROR, "dsp: %s: warning: max_part_len < min_part_len\n", argv[0]);
		max_part_len = min_part_len;
	}

	for (i = n_channels = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	if (n_channels > MINIMUM(Convproc::MAXINP, Convproc::MAXOUT)) {
		LOG(LL_ERROR, "dsp: %s: error: number of channels must not exceed %d\n", argv[0], MINIMUM(Convproc::MAXINP, Convproc::MAXOUT));
		return NULL;
	}
	p = construct_full_path(dir, argv[argc - 1]);
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
	cproc = new Convproc;
	if (cproc->configure(n_channels, n_channels, c_filter->frames, min_part_len, min_part_len, max_part_len)) {
		LOG(LL_ERROR, "dsp: %s: error: failed to configure convolution engine\n", argv[0]);
		destroy_codec(c_filter);
		delete cproc;
		return NULL;
	}
	LOG(LL_VERBOSE, "dsp: %s: info: filter_frames=%ld min_part_len=%d max_part_len=%d\n", argv[0], c_filter->frames, min_part_len, max_part_len);

	e = (struct effect *) calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = (char *) NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = zita_convolver_effect_run;
	e->delay = zita_convolver_effect_delay;
	e->reset = zita_convolver_effect_reset;
	e->drain = zita_convolver_effect_drain;
	e->destroy = zita_convolver_effect_destroy;

	state = (struct zita_convolver_state *) calloc(1, sizeof(struct zita_convolver_state));
	state->filter_frames = c_filter->frames;
	state->len = min_part_len;
	state->cproc = cproc;
	state->output = (sample_t **) calloc(istream->channels, sizeof(sample_t *));
	for (i = 0; i < istream->channels; ++i)
		state->output[i] = (sample_t *) calloc(state->len, sizeof(sample_t));
	e->data = (void *) state;

	buf_interleaved = (sample_t *) calloc(c_filter->frames * c_filter->channels, sizeof(sample_t));
	if (c_filter->read(c_filter, buf_interleaved, c_filter->frames) != c_filter->frames)
		LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	buf_planar = (float **) calloc(c_filter->channels, sizeof(float *));
	for (i = 0; i < c_filter->channels; ++i)
		buf_planar[i] = (float *) calloc(c_filter->frames, sizeof(float));
	write_buf_floatp(buf_interleaved, buf_planar, c_filter->channels, c_filter->frames);
	free(buf_interleaved);
	for (i = k = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			cproc->impdata_create(k, k, 1, buf_planar[(c_filter->channels == 1) ? 0 : k], 0, c_filter->frames);
			++k;
		}
	}
	for (i = 0; i < c_filter->channels; ++i)
		free(buf_planar[i]);
	free(buf_planar);
	destroy_codec(c_filter);
	cproc->start_process(0, 0);

	return e;
}
