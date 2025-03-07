/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "biquad.h"
#include "util.h"

static double parse_width(const char *s, int *type, char **endptr)
{
	*type = BIQUAD_WIDTH_Q;
	double w = M_SQRT1_2;
	if (s[0] == 'b' && s[1] == 'w' && s[2] != '\0') {
		const char *s_ptr = s + 2;
		const int order = strtol(s_ptr, endptr, 10);
		if (*endptr == s_ptr || (**endptr != '\0' && **endptr != '.'))
			goto fail;  /* failed to parse order */
		if (order < 2) {
			LOG_FMT(LL_ERROR, "%s(): filter order must be >= 2", __func__);
			goto fail;
		}
		const int n_biquads = order / 2;
		int p_idx = 0;
		if (**endptr == '.') {
			s_ptr = *endptr + 1;
			p_idx = strtol(s_ptr, endptr, 10);
			if (*endptr == s_ptr || **endptr != '\0')
				goto fail;  /* failed to parse index */
			if (p_idx < 0 || p_idx >= n_biquads) {
				LOG_FMT(LL_ERROR, "%s(): filter index out of range", __func__);
				goto fail;
			}
		}
		p_idx = n_biquads - p_idx;  /* index from outermost conjugate pair */
		w = 1.0/(2.0*sin(M_PI/order*(p_idx-0.5)));
	}
	else {
		w = strtod(s, endptr);
		if (*endptr != s) {
			switch(**endptr) {
			case 'q':
				*type = BIQUAD_WIDTH_Q;
				++(*endptr);
				break;
			case 's':
				*type = BIQUAD_WIDTH_SLOPE;
				++(*endptr);
				break;
			case 'd':
				*type = BIQUAD_WIDTH_SLOPE_DB;
				++(*endptr);
				break;
			case 'o':
				*type = BIQUAD_WIDTH_BW_OCT;
				++(*endptr);
				break;
			case 'k':
				w *= 1000.0;
			case 'h':
				*type = BIQUAD_WIDTH_BW_HZ;
				++(*endptr);
				break;
			}
			if (**endptr != '\0') LOG_FMT(LL_ERROR, "%s(): trailing characters: %s", __func__, *endptr);
		}
	}
	return w;
	fail:
	*endptr = (char *) s;
	return w;
}

void biquad_init(struct biquad_state *state, double b0, double b1, double b2, double a0, double a1, double a2)
{
	state->c0 = b0 / a0;
	state->c1 = b1 / a0;
	state->c2 = b2 / a0;
	state->c3 = a1 / a0;
	state->c4 = a2 / a0;
	biquad_reset(state);
}

void biquad_reset(struct biquad_state *state)
{
#if BIQUAD_USE_TDF_2
	state->m0 = state->m1 = 0.0;
#else
	state->i0 = state->i1 = 0.0;
	state->o0 = state->o1 = 0.0;
#endif
}

