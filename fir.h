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

#ifndef DSP_FIR_H
#define DSP_FIR_H

#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "util.h"

struct effect * fir_effect_init_with_filter(const struct effect_info *, const struct stream_info *, const char *, sample_t *, int, ssize_t, int);
sample_t * fir_read_filter(const struct effect_info *, const char *, const struct codec_params *, int *, ssize_t *);
int fir_parse_codec_opts(const struct effect_info *, const struct stream_info *, struct codec_params *, struct dsp_getopt_state *, int, const char *const *);
struct effect * fir_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#endif
