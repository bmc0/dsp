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
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include "pcm.h"
#include "util.h"
#include "sampleconv.h"

struct pcm_state {
	int fd;
	const struct pcm_enc_info *enc_info;
	ssize_t pos;
};

struct pcm_enc_info {
	const char *name;
	int bytes, prec, can_dither;
	void (*read_func)(void *, sample_t *, ssize_t);
	void (*write_func)(sample_t *, void *, ssize_t);
};

static const struct pcm_enc_info encodings[] = {
	{ "s16",    2, 16, 1, read_buf_s16,    write_buf_s16 },
	{ "u8",     1, 8,  1, read_buf_u8,     write_buf_u8 },
	{ "s8",     1, 8,  1, read_buf_s8,     write_buf_s8 },
	{ "s24",    4, 24, 1, read_buf_s24,    write_buf_s24 },
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__  == __ORDER_LITTLE_ENDIAN__
	{ "s24_3",  3, 24, 1, read_buf_s24_3,  write_buf_s24_3 },
#endif
	{ "s32",    4, 32, 1, read_buf_s32,    write_buf_s32 },
	{ "float",  4, 24, 0, read_buf_float,  write_buf_float },
	{ "double", 8, 53, 0, read_buf_double, write_buf_double },
};

static const struct pcm_enc_info * pcm_get_enc_info(const char *enc)
{
	if (enc == NULL)
		return &encodings[0];
	for (int i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

ssize_t pcm_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct pcm_state *state = (struct pcm_state *) c->data;

	ssize_t n = read(state->fd, buf, frames * c->channels * state->enc_info->bytes);
	if (n == -1) {
		LOG_FMT(LL_ERROR, "%s: read failed: %s", __func__, strerror(errno));
		return 0;
	}
	n = n / state->enc_info->bytes / c->channels;
	state->enc_info->read_func(buf, buf, n * c->channels);
	state->pos += n;
	return n;
}

ssize_t pcm_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct pcm_state *state = (struct pcm_state *) c->data;

	state->enc_info->write_func(buf, buf, frames * c->channels);
	ssize_t n = write(state->fd, buf, frames * c->channels * state->enc_info->bytes);
	if (n == -1) {
		LOG_FMT(LL_ERROR, "%s: write failed: %s", __func__, strerror(errno));
		return 0;
	}
	n = n / state->enc_info->bytes / c->channels;
	state->pos += n;
	return n;
}

#ifdef __BYTE_ORDER__
struct wav_header {
	uint8_t riff[4];
	uint32_t filesize;
	uint8_t wave[4], fmt[4];
	uint32_t chunksize;
	uint16_t audioformat;
	uint16_t channels;
	uint32_t fs;
	uint32_t bytes_sec;
	uint16_t bytes_frame;
	uint16_t bits_sample;
	uint8_t chunk_id[4];
	uint32_t datasize;
};

ssize_t wavpipe_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct pcm_state *state = (struct pcm_state *) c->data;
	struct wav_header h = {0};
#if __BYTE_ORDER__  == __ORDER_LITTLE_ENDIAN__
	memcpy(h.riff,     "RIFF", 4);
#elif __BYTE_ORDER__  == __ORDER_BIG_ENDIAN__
	memcpy(h.riff,     "RIFX", 4);
#else
	#error "unknown byte order"
#endif
	memcpy(h.wave,     "WAVE", 4);
	memcpy(h.fmt,      "fmt ", 4);
	memcpy(h.chunk_id, "data", 4);
	h.filesize = 0xFFFFFFFFU;
	h.chunksize = 0x10;
	h.audioformat = (state->enc_info->can_dither) ? 0x1 : 0x3;
	h.channels = c->channels;
	h.fs = c->fs;
	h.bytes_frame = c->channels * state->enc_info->bytes;
	h.bytes_sec = h.bytes_frame * c->fs;
	h.bits_sample = state->enc_info->bytes * 8;
	h.datasize = 0xFFFFFFFFU;
	if (write(state->fd, &h, sizeof(h)) == -1) {
		LOG_FMT(LL_ERROR, "%s: write failed: %s", __func__, strerror(errno));
		return 0;
	}
	c->write = pcm_write;
	return pcm_write(c, buf, frames);
}

