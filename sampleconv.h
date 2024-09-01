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

#ifndef DSP_SAMPLECONV_H
#define DSP_SAMPLECONV_H

#include <stdint.h>
#include <math.h>
#include "dsp.h"

/* ### NOTE ###
 * The read_buf_<fmt> and write_buf_<fmt> functions will work properly when
 * dest and src are the same buffer provided
 * sizeof(sample_t) >= sizeof(fmt). Since sample_t is currently a double,
 * this is true for all supported formats.
*/

#define S24_SIGN_EXTEND(x) ((x & 0x800000) ? x | ~0x7fffff : x)

#if BIT_PERFECT
	/* these clip, but are bit perfect */
	#define SAMPLE_TO_U8(x)     ((uint8_t) ((x * 128.0 + 127.0 <= -0.5) ? 0 : lround(x * 128.0 + 127.0)))
	#define U8_TO_SAMPLE(x)     (((sample_t) x - 127.0) / 128.0)
	#define SAMPLE_TO_S8(x)     ((int8_t) ((x * 128.0 >= 127.5) ? 127 : lround(x * 128.0)))
	#define S8_TO_SAMPLE(x)     ((sample_t) x / 128.0)
	#define SAMPLE_TO_S16(x)    ((int16_t) ((x * 32768.0 >= 32767.5) ? 32767 : lround(x * 32768.0)))
	#define S16_TO_SAMPLE(x)    ((sample_t) x / 32768.0)
	#define SAMPLE_TO_S24(x)    ((int32_t) ((x * 8388608.0 >= 8388607.5) ? 8388607 : lround(x * 8388608.0)))
	#define S24_TO_SAMPLE(x)    ((sample_t) S24_SIGN_EXTEND(x) / 8388608.0)
	#define SAMPLE_TO_S32(x)    ((int32_t) ((x * 2147483648.0 >= 2147483647.5) ? 2147483647 : lround(x * 2147483648.0)))
	#define S32_TO_SAMPLE(x)    ((sample_t) x / 2147483648.0)
#else
	/* these do not clip, but are also not bit perfect */
	#define SAMPLE_TO_U8(x)     ((uint8_t) lround(x * 127.0 + 127.0))
	#define U8_TO_SAMPLE(x)     (((sample_t) x - 127.0) / 128.0)
	#define SAMPLE_TO_S8(x)     ((int8_t) lround(x * 127.0))
	#define S8_TO_SAMPLE(x)     ((sample_t) x / 128.0)
	#define SAMPLE_TO_S16(x)    ((int16_t) lround(x * 32767.0))
	#define S16_TO_SAMPLE(x)    ((sample_t) x / 32768.0)
	#define SAMPLE_TO_S24(x)    ((int32_t) lround(x * 8388607.0))
	#define S24_TO_SAMPLE(x)    ((sample_t) S24_SIGN_EXTEND(x) / 8388608.0)
	#define SAMPLE_TO_S32(x)    ((int32_t) lround(x * 2147483647.0))
	#define S32_TO_SAMPLE(x)    ((sample_t) x / 2147483648.0)
#endif

#define SAMPLE_TO_FLOAT(x)  ((float) x)
#define FLOAT_TO_SAMPLE(x)  ((sample_t) x)
#define SAMPLE_TO_DOUBLE(x) ((double) x)
#define DOUBLE_TO_SAMPLE(x) ((sample_t) x)

void write_buf_u8(sample_t *, void *, ssize_t);
void read_buf_u8(void *, sample_t *, ssize_t);
void write_buf_s8(sample_t *, void *, ssize_t);
void read_buf_s8(void *, sample_t *, ssize_t);
void write_buf_s16(sample_t *, void *, ssize_t);
void read_buf_s16(void *, sample_t *, ssize_t);
void write_buf_s24(sample_t *, void *, ssize_t);
void read_buf_s24(void *, sample_t *, ssize_t);
void write_buf_s32(sample_t *, void *, ssize_t);
void read_buf_s32(void *, sample_t *, ssize_t);
void write_buf_s24_3(sample_t *, void *, ssize_t);
void read_buf_s24_3(void *, sample_t *, ssize_t);
void write_buf_float(sample_t *, void *, ssize_t);
void read_buf_float(void *, sample_t *, ssize_t);
void write_buf_double(sample_t *, void *, ssize_t);
void read_buf_double(void *, sample_t *, ssize_t);

#endif