void biquad_init_using_type(struct biquad_state *b, int type, double fs, double arg0, double arg1, double arg2, double arg3, int width_type)
{
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
	if (type == BIQUAD_LOWPASS_TRANSFORM || type == BIQUAD_HIGHPASS_TRANSFORM) {
		const double fz = arg0, qz = arg1;
		const double fp = arg2, qp = arg3;

		const double w0z = 2*M_PI*fz / fs, w0p = 2*M_PI*fp / fs;
		const double cos_w0z = cos(w0z), cos_w0p = cos(w0p);
		const double alpha_z = sin(w0z) / (2.0*qz), alpha_p = sin(w0p) / (2.0*qp);
		const double kz = (type == BIQUAD_LOWPASS_TRANSFORM) ? 2.0/(1.0-cos_w0z) : 2.0/(1.0+cos_w0z);
		const double kp = (type == BIQUAD_LOWPASS_TRANSFORM) ? 2.0/(1.0-cos_w0p) : 2.0/(1.0+cos_w0p);

		b0 = (1.0 + alpha_z)*kz;
		b1 = (-2.0 * cos_w0z)*kz;
		b2 = (1.0 - alpha_z)*kz;
		a0 = (1.0 + alpha_p)*kp;
		a1 = (-2.0 * cos_w0p)*kp;
		a2 = (1.0 - alpha_p)*kp;
	}
	else {
		double alpha, c, f0 = arg0, width = arg1;
		const double gain = arg2;

		if (width_type == BIQUAD_WIDTH_SLOPE_DB) {
			width_type = BIQUAD_WIDTH_SLOPE;
			width /= 12.0;
			if (type == BIQUAD_LOWSHELF)
				f0 *= pow(10.0, fabs(gain) / 80.0 / width);
			else if (type == BIQUAD_HIGHSHELF)
				f0 /= pow(10.0, fabs(gain) / 80.0 / width);
		}

		const double a = pow(10.0, gain / 40.0);
		const double w0 = 2*M_PI*f0 / fs;
		const double sin_w0 = sin(w0), cos_w0 = cos(w0);

		switch (width_type) {
		case BIQUAD_WIDTH_SLOPE:
			alpha = sin_w0/2.0 * sqrt((a + 1.0/a) * (1.0/width - 1.0) + 2.0);
			break;
		case BIQUAD_WIDTH_BW_OCT:
			alpha = sin_w0 * sinh(M_LN2/2 * width * w0 / sin_w0);
			break;
		case BIQUAD_WIDTH_BW_HZ:
			alpha = sin_w0 / (2.0 * f0 / width);
			break;
		case BIQUAD_WIDTH_Q:
		default:
			alpha = sin_w0 / (2.0 * width);
		}

		switch (type) {
		case BIQUAD_LOWPASS_1:
			c = 1.0 + cos_w0;
			b0 = sin_w0;
			b1 = sin_w0;
			b2 = 0.0;
			a0 = sin_w0 + c;
			a1 = sin_w0 - c;
			a2 = 0.0;
			break;
		case BIQUAD_HIGHPASS_1:
			c = 1.0 + cos_w0;
			b0 = c;
			b1 = -c;
			b2 = 0.0;
			a0 = sin_w0 + c;
			a1 = sin_w0 - c;
			a2 = 0.0;
			break;
		case BIQUAD_ALLPASS_1:
			c = 1.0 + cos_w0;
			b0 = sin_w0 - c;
			b1 = sin_w0 + c;
			b2 = 0.0;
			a0 = b1;
			a1 = b0;
			a2 = 0.0;
			break;
		case BIQUAD_LOWSHELF_1:
			c = 1.0 + cos_w0;
			b0 = a * sin_w0 + c;
			b1 = a * sin_w0 - c;
			b2 = 0.0;
			a0 = sin_w0 / a + c;
			a1 = sin_w0 / a - c;
			a2 = 0.0;
			break;
		case BIQUAD_HIGHSHELF_1:
			c = 1.0 + cos_w0;
			b0 = sin_w0 + c * a;
			b1 = sin_w0 - c * a;
			b2 = 0.0;
			a0 = sin_w0 + c / a;
			a1 = sin_w0 - c / a;
			a2 = 0.0;
			break;
		case BIQUAD_LOWPASS_1P:
			c = 1.0 - cos_w0;
			b0 = -c + sqrt(c*c + 2.0*c);
			b1 = b2 = 0.0;
			a0 = 1.0;
			a1 = -1.0 + b0;
			a2 = 0.0;
			break;
		case BIQUAD_LOWPASS:
			b0 = (1.0 - cos_w0) / 2.0;
			b1 = 1.0 - cos_w0;
			b2 = b0;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cos_w0;
			a2 = 1.0 - alpha;
			break;
		case BIQUAD_HIGHPASS:
			b0 = (1.0 + cos_w0) / 2.0;
			b1 = -(1.0 + cos_w0);
			b2 = b0;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cos_w0;
			a2 = 1.0 - alpha;
			break;
		case BIQUAD_BANDPASS_SKIRT:
			b0 = sin_w0 / 2.0;
			b1 = 0.0;
			b2 = -b0;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cos_w0;
			a2 = 1.0 - alpha;
			break;
		case BIQUAD_BANDPASS_PEAK:
			b0 = alpha;
			b1 = 0.0;
			b2 = -alpha;
			a0 = 1.0 + alpha;
			a1 = -2.0 * cos_w0;
			a2 = 1.0 - alpha;
			break;
		case BIQUAD_NOTCH:
			b0 = 1.0;
			b1 = -2.0 * cos_w0;
			b2 = 1.0;
			a0 = 1.0 + alpha;
			a1 = b1;
			a2 = 1.0 - alpha;
			break;
		case BIQUAD_ALLPASS:
			b0 = 1.0 - alpha;
			b1 = -2.0 * cos_w0;
			b2 = 1.0 + alpha;
			a0 = b2;
			a1 = b1;
			a2 = b0;
			break;
		case BIQUAD_PEAK:
			b0 = 1.0 + alpha * a;
			b1 = -2.0 * cos_w0;
			b2 = 1.0 - alpha * a;
			a0 = 1.0 + alpha / a;
			a1 = b1;
			a2 = 1.0 - alpha / a;
			break;
		case BIQUAD_LOWSHELF:
			c = 2.0 * sqrt(a) * alpha;
			b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + c);
			b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0);
			b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - c);
			a0 = (a + 1.0) + (a - 1.0) * cos_w0 + c;
			a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0);
			a2 = (a + 1.0) + (a - 1.0) * cos_w0 - c;
			break;
		case BIQUAD_HIGHSHELF:
			c = 2.0 * sqrt(a) * alpha;
			b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + c);
			b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0);
			b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - c);
			a0 = (a + 1.0) - (a - 1.0) * cos_w0 + c;
			a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0);
			a2 = (a + 1.0) - (a - 1.0) * cos_w0 - c;
			break;
		}
	}
	biquad_init(b, b0, b1, b2, a0, a1, a2);
}