#define IS_WAVPIPE(type) (strcmp(type, "wavpipe") == 0)
#define WAVPIPE_BAD_ENC(enc_info) \
	((enc_info)->write_func == write_buf_s8 || (enc_info)->write_func == write_buf_s24)
static const struct pcm_enc_info * wavpipe_get_enc_info(const char *enc)
{
	const struct pcm_enc_info *enc_info = pcm_get_enc_info(enc);
	if (enc_info && WAVPIPE_BAD_ENC(enc_info)) return NULL;
	return enc_info;
}

#endif

ssize_t pcm_seek(struct codec *c, ssize_t pos)
{
	struct pcm_state *state = (struct pcm_state *) c->data;
	if (c->frames == -1)
		return -1;
	if (pos < 0)
		pos = 0;
	else if (pos > c->frames)
		pos = c->frames;
	off_t o = lseek(state->fd, pos * state->enc_info->bytes * c->channels, SEEK_SET);
	if (o == -1)
		return -1;
	state->pos = o;
	return o / state->enc_info->bytes / c->channels;
}

ssize_t pcm_delay(struct codec *c)
{
	return 0;
}

void pcm_drop(struct codec *c)
{
	/* do nothing */
}

void pcm_pause(struct codec *c, int p)
{
	/* do nothing */
}

void pcm_destroy(struct codec *c)
{
	struct pcm_state *state = (struct pcm_state *) c->data;
	close(state->fd);
	free(state);
}

struct codec * pcm_codec_init(const struct codec_params *p)
{
	int fd = -1;
#ifdef __BYTE_ORDER__
	const int is_wavpipe = IS_WAVPIPE(p->type);
	const struct pcm_enc_info *enc_info = (is_wavpipe)
		? wavpipe_get_enc_info(p->enc)
		: pcm_get_enc_info(p->enc);
#else
	const struct pcm_enc_info *enc_info = pcm_get_enc_info(p->enc);
#endif
	if (enc_info == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: bad encoding: %s", p->type, p->enc);
		goto fail;
	}
	if (!(p->endian == CODEC_ENDIAN_DEFAULT || p->endian == CODEC_ENDIAN_NATIVE)) {
		LOG_FMT(LL_ERROR, "%s: error: endian conversion not supported", p->type);
		goto fail;
	}
	if (strcmp(p->path, "-") == 0)
		fd = (p->mode == CODEC_MODE_WRITE) ? STDOUT_FILENO : STDIN_FILENO;
	else if ((fd = open(p->path, (p->mode == CODEC_MODE_WRITE) ? O_WRONLY|O_CREAT|O_TRUNC : O_RDONLY, 0644)) == -1) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open file: %s: %s", p->type, p->path, strerror(errno));
		goto fail;
	}

	struct pcm_state *state = calloc(1, sizeof(struct pcm_state));
	state->fd = fd;
	state->enc_info = enc_info;

	struct codec *c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = p->type;
	c->enc = enc_info->name;
	c->fs = p->fs;
	c->channels = p->channels;
	c->prec = enc_info->prec;
	if (enc_info->can_dither) c->hints |= CODEC_HINT_CAN_DITHER;
	c->frames = -1;
	if (p->mode == CODEC_MODE_READ) {
		off_t size = lseek(fd, 0, SEEK_END);
		c->frames = (size == -1) ? -1 : size / enc_info->bytes / p->channels;
		lseek(fd, 0, SEEK_SET);
	}
	if (p->mode == CODEC_MODE_READ) c->read = pcm_read;
#ifdef __BYTE_ORDER__
	else if (is_wavpipe) c->write = wavpipe_write;
#endif
	else c->write = pcm_write;
	c->seek = pcm_seek;
	c->delay = pcm_delay;
	c->drop = pcm_drop;
	c->pause = pcm_pause;
	c->destroy = pcm_destroy;
	c->data = state;

	return c;

	fail:
	return NULL;
}

void pcm_codec_print_encodings(const char *type)
{
#ifdef __BYTE_ORDER__
	if (IS_WAVPIPE(type)) {
		for (int i = 0; i < LENGTH(encodings); ++i) {
			if (!WAVPIPE_BAD_ENC(&encodings[i]))
				fprintf(stdout, " %s", encodings[i].name);
		}
		return;
	}
#endif
	for (int i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
