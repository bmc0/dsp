/*
 * This file is part of dsp.
 *
 * Copyright (c) 2018-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_ST2MS_H
#define DSP_ST2MS_H

#include "dsp.h"
#include "effect.h"

enum {
	ST2MS_EFFECT_NUMBER_ST2MS = 1,
	ST2MS_EFFECT_NUMBER_MS2ST,
};

struct effect * st2ms_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#define ST2MS_EFFECT_INFO \
	{ "st2ms", "", st2ms_effect_init, ST2MS_EFFECT_NUMBER_ST2MS }, \
	{ "ms2st", "", st2ms_effect_init, ST2MS_EFFECT_NUMBER_MS2ST }

#endif
