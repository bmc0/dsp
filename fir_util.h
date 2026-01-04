/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_FIR_UTIL_H
#define DSP_FIR_UTIL_H

#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "util.h"

#define FIR_DEFAULT_OPTSTR "a::t:e:BLNr:c:"
#define FIR_USAGE_OPTS   "[-a[offset[s|m|S]]] [input_options]"
#define FIR_USAGE_FILTER "[file:][~/]filter_path|coefs:list[/list...]"

struct fir_config {
	int do_align;
	ssize_t offset;
	struct codec_params p;
};

sample_t * fir_read_filter(const struct effect_info *, const struct stream_info *, const char *, const struct codec_params *, int *, ssize_t *);
int fir_parse_opts(const struct effect_info *, const struct stream_info *, struct fir_config *, struct dsp_getopt_state *, int, const char *const *, const char *,
	int (*)(const struct effect_info *, const struct stream_info *, const struct fir_config *, int, const char *, void *), void *);
struct effect * fir_init_align(const struct effect_info *, const struct stream_info *, const char *,
	const struct fir_config *, const sample_t *, int, ssize_t);

#endif
