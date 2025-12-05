/*
 * This file is part of dsp.
 *
 * Copyright (c) 2016-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_ZITA_CONVOLVER_H
#define DSP_ZITA_CONVOLVER_H

#ifdef HAVE_ZITA_CONVOLVER
#ifdef __cplusplus
extern "C" {
#endif

#include "dsp.h"
#include "effect.h"

struct effect * zita_convolver_effect_init_with_filter(const struct effect_info *, const struct stream_info *, const char *, sample_t *, int, ssize_t, int, int);
struct effect * zita_convolver_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#ifdef __cplusplus
}
#endif
#define ZITA_CONVOLVER_EFFECT_INFO \
	{ \
		"zita_convolver", \
		"[input_options] [min_part_len [max_part_len]] [file:][~/]filter_path|coefs:list[/list...]", \
		zita_convolver_effect_init, 0 \
	}
#else
#define ZITA_CONVOLVER_EFFECT_INFO \
	{ "zita_convolver", NULL, NULL, 0 }
#endif

#endif
