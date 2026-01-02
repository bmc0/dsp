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

#ifndef DSP_FIR_H
#define DSP_FIR_H

#ifdef HAVE_FFTW3
#include "dsp.h"
#include "effect.h"
#include "fir_util.h"

struct effect * fir_effect_init_with_filter(const struct effect_info *, const struct stream_info *, const char *, sample_t *, int, ssize_t, int);
struct effect * fir_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#define FIR_EFFECT_INFO \
	{ "fir", FIR_USAGE_OPTS " " FIR_USAGE_FILTER, fir_effect_init, 0 }
#else
#define FIR_EFFECT_INFO \
	{ "fir", NULL, NULL, 0 }
#endif

#endif