sample_t * biquad_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	struct biquad_state *state = (struct biquad_state *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels)
		for (int k = 0; k < e->ostream.channels; ++k)
			if (GET_BIT(e->channel_selector, k))
				ibuf[i + k] = biquad(&state[k], ibuf[i + k]);
	return ibuf;
}

sample_t * biquad_effect_run_all(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	const ssize_t samples = *frames * e->ostream.channels;
	struct biquad_state *state = (struct biquad_state *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels)
		for (int k = 0; k < e->ostream.channels; ++k)
			ibuf[i + k] = biquad(&state[k], ibuf[i + k]);
	return ibuf;
}

void biquad_effect_reset(struct effect *e)
{
	struct biquad_state *state = (struct biquad_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		if (GET_BIT(e->channel_selector, k))
			biquad_reset(&state[k]);
}

void biquad_effect_plot(struct effect *e, int i)
{
	struct biquad_state *state = (struct biquad_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (GET_BIT(e->channel_selector, k)) {
			printf("H%d_%d(w)=(abs(w)<=pi)?(" BIQUAD_PLOT_FMT "):0/0\n",
				k, i, BIQUAD_PLOT_FMT_ARGS(&state[k]));
		}
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void biquad_effect_destroy(struct effect *e)
{
	free(e->data);
	free(e->channel_selector);
}

static int biquad_effect_can_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge != src->merge) return 0;
	for (int k = 0; k < dest->ostream.channels; ++k)
		if (GET_BIT(dest->channel_selector, k) && GET_BIT(src->channel_selector, k))
			return 0;
	return 1;
}

static void biquad_effect_set_run_func(struct effect *e)
{
	const int n_ch = e->ostream.channels;
	const int n_sel = num_bits_set(e->channel_selector, n_ch);
	if (n_sel == n_ch) e->run = biquad_effect_run_all;
	else e->run = biquad_effect_run;
}

struct effect * biquad_effect_merge(struct effect *dest, struct effect *src)
{
	if (biquad_effect_can_merge(dest, src)) {
		struct biquad_state *dest_state = (struct biquad_state *) dest->data;
		struct biquad_state *src_state = (struct biquad_state *) src->data;
		for (int k = 0; k < dest->ostream.channels; ++k) {
			if (GET_BIT(src->channel_selector, k)) {
				SET_BIT(dest->channel_selector, k);
				memcpy(&dest_state[k], &src_state[k], sizeof(struct biquad_state));
			}
		}
		biquad_effect_set_run_func(dest);
		return dest;
	}
	return NULL;
}

#define GET_ARG(v, str, name) \
	do { \
		v = strtod(str, &endptr); \
		CHECK_ENDPTR(str, endptr, name, return NULL); \
	} while (0)

#define GET_FREQ_ARG(v, str, name) \
	do { \
		v = parse_freq(str, &endptr); \
		CHECK_ENDPTR(str, endptr, name, return NULL); \
		CHECK_FREQ(v, istream->fs, name, return NULL); \
	} while (0)

#define GET_WIDTH_ARG(v, str, name) \
	do { \
		v = parse_width(str, &width_type, &endptr); \
		CHECK_ENDPTR(str, endptr, name, return NULL); \
		CHECK_RANGE((v) > 0.0, name, return NULL); \
	} while (0)

#define BIQUAD_WIDTH_TEST_NO_SLOPE (width_type != BIQUAD_WIDTH_SLOPE && width_type != BIQUAD_WIDTH_SLOPE_DB)
#define CHECK_WIDTH_TYPE(cond) \
	do { if (!(cond)) { \
		LOG_FMT(LL_ERROR, "%s: error: invalid width type", argv[0]); \
		return NULL; \
	} } while (0)

#define INIT_COMMON(n_args, b_type) \
	do { \
		if (argc != (n_args) + 1) { \
			LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage); \
			return NULL; \
		} \
		type = b_type; \
	} while (0)

