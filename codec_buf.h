/*
 * This file is part of dsp.
 *
 * Copyright (c) 2024-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_CODEC_BUF_H
#define DSP_CODEC_BUF_H

#include "dsp.h"
#include "codec.h"

/* Internal use only */

enum codec_read_buf_cmd {
	CODEC_READ_BUF_CMD_SYNC = 1,  /* wait for previous command to be read (sync, obviously) */
	CODEC_READ_BUF_CMD_SEEK,      /* seek current input (sync, arg) */
	CODEC_READ_BUF_CMD_PAUSE,     /* pause input codec (async) */
	CODEC_READ_BUF_CMD_UNPAUSE,   /* unpause input codec (async) */
	CODEC_READ_BUF_CMD_SKIP,      /* skip to next input (sync) */
	CODEC_READ_BUF_CMD_TERM,      /* terminate (async) */
};

struct codec_read_buf {
	struct codec_list *codecs;
	struct codec *cur_codec;
	void (*error_cb)(int);
	void *data;
	int next;  /* at end of current input */
};

enum codec_write_buf_cmd {
	CODEC_WRITE_BUF_CMD_SYNC = 1,          /* wait for previous command to be read (sync, obviously) */
	CODEC_WRITE_BUF_CMD_DROP_BLOCK_QUEUE,  /* drop blocks in queue (async) */
	CODEC_WRITE_BUF_CMD_DROP_ALL,          /* as above, but drop output codec buffer as well (async) */
	CODEC_WRITE_BUF_CMD_PAUSE,             /* pause output codec (async) */
	CODEC_WRITE_BUF_CMD_UNPAUSE,           /* unpause output codec (async) */
	CODEC_WRITE_BUF_CMD_DRAIN,             /* if running, write pending blocks and stop (sync) */
	CODEC_WRITE_BUF_CMD_TERM,              /* terminate after stop (async) */
};

struct codec_write_buf {
	struct codec *codec;
	void (*error_cb)(int);
	void *data;
};

ssize_t codec_read_buf_cmd_push(void *, enum codec_read_buf_cmd, ssize_t);
ssize_t codec_read_buf_pull(void *, sample_t *, ssize_t, const struct codec *, int *);
ssize_t codec_read_buf_delay_nw(struct codec_read_buf *);
void codec_read_buf_destroy_nw(struct codec_read_buf *);

void codec_write_buf_cmd_push(void *, enum codec_write_buf_cmd);
void codec_write_buf_push(void *, sample_t *, ssize_t);
ssize_t codec_write_buf_delay_nw(struct codec_write_buf *);
void codec_write_buf_destroy_nw(struct codec_write_buf *);

/* Public API */

#define CODEC_BUF_MIN_BLOCKS 2

enum {
	CODEC_BUF_ERROR_SHORT_WRITE = 1,
	CODEC_BUF_ERROR_READ,
};

struct codec_read_buf * codec_read_buf_init(struct codec_list *, int, int, void (*)(int));

static inline ssize_t codec_read_buf_read(struct codec_read_buf *rb, sample_t *data, ssize_t frames)
{
	struct codec *codec = rb->cur_codec;
	if (codec == NULL || frames <= 0 || rb->next) return 0;
	ssize_t r = 0;
	if (rb->data) r = codec_read_buf_pull(rb->data, data, frames, codec, &rb->next);
	else {
		r = codec->read(codec, data, frames);
		if (r != frames) rb->next = 1;
	}
	return r;
}

static inline ssize_t codec_read_buf_delay(struct codec_read_buf *rb)
{
	struct codec *codec = rb->cur_codec;
	if (codec == NULL || rb->next) return 0;
	if (rb->data) return codec_read_buf_delay_nw(rb);
	return codec->delay(codec);
}

static inline ssize_t codec_read_buf_seek(struct codec_read_buf *rb, ssize_t pos)
{
	struct codec *codec = rb->cur_codec;
	if (codec == NULL || rb->next) return -1;
	if (rb->data) {
		/* assume no seeking to pos > 0 for real-time codecs */
		if (pos > 0 && (codec->hints & CODEC_HINT_REALTIME)) return -1;
		return codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SEEK, pos);
	}
	return codec->seek(codec, pos);
}

static inline void codec_read_buf_pause(struct codec_read_buf *rb, int pause_state, int sync)
{
	struct codec *codec = rb->cur_codec;
	if (codec == NULL) return;
	if (rb->data) {
		codec_read_buf_cmd_push(rb->data, (pause_state) ? CODEC_READ_BUF_CMD_PAUSE : CODEC_READ_BUF_CMD_UNPAUSE, 0);
		if (sync) codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SYNC, 0);
	}
	else codec->pause(codec, pause_state);
}

static inline struct codec * codec_read_buf_next(struct codec_read_buf *rb)
{
	if (rb->cur_codec == NULL) return NULL;
	if (rb->data && !rb->next) codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SKIP, 0);
	rb->cur_codec = rb->cur_codec->next;
	rb->next = 0;
	return rb->cur_codec;
}

static inline void codec_read_buf_destroy(struct codec_read_buf *rb)
{
	if (rb == NULL) return;
	if (rb->data) codec_read_buf_destroy_nw(rb);
	free(rb);
}

struct codec_write_buf * codec_write_buf_init(struct codec *, int, int, void (*)(int));

static inline void codec_write_buf_write(struct codec_write_buf *wb, sample_t *data, ssize_t frames)
{
	if (frames <= 0) return;
	if (wb->data) codec_write_buf_push(wb->data, data, frames);
	else {
		if (wb->codec->write(wb->codec, data, frames) != frames)
			wb->error_cb(CODEC_BUF_ERROR_SHORT_WRITE);
	}
}

static inline ssize_t codec_write_buf_delay(struct codec_write_buf *wb)
{
	if (wb->data) return codec_write_buf_delay_nw(wb);
	return wb->codec->delay(wb->codec);
}

static inline void codec_write_buf_drop(struct codec_write_buf *wb, int drop_all, int sync)
{
	if (wb->data) {
		codec_write_buf_cmd_push(wb->data, (drop_all) ? CODEC_WRITE_BUF_CMD_DROP_ALL : CODEC_WRITE_BUF_CMD_DROP_BLOCK_QUEUE);
		if (sync) codec_write_buf_cmd_push(wb->data, CODEC_WRITE_BUF_CMD_SYNC);
	}
	else wb->codec->drop(wb->codec);
}

static inline void codec_write_buf_pause(struct codec_write_buf *wb, int pause_state, int sync)
{
	if (wb->data) {
		codec_write_buf_cmd_push(wb->data, (pause_state) ? CODEC_WRITE_BUF_CMD_PAUSE : CODEC_WRITE_BUF_CMD_UNPAUSE);
		if (sync) codec_write_buf_cmd_push(wb->data, CODEC_WRITE_BUF_CMD_SYNC);
	}
	else wb->codec->pause(wb->codec, pause_state);
}

static inline void codec_write_buf_destroy(struct codec_write_buf *wb)
{
	if (wb == NULL) return;
	if (wb->data) codec_write_buf_destroy_nw(wb);
	free(wb);
}

#endif
