/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2024 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_CODEC_H
#define DSP_CODEC_H

#include "dsp.h"

enum {
	CODEC_MODE_READ  = 1<<0,
	CODEC_MODE_WRITE = 1<<1,
};

enum {
	CODEC_ENDIAN_DEFAULT,
	CODEC_ENDIAN_BIG,
	CODEC_ENDIAN_LITTLE,
	CODEC_ENDIAN_NATIVE,
};

enum {
	CODEC_HINT_INTERACTIVE = 1<<0,
	CODEC_HINT_CAN_DITHER  = 1<<1,
	CODEC_HINT_NO_OUT_BUF  = 1<<2,
};

struct codec {
	struct codec *next;
	const char *path, *type, *enc;
	int fs, channels, prec, hints, buf_ratio;
	ssize_t frames;
	ssize_t (*read)(struct codec *, sample_t *, ssize_t);   /* should be NULL if mode == CODEC_MODE_WRITE */
	ssize_t (*write)(struct codec *, sample_t *, ssize_t);  /* should be NULL if mode == CODEC_MODE_READ */
	ssize_t (*seek)(struct codec *, ssize_t);
	ssize_t (*delay)(struct codec *);
	void (*drop)(struct codec *);  /* drop pending frames */
	void (*pause)(struct codec *, int);
	void (*destroy)(struct codec *);
	void *data;
};

struct codec_list {
	struct codec *head;
	struct codec *tail;
};

#define CODEC_LIST_INITIALIZER ((struct codec_list) { NULL, NULL })

struct codec_params {
	const char *path, *type, *enc;
	int fs, channels, endian, mode, block_frames, buf_ratio;
};

#define CODEC_PARAMS_AUTO(path_arg, mode_arg) { \
	.path = path_arg, \
	.endian = CODEC_ENDIAN_DEFAULT, \
	.mode = mode_arg, \
	.block_frames = DEFAULT_BLOCK_FRAMES, \
	.buf_ratio = DEFAULT_BUF_RATIO, \
}

#define CODEC_DEFAULT_DEVICE "default"

struct codec * init_codec(const struct codec_params *);
void destroy_codec(struct codec *);
void append_codec(struct codec_list *, struct codec *);
void destroy_codec_list_head(struct codec_list *);
void destroy_codec_list(struct codec_list *);
void print_all_codecs(void);

#endif
