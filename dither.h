/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_DITHER_H
#define DSP_DITHER_H

#include "dsp.h"
#include "effect.h"

int effect_is_dither(const struct effect *);
void dither_effect_set_params(struct effect *, int, int);
struct effect * dither_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#define DITHER_EFFECT_INFO \
	{ "dither", "[shape] [[quantize_bits] bits]", dither_effect_init, 0 }

#endif
