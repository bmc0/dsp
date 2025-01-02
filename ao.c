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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ao/ao.h>
#include "ao.h"
#include "util.h"
#include "sampleconv.h"

struct ao_state {
	ao_device *dev;
	struct ao_enc_info *enc_info;
};

struct ao_enc_info {
	const char *name;
	int bytes, prec;
	void (*write_func)(sample_t *, void *, ssize_t);
};

static const char codec_name[] = "ao";
static int ao_open_count = 0;

static struct ao_enc_info encodings[] = {
	{ "s16", 2, 16, write_buf_s16 },
	{ "u8",  1, 8,  write_buf_u8 },
	{ "s32", 4, 32, write_buf_s32 },
};

static struct ao_enc_info * ao_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

ssize_t ao_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct ao_state *state = (struct ao_state *) c->data;

	state->enc_info->write_func(buf, buf, frames * c->channels);
	if (ao_play(state->dev, (char *) buf, frames * c->channels * state->enc_info->bytes) == 0) {
		LOG_FMT(LL_ERROR, "%s: ao_play(): write failed", codec_name);
		return 0;
	}
	return frames;
}

ssize_t ao_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t ao_delay(struct codec *c)
{
	return 0;
}

void ao_drop(struct codec *c)
{
	/* do nothing */
}

void ao_pause(struct codec *c, int p)
{
	/* do nothing */
}

void ao_destroy(struct codec *c)
{
	struct ao_state *state = (struct ao_state *) c->data;
	ao_close(state->dev);
	--ao_open_count;
	if (ao_open_count == 0)
		ao_shutdown();
	free(state);
}

struct codec * ao_codec_init(const struct codec_params *p)
{
	int driver;
	struct ao_enc_info *enc_info;
	struct ao_sample_format format;
	ao_device *dev = NULL;
	struct ao_state *state = NULL;
	struct ao_option *opts = NULL;
	struct codec *c = NULL;
	char buf_time_str[12];

	if (ao_open_count == 0)
		ao_initialize();
	if ((driver = ao_default_driver_id()) == -1) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed get default driver id", codec_name);
		goto fail;
	}
	if ((enc_info = ao_get_enc_info(p->enc)) == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: bad encoding: %s", codec_name, p->enc);
		goto fail;
	}
	format.bits = enc_info->prec;
	format.rate = p->fs;
	format.channels = p->channels;
	format.byte_format = AO_FMT_NATIVE;
	format.matrix = NULL;
	const double buf_time = (1000.0 / p->fs) * p->buf_ratio * p->block_frames;
	snprintf(buf_time_str, sizeof(buf_time_str), "%.2f", buf_time);
	if (LOGLEVEL(LL_VERBOSE))
		ao_append_option(&opts, "verbose", "");
	if (strcmp(p->path, CODEC_DEFAULT_DEVICE) != 0)
		ao_append_option(&opts, "dev", p->path);
	ao_append_option(&opts, "client_name", dsp_globals.prog_name);
	ao_append_option(&opts, "buffer_time", buf_time_str);
	if ((dev = ao_open_live(driver, &format, opts)) == NULL) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: could not open device", codec_name);
		goto fail;
	}
	ao_free_options(opts);

	state = calloc(1, sizeof(struct ao_state));
	state->dev = dev;
	state->enc_info = enc_info;

	c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = p->type;
	c->enc = enc_info->name;
	c->fs = p->fs;
	c->channels = p->channels;
	c->prec = enc_info->prec;
	c->hints |= CODEC_HINT_CAN_DITHER;  /* all formats are fixed-point LPCM */
	c->hints |= CODEC_HINT_INTERACTIVE;
	c->hints |= CODEC_HINT_REALTIME;
	c->buf_ratio = p->buf_ratio;
	c->frames = -1;
	c->write = ao_write;
	c->seek = ao_seek;
	c->delay = ao_delay;
	c->drop = ao_drop;
	c->pause = ao_pause;
	c->destroy = ao_destroy;
	c->data = state;

	++ao_open_count;

	return c;

	fail:
	ao_free_options(opts);
	if (ao_open_count == 0)
		ao_shutdown();
	return NULL;
}

void ao_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
