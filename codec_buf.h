/*
 * This file is part of dsp.
 *
 * Copyright (c) 2024 Michael Barbour <barbour.michael.0@gmail.com>
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
	void *data;
};

void codec_write_buf_cmd_push(void *, enum codec_write_buf_cmd);
void codec_write_buf_push(void *, sample_t *, ssize_t, void (*)(int));
ssize_t codec_write_buf_delay_nw(struct codec_write_buf *);
void codec_write_buf_destroy_nw(struct codec_write_buf *);

/* Public API */

enum {
	CODEC_BUF_ERROR_SHORT_WRITE = 1,
};

struct codec_write_buf * codec_write_buf_init(struct codec *, int, int);

static inline void codec_write_buf_write(struct codec_write_buf *wb, sample_t *data, ssize_t frames, void (*error_cb)(int))
{
	if (wb->data) codec_write_buf_push(wb->data, data, frames, error_cb);
	else {
		if (wb->codec->write(wb->codec, data, frames) != frames)
			error_cb(CODEC_BUF_ERROR_SHORT_WRITE);
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
