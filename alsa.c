/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <stdint.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include "alsa.h"
#include "util.h"
#include "sampleconv.h"

#define DUMP_PCM_INFO 0

struct alsa_enc_info {
	const char *name;
	snd_pcm_format_t fmt;
	int bytes, prec, can_dither;
	void (*write_func)(sample_t *, void *, ssize_t);
	void (*read_func)(void *, sample_t *, ssize_t);
};

struct alsa_state {
	snd_pcm_t *dev;
	struct alsa_enc_info *enc_info;
	snd_pcm_sframes_t delay;
};

static const char codec_name[] = "alsa";

static int alsa_prepare_device(struct alsa_state *state)
{
	int err;
	if ((err = snd_pcm_prepare(state->dev)) < 0)
		LOG_FMT(LL_ERROR, "%s: error: failed to prepare device: %s", codec_name, snd_strerror(err));
	return err;
}

ssize_t alsa_read(struct codec *c, sample_t *sbuf, ssize_t frames)
{
	ssize_t n, r = 0;
	uint8_t *buf = (uint8_t *) sbuf;
	struct alsa_state *state = (struct alsa_state *) c->data;

	if (snd_pcm_state(state->dev) == SND_PCM_STATE_SETUP && alsa_prepare_device(state) < 0)
		return 0;

	try_again:
	n = snd_pcm_readi(state->dev, buf, frames);
	if (n < 0) {
		if (n == -EAGAIN)
			goto try_again;
		if (n == -EPIPE)
			LOG_FMT(LL_ERROR, "%s: warning: overrun occurred", codec_name);
		n = snd_pcm_recover(state->dev, n, 1);
		if (n < 0) {
			LOG_FMT(LL_ERROR, "%s: error: read failed", codec_name);
			return r;
		}
		else
			goto try_again;
	}
	r += n;
	if (r < frames) {
		buf += n * c->channels * state->enc_info->bytes;
		goto try_again;
	}
	state->enc_info->read_func(sbuf, sbuf, n * c->channels);
	return r;
}

ssize_t alsa_write(struct codec *c, sample_t *sbuf, ssize_t frames)
{
	ssize_t n, w = 0;
	uint8_t *buf = (uint8_t *) sbuf;
	struct alsa_state *state = (struct alsa_state *) c->data;

	if (snd_pcm_state(state->dev) == SND_PCM_STATE_SETUP && alsa_prepare_device(state) < 0)
		return 0;

	state->enc_info->write_func(sbuf, sbuf, frames * c->channels);
	try_again:
	n = snd_pcm_writei(state->dev, buf, frames);
	if (n < 0) {
		if (n == -EAGAIN)
			goto try_again;
		if (n == -EPIPE)
			LOG_FMT(LL_ERROR, "%s: warning: underrun occurred", codec_name);
		n = snd_pcm_recover(state->dev, n, 1);
		if (n < 0) {
			LOG_FMT(LL_ERROR, "%s: error: write failed", codec_name);
			return w;
		}
		else
			goto try_again;
	}
	w += n;
	if (w < frames) {
		buf += n * c->channels * state->enc_info->bytes;
		goto try_again;
	}
	return w;
}

ssize_t alsa_seek(struct codec *c, ssize_t pos)
{
	if (pos <= 0) {
		c->drop(c);
		return 0;
	}
	return -1;
}

ssize_t alsa_delay(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) == SND_PCM_STATE_PAUSED)
		return state->delay;
	snd_pcm_delay(state->dev, &state->delay);
	return state->delay;
}

void alsa_drop(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (c->write) {
		snd_pcm_drop(state->dev);
		state->delay = 0;
	}
}

void alsa_pause(struct codec *c, int p)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) != SND_PCM_STATE_PAUSED)
		snd_pcm_delay(state->dev, &state->delay);
	snd_pcm_pause(state->dev, p);
}

void alsa_destroy(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) == SND_PCM_STATE_RUNNING)
		snd_pcm_drain(state->dev);
	snd_pcm_close(state->dev);
	free(state);
}

static struct alsa_enc_info encodings[] = {
	{ "s16",    SND_PCM_FORMAT_S16,     2, 16, 1, write_buf_s16,    read_buf_s16 },
	{ "u8",     SND_PCM_FORMAT_U8,      1, 8,  1, write_buf_u8,     read_buf_u8 },
	{ "s8",     SND_PCM_FORMAT_S8,      1, 8,  1, write_buf_s8,     read_buf_s8 },
	{ "s24",    SND_PCM_FORMAT_S24,     4, 24, 1, write_buf_s24,    read_buf_s24 },
	{ "s24_3",  SND_PCM_FORMAT_S24_3LE, 3, 24, 1, write_buf_s24_3,  read_buf_s24_3 },
	{ "s32",    SND_PCM_FORMAT_S32,     4, 32, 1, write_buf_s32,    read_buf_s32 },
	{ "float",  SND_PCM_FORMAT_FLOAT,   4, 24, 0, write_buf_float,  read_buf_float },
	{ "double", SND_PCM_FORMAT_FLOAT64, 8, 53, 0, write_buf_double, read_buf_double },
};