struct effect * biquad_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int i, type, width_type = BIQUAD_WIDTH_Q;
	double arg0 = 0.0, arg1 = 0.0, arg2 = 0.0, arg3 = 0.0;
	double b0 = 0.0, b1 = 0.0, b2 = 0.0, a0 = 0.0, a1 = 0.0, a2 = 0.0;
	struct biquad_state *state;
	struct effect *e;
	char *endptr;

	switch (ei->effect_number) {
	case BIQUAD_LOWPASS_1:
	case BIQUAD_HIGHPASS_1:
	case BIQUAD_ALLPASS_1:
	case BIQUAD_LOWPASS_1P:
		INIT_COMMON(1, ei->effect_number);
		GET_FREQ_ARG(arg0, argv[1], "f0");
		break;
	case BIQUAD_LOWSHELF_1:
	case BIQUAD_HIGHSHELF_1:
		INIT_COMMON(2, ei->effect_number);
		GET_FREQ_ARG(arg0, argv[1], "f0");
		/* no width argument */
		GET_ARG(arg2, argv[2], "gain");
		break;
	case BIQUAD_LOWPASS:
	case BIQUAD_HIGHPASS:
	case BIQUAD_BANDPASS_SKIRT:
	case BIQUAD_BANDPASS_PEAK:
	case BIQUAD_NOTCH:
	case BIQUAD_ALLPASS:
		INIT_COMMON(2, ei->effect_number);
		GET_FREQ_ARG(arg0, argv[1], "f0");
		GET_WIDTH_ARG(arg1, argv[2], "width");
		CHECK_WIDTH_TYPE(BIQUAD_WIDTH_TEST_NO_SLOPE);
		break;
	case BIQUAD_PEAK:
	case BIQUAD_LOWSHELF:
	case BIQUAD_HIGHSHELF:
		INIT_COMMON(3, ei->effect_number);
		GET_FREQ_ARG(arg0, argv[1], "f0");
		GET_WIDTH_ARG(arg1, argv[2], "width");
		if (ei->effect_number == BIQUAD_PEAK) CHECK_WIDTH_TYPE(BIQUAD_WIDTH_TEST_NO_SLOPE);
		GET_ARG(arg2, argv[3], "gain");
		break;
	case BIQUAD_LOWPASS_TRANSFORM:
	case BIQUAD_HIGHPASS_TRANSFORM:
		INIT_COMMON(4, ei->effect_number);
		GET_FREQ_ARG(arg0, argv[1], "fz");
		GET_WIDTH_ARG(arg1, argv[2], "width_z");
		CHECK_WIDTH_TYPE(width_type == BIQUAD_WIDTH_Q);
		GET_FREQ_ARG(arg2, argv[3], "fp");
		GET_WIDTH_ARG(arg3, argv[4], "width_p");
		CHECK_WIDTH_TYPE(width_type == BIQUAD_WIDTH_Q);
		break;
	case BIQUAD_DEEMPH:
		INIT_COMMON(0, BIQUAD_HIGHSHELF);
		width_type = BIQUAD_WIDTH_SLOPE;
		switch (istream->fs) {
		case 44100:
			arg0 = 5283;
			arg1 = 0.4845;
			arg2 = -9.477;
			break;
		case 48000:
			arg0 = 5356;
			arg1 = 0.479;
			arg2 = -9.62;
			break;
		default:
			LOG_FMT(LL_ERROR, "%s: error: sample rate must be 44100 or 48000", argv[0]);
			return NULL;
		}
		break;
	case BIQUAD_BIQUAD:
		INIT_COMMON(6, BIQUAD_BIQUAD);
		GET_ARG(b0, argv[1], "b0");
		GET_ARG(b1, argv[2], "b1");
		GET_ARG(b2, argv[3], "b2");
		GET_ARG(a0, argv[4], "a0");
		GET_ARG(a1, argv[5], "a1");
		GET_ARG(a2, argv[6], "a2");
		break;
	default:
		LOG_FMT(LL_ERROR, "%s: BUG: unknown filter type: %s (%d)", __FILE__, argv[0], ei->effect_number);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	biquad_effect_set_run_func(e);
	e->reset = biquad_effect_reset;
	e->plot = biquad_effect_plot;
	e->destroy = biquad_effect_destroy;
	e->merge = biquad_effect_merge;
	state = calloc(istream->channels, sizeof(struct biquad_state));
	for (i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (type == BIQUAD_BIQUAD)
				biquad_init(&state[i], b0, b1, b2, a0, a1, a2);
			else
				biquad_init_using_type(&state[i], type, istream->fs, arg0, arg1, arg2, arg3, width_type);
		}
	}
	e->data = state;
	return e;
}
