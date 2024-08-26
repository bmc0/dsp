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

enum {
	CODEC_BUF_ERROR_SHORT_WRITE = 1,
};

struct codec_write_buf {
	void (*write)(struct codec_write_buf *, sample_t *, ssize_t);
	ssize_t (*delay)(struct codec_write_buf *);
	void (*drop)(struct codec_write_buf *, int);
	void (*pause)(struct codec_write_buf *, int);
	void (*drain)(struct codec_write_buf *);
	void (*sync)(struct codec_write_buf *);
	void (*destroy)(struct codec_write_buf *);
	void *data;
};

struct codec_write_buf * codec_write_buf_init(struct codec *, int, int, void (*)(int));

#endif