static struct alsa_enc_info * alsa_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

struct codec * alsa_codec_init(const struct codec_params *p)
{
	int err;
	snd_pcm_t *dev = NULL;
	snd_pcm_hw_params_t *hw_p = NULL;
	snd_pcm_sw_params_t *sw_p = NULL;
	struct codec *c = NULL;
	struct alsa_state *state = NULL;
	snd_pcm_uframes_t buf_frames_min, buf_frames_max, buf_frames;
	struct alsa_enc_info *enc_info;

	if ((err = snd_pcm_open(&dev, p->path, (p->mode == CODEC_MODE_WRITE) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open device: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((enc_info = alsa_get_enc_info(p->enc)) == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: bad encoding: %s", codec_name, p->enc);
		goto fail;
	}

	if ((err = snd_pcm_hw_params_malloc(&hw_p)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to allocate hw params: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_any(dev, hw_p)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to initialize hw params: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_access(dev, hw_p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set access: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_format(dev, hw_p, enc_info->fmt)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set format: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_rate(dev, hw_p, (unsigned int) p->fs, 0)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set rate: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels(dev, hw_p, p->channels)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set channels: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_get_buffer_size_min(hw_p, &buf_frames_min)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to get minimum buffer size: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_get_buffer_size_max(hw_p, &buf_frames_max)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to get maximum buffer size: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	buf_frames = MINIMUM(MAXIMUM(p->block_frames, buf_frames_min), buf_frames_max);
	if ((err = snd_pcm_hw_params_set_buffer_size_min(dev, hw_p, &buf_frames)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set minimum buffer size: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	buf_frames = MINIMUM(MAXIMUM(p->block_frames * p->buf_ratio, buf_frames), buf_frames_max);
	if ((err = snd_pcm_hw_params_set_buffer_size_near(dev, hw_p, &buf_frames)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set buffer size: %s", codec_name, snd_strerror(err));
		goto fail;
	}
	LOG_FMT(LL_VERBOSE, "%s: info: buffer size: %lu frames [%lu %lu]", codec_name, buf_frames, buf_frames_min, buf_frames_max);
	if ((err = snd_pcm_hw_params(dev, hw_p)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to set hw params: %s", codec_name, snd_strerror(err));
		goto fail;
	}

	if (p->mode == CODEC_MODE_WRITE) {
		if ((err = snd_pcm_sw_params_malloc(&sw_p)) < 0) {
			LOG_FMT(LL_ERROR, "%s: error: failed to allocate sw params: %s", codec_name, snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_sw_params_current(dev, sw_p)) < 0) {
			LOG_FMT(LL_ERROR, "%s: error: failed to get current sw params: %s", codec_name, snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_sw_params_set_start_threshold(dev, sw_p, MINIMUM(p->block_frames * 2, buf_frames))) < 0) {
			LOG_FMT(LL_ERROR, "%s: error: failed to set start threshold: %s", codec_name, snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_sw_params(dev, sw_p)) < 0) {
			LOG_FMT(LL_ERROR, "%s: error: failed to set sw params: %s", codec_name, snd_strerror(err));
			goto fail;
		}
	}

	if (hw_p) snd_pcm_hw_params_free(hw_p);
	if (sw_p) snd_pcm_sw_params_free(sw_p);

	#if DUMP_PCM_INFO
		snd_output_t *output = NULL;
		if (snd_output_stdio_attach(&output, stderr, 0) == 0)
			snd_pcm_dump(dev, output);
	#endif

	state = calloc(1, sizeof(struct alsa_state));
	state->dev = dev;
	state->enc_info = enc_info;
	state->delay = 0;

	c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = p->type;
	c->enc = enc_info->name;
	c->fs = p->fs;
	c->channels = p->channels;
	c->prec = enc_info->prec;
	if (enc_info->can_dither) c->hints |= CODEC_HINT_CAN_DITHER;
	if (p->mode == CODEC_MODE_WRITE) c->hints |= CODEC_HINT_INTERACTIVE;
	c->buf_ratio = buf_frames / p->block_frames;
	c->frames = -1;
	if (p->mode == CODEC_MODE_READ) c->read = alsa_read;
	else c->write = alsa_write;
	c->seek = alsa_seek;
	c->delay = alsa_delay;
	c->drop = alsa_drop;
	c->pause = alsa_pause;
	c->destroy = alsa_destroy;
	c->data = state;

	return c;

	fail:
	if (hw_p) snd_pcm_hw_params_free(hw_p);
	if (sw_p) snd_pcm_sw_params_free(sw_p);
	if (dev) snd_pcm_close(dev);
	return NULL;
}

void alsa_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
