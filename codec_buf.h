/*
 * This file is part of dsp.
 *
 * Copyright (c) 2024-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "util.h"

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
	struct read_buf_input_list *inputs;
	struct read_buf_input *cur_input;
	void (*error_cb)(int);
	void *data;
	ssize_t pos;
	int repeats;
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
ssize_t codec_read_buf_pull(void *, sample_t *, ssize_t, const struct read_buf_input *, ssize_t *, int *, int *);
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

struct read_buf_input {
	struct read_buf_input *prev, *next;
	struct codec *codec;
	ssize_t start, end;
	int repeats;
};

struct read_buf_input_list {
	struct read_buf_input *head, *tail;
};

#define READ_BUF_INPUT_LIST_INITIALIZER {0}
#define READ_BUF_INPUT_END_UNSPECIFIED -1
#define READ_BUF_INPUT_REPEAT_INF      -1

struct read_buf_input * read_buf_input_list_add(struct read_buf_input_list *, struct codec *, ssize_t, ssize_t, int);
void read_buf_input_list_destroy_head(struct read_buf_input_list *);
void read_buf_input_list_destroy(struct read_buf_input_list *);
struct codec_read_buf * codec_read_buf_init(struct read_buf_input_list *, int, int, void (*)(int));

static inline ssize_t codec_read_buf_read(struct codec_read_buf *rb, sample_t *data, ssize_t frames)
{
	struct read_buf_input *input = rb->cur_input;
	if (input == NULL || frames <= 0 || rb->next) return 0;
	ssize_t r = 0;
	if (rb->data) r = codec_read_buf_pull(rb->data, data, frames, input, &rb->pos, &rb->repeats, &rb->next);
	else {
		read_restart:
		r = frames;
		if (input->end >= 0) r = MINIMUM(r, input->end-rb->pos);
		if (r > 0) r = input->codec->read(input->codec, data, r);
		if (r <= 0 && rb->repeats != 0) {
			--rb->repeats;
			ssize_t seek_pos = input->codec->seek(input->codec, input->start);
			if (seek_pos >= 0) {
				rb->pos = seek_pos;
				goto read_restart;
			}
			else LOG_FMT(LL_ERROR, "%s(): error: seek failed; can't do repeat", __func__);
		}
		if (r <= 0) rb->next = 1;
		rb->pos += MAXIMUM(r, 0);
	}
	return r;
}

static inline ssize_t codec_read_buf_get_pos(struct codec_read_buf *rb)
{
	return rb->pos;
}

static inline int codec_read_buf_get_repeats(struct codec_read_buf *rb)
{
	return rb->repeats;
}

static inline ssize_t codec_read_buf_delay(struct codec_read_buf *rb)
{
	struct read_buf_input *input = rb->cur_input;
	if (input == NULL || rb->next) return 0;
	if (rb->data) return codec_read_buf_delay_nw(rb);
	return input->codec->delay(input->codec);
}

static inline ssize_t codec_read_buf_seek(struct codec_read_buf *rb, ssize_t pos)
{
	struct read_buf_input *input = rb->cur_input;
	if (input == NULL || rb->next) return -1;
	pos = MAXIMUM(pos, input->start);
	if (input->end >= 0) pos = MINIMUM(pos, input->end);
	if (rb->data) {
		/* assume no seeking to pos > 0 for real-time codecs */
		if (pos > 0 && (input->codec->hints & CODEC_HINT_REALTIME)) return -1;
		return codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SEEK, pos);
	}
	return input->codec->seek(input->codec, pos);
}

static inline void codec_read_buf_pause(struct codec_read_buf *rb, int pause_state, int sync)
{
	struct read_buf_input *input = rb->cur_input;
	if (input == NULL) return;
	if (rb->data) {
		codec_read_buf_cmd_push(rb->data, (pause_state) ? CODEC_READ_BUF_CMD_PAUSE : CODEC_READ_BUF_CMD_UNPAUSE, 0);
		if (sync) codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SYNC, 0);
	}
	else input->codec->pause(input->codec, pause_state);
}

static inline struct read_buf_input * codec_read_buf_next(struct codec_read_buf *rb)
{
	if (rb->cur_input == NULL) return NULL;
	if (rb->data && !rb->next) codec_read_buf_cmd_push(rb->data, CODEC_READ_BUF_CMD_SKIP, 0);
	rb->cur_input = rb->cur_input->next;
	rb->next = 0;
	rb->pos = (rb->cur_input) ? rb->cur_input->start : 0;
	rb->repeats = (rb->cur_input) ? rb->cur_input->repeats : 0;
	return rb->cur_input;
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
