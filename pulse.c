/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <pulse/simple.h>
#include <pulse/error.h>
#include "pulse.h"
#include "util.h"
#include "sampleconv.h"

struct pulse_enc_info {
	const char *name;
	int fmt, bytes, prec, can_dither;
	void (*write_func)(sample_t *, void *, ssize_t);
	void (*read_func)(void *, sample_t *, ssize_t);
};

struct pulse_state {
	pa_simple *s;
	struct pulse_enc_info *enc_info;
};

static const char codec_name[] = "pulse";

ssize_t pulse_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	int err;
	struct pulse_state *state = (struct pulse_state *) c->data;

	if (pa_simple_read(state->s, buf, frames * c->channels * state->enc_info->bytes, &err) < 0) {
		LOG_FMT(LL_ERROR, "%s: read: error: %s", codec_name, pa_strerror(err));
		return 0;
	}
	state->enc_info->read_func(buf, buf, frames * c->channels);
	return frames;
}

ssize_t pulse_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	int err;
	struct pulse_state *state = (struct pulse_state *) c->data;

	state->enc_info->write_func(buf, buf, frames * c->channels);
	if (pa_simple_write(state->s, buf, frames * c->channels * state->enc_info->bytes, &err) < 0) {
		LOG_FMT(LL_ERROR, "%s: write: error: %s", codec_name, pa_strerror(err));
		return 0;
	}
	return frames;
}

ssize_t pulse_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t pulse_delay(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	return pa_simple_get_latency(state->s, NULL) * c->fs / 1000000;
}

void pulse_drop(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	pa_simple_flush(state->s, NULL);
}

void pulse_pause(struct codec *c, int p)
{
	/* do nothing */
}

void pulse_destroy(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	pa_simple_drain(state->s, NULL);
	pa_simple_free(state->s);
	free(state);
}

static struct pulse_enc_info encodings[] = {
	{ "s16",   PA_SAMPLE_S16NE,     2, 16, 1, write_buf_s16,   read_buf_s16 },
	{ "u8",    PA_SAMPLE_U8,        1,  8, 1, write_buf_u8,    read_buf_u8 },
	{ "s24",   PA_SAMPLE_S24_32NE,  4, 24, 1, write_buf_s24,   read_buf_s24 },
	{ "s24_3", PA_SAMPLE_S24LE,     3, 24, 1, write_buf_s24_3, read_buf_s24_3 },
	{ "s32",   PA_SAMPLE_S32NE,     4, 32, 1, write_buf_s32,   read_buf_s32 },
	{ "float", PA_SAMPLE_FLOAT32NE, 4, 24, 0, write_buf_float, read_buf_float },
};

static struct pulse_enc_info * pulse_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

struct codec * pulse_codec_init(const struct codec_params *p)
{
	int err;
	pa_simple *s;
	struct codec *c;
	struct pulse_state *state;
	struct pulse_enc_info *enc_info;

	if ((enc_info = pulse_get_enc_info(p->enc)) == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: bad encoding: %s", codec_name, p->enc);
		return NULL;
	}
	const pa_sample_spec ss = {
		.format = enc_info->fmt,
		.channels = p->channels,
		.rate = p->fs
	};
	const pa_buffer_attr buf_attr = {
		.maxlength = -1,
		.minreq = -1,
		.prebuf = -1,
		.tlength = (ssize_t) enc_info->bytes * p->channels * p->block_frames * p->buf_ratio,
		.fragsize = -1
	};
	s = pa_simple_new(NULL, dsp_globals.prog_name, (p->mode == CODEC_MODE_WRITE) ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD,
		(strcmp(p->path, CODEC_DEFAULT_DEVICE) == 0) ? NULL : p->path, dsp_globals.prog_name, &ss, NULL, &buf_attr, &err);
	if (s == NULL) {
		LOG_FMT(LL_OPEN_ERROR, "%s: failed to open device: %s", codec_name, pa_strerror(err));
		return NULL;
	}

	state = calloc(1, sizeof(struct pulse_state));
	state->s = s;
	state->enc_info = enc_info;

	c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = p->type;
	c->enc = enc_info->name;
	c->fs = p->fs;
	c->channels = p->channels;
	c->prec = enc_info->prec;
	if (enc_info->can_dither) c->hints |= CODEC_HINT_CAN_DITHER;
	if (p->mode == CODEC_MODE_WRITE) c->hints |= CODEC_HINT_INTERACTIVE;
	c->frames = -1;
	if (p->mode == CODEC_MODE_READ) c->read = pulse_read;
	else c->write = pulse_write;
	c->seek = pulse_seek;
	c->delay = pulse_delay;
	c->drop = pulse_drop;
	c->pause = pulse_pause;
	c->destroy = pulse_destroy;
	c->data = state;

	return c;
}

void pulse_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
