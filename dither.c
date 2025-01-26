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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include "dither.h"
#include "util.h"

/*
 * References:
 *     [1] S. P. Lipshitz, J. Vanderkooy, and R. A. Wannamaker,
 *         "Minimally Audible Noise Shaping," J. AES, vol. 39, no. 11,
 *         November 1991
 *     [2] R. A. Wannamaker, "Psychoacoustically Optimal Noise Shaping,"
 *         J. AES, vol. 40, no. 7/8, July 1992
*/

#define MAX_FIR_LEN 9

enum dither_type {
	DITHER_TYPE_FLAT = 1,     /* flat tpdf with no feedback */
	DITHER_TYPE_SLOPED,       /* flat tpdf with feedback; first-order highpass response */
	DITHER_TYPE_SLOPED2,      /* sloped tpdf with feedback; stronger HF emphasis than SLOPED */
	DITHER_TYPE_LIPSHITZ_44,  /* 5-tap e-weighted curve from [1] */
	DITHER_TYPE_WAN3_44,      /* 3-tap f-weighted curve from [2] */
	DITHER_TYPE_WAN9_44,      /* 9-tap f-weighted curve from [2] */
};

enum dither_flags {
	DITHER_FLAG_ENABLE             = 1<<0,
	DITHER_FLAG_NOISE_BITS_AUTO    = 1<<1,
	DITHER_FLAG_QUANTIZE_BITS_AUTO = 1<<2,
};

struct dither_type_info {
	const char *name;
	enum dither_type type;
	int fs;
};

struct dither_state {
	void (*run)(struct dither_state *, sample_t *, ssize_t, ssize_t);
	sample_t n_mult, q_mult[2];
	sample_t z_1, fir_buf[MAX_FIR_LEN];
	int32_t m0;
	int p;
	enum dither_flags flags;
};

static struct dither_type_info dither_types[] = {
	{ "flat",     DITHER_TYPE_FLAT,            0 },
	{ "sloped",   DITHER_TYPE_SLOPED,          0 },
	{ "sloped2",  DITHER_TYPE_SLOPED2,         0 },
	{ "lipshitz", DITHER_TYPE_LIPSHITZ_44, 44100 },
	{ "wan3",     DITHER_TYPE_WAN3_44,     46000 },
	{ "wan9",     DITHER_TYPE_WAN9_44,     46000 },
};

static const sample_t filter_lipshitz_44[] = {
	2.033, -2.165, 1.959, -1.590, 0.6149
};

static const sample_t filter_wan3_44[] = {
	1.623, -0.982, 0.109
};

static const sample_t filter_wan9_44[] = {
	2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847
};

static uint32_t r_seed[2] = { 1, 1 };

static struct dither_type_info * get_dither_type_info(const char *name, int fs)
{
	if (name == NULL)
		return &dither_types[0];
	for (int i = 0; i < LENGTH(dither_types); ++i)
		if (strcmp(name, dither_types[i].name) == 0
				&& (fs == 0 || dither_types[i].fs == 0 || fabs(dither_types[i].fs - fs) < dither_types[i].fs * 0.05))
			return &dither_types[i];
	return NULL;
}

static const char * get_dither_type_name(enum dither_type t)
{
	for (int i = 0; i < LENGTH(dither_types); ++i)
		if (dither_types[i].type == t)
			return dither_types[i].name;
	return "unknown";
}

static inline sample_t noise_tpdf_flat(struct dither_state *state)
{
	int32_t n1 = pm_rand1_r(&r_seed[0]);
	int32_t n2 = pm_rand2_r(&r_seed[1]);
	return (n1 - n2) * state->n_mult;
}

static inline sample_t noise_tpdf_sloped(struct dither_state *state)
{
	int32_t n1 = pm_rand1_r(&r_seed[0]);
	int32_t n2 = state->m0;
	state->m0 = n1;
	return (n1 - n2) * state->n_mult;
}

static inline sample_t filter_fn_none(struct dither_state *state, const sample_t *filter_coefs, sample_t s)
{
	return s;
}

#define FIR_DEFINE_FN(N) \
	static inline sample_t filter_fn_fir_ ## N (struct dither_state *state, const sample_t filter[N], sample_t s) \
	{ \
		for (int n = state->p, m = 0; m < N; ++m) { \
			state->fir_buf[n] += s * filter[m]; \
			n = (n+1 < N) ? n+1 : 0; \
		} \
		const sample_t r = state->fir_buf[state->p]; \
		state->fir_buf[state->p] = 0.0; \
		state->p = (state->p+1 < N) ? state->p+1 : 0; \
		return r; \
	}

FIR_DEFINE_FN(3)
FIR_DEFINE_FN(5)
FIR_DEFINE_FN(9)

#define DITHER_LOOP_NO_FB(noise_fn) \
	do { \
		const sample_t noise = noise_fn(state); \
		*buf = state->q_mult[1] * nearbyint(state->q_mult[0] * (*buf + noise)); \
	} while (0)

#define DITHER_LOOP_FB(noise_fn, filter_fn, filter) \
	do { \
		const sample_t noise = noise_fn(state); \
		const sample_t p0 = *buf - filter_fn(state, filter, state->z_1); \
		const sample_t p1 = state->q_mult[1] * nearbyint(state->q_mult[0] * (p0 + noise)); \
		state->z_1 = p1 - p0; \
		*buf = p1; \
	} while (0)

#define DITHER_RUN_DEFINE_FN(X, C) \
	static void dither_run_ ## X (struct dither_state *state, sample_t *buf, ssize_t samples, ssize_t stride) \
	{ while (samples-- > 0) { C; buf += stride; } }

DITHER_RUN_DEFINE_FN(flat, DITHER_LOOP_NO_FB(noise_tpdf_flat))
DITHER_RUN_DEFINE_FN(sloped, DITHER_LOOP_FB(noise_tpdf_flat, filter_fn_none, NULL))
DITHER_RUN_DEFINE_FN(sloped2, DITHER_LOOP_FB(noise_tpdf_sloped, filter_fn_none, NULL))
DITHER_RUN_DEFINE_FN(lipshitz_44, DITHER_LOOP_FB(noise_tpdf_flat, filter_fn_fir_5, filter_lipshitz_44))
DITHER_RUN_DEFINE_FN(wan3_44, DITHER_LOOP_FB(noise_tpdf_flat, filter_fn_fir_3, filter_wan3_44))
DITHER_RUN_DEFINE_FN(wan9_44, DITHER_LOOP_FB(noise_tpdf_flat, filter_fn_fir_9, filter_wan9_44))

static inline void dither_reset(struct dither_state *state)
{
	state->z_1 = 0.0;
	memset(state->fir_buf, 0, sizeof(sample_t) * MAX_FIR_LEN);
	state->m0 = 1;
	state->p = 0;
}

static inline void dither_set_noise_bits(struct dither_state *state, double noise_bits)
{
	state->n_mult = 2.0 / exp2(noise_bits) / PM_RAND_MAX;
}

static inline void dither_set_quantize_bits(struct dither_state *state, int quantize_bits)
{
	quantize_bits = MAXIMUM(MINIMUM(quantize_bits, 32), 2);
	state->q_mult[0] = (sample_t) (((uint32_t) 1) << (quantize_bits - 1));
	state->q_mult[1] = 1.0 / state->q_mult[0];
}

static void dither_init(struct dither_state *state, int quantize_bits, double noise_bits, enum dither_type type, enum dither_flags flags)
{
	switch (type) {
	case DITHER_TYPE_FLAT:        state->run = dither_run_flat;        break;
	case DITHER_TYPE_SLOPED:      state->run = dither_run_sloped;      break;
	case DITHER_TYPE_SLOPED2:     state->run = dither_run_sloped2;     break;
	case DITHER_TYPE_LIPSHITZ_44: state->run = dither_run_lipshitz_44; break;
	case DITHER_TYPE_WAN3_44:     state->run = dither_run_wan3_44;     break;
	case DITHER_TYPE_WAN9_44:     state->run = dither_run_wan9_44;     break;
	}
	dither_set_noise_bits(state, noise_bits);
	dither_set_quantize_bits(state, quantize_bits);
	state->flags = flags;
	dither_reset(state);
}

sample_t * dither_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct dither_state *state = (struct dither_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		if (state[k].flags & DITHER_FLAG_ENABLE)
			state[k].run(&state[k], &ibuf[k], *frames, e->ostream.channels);
	return ibuf;
}

void dither_effect_reset(struct effect *e)
{
	struct dither_state *state = (struct dither_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k)
		if (GET_BIT(e->channel_selector, k))
			dither_reset(&state[k]);
}

void dither_effect_destroy(struct effect *e)
{
	free(e->data);
	free(e->channel_selector);
}

static int dither_effect_can_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge != src->merge) return 0;
	for (int k = 0; k < dest->ostream.channels; ++k)
		if (GET_BIT(dest->channel_selector, k) && GET_BIT(src->channel_selector, k))
			return 0;
	return 1;
}

struct effect * dither_effect_merge(struct effect *dest, struct effect *src)
{
	if (dither_effect_can_merge(dest, src)) {
		struct dither_state *dest_state = (struct dither_state *) dest->data;
		struct dither_state *src_state = (struct dither_state *) src->data;
		for (int k = 0; k < dest->ostream.channels; ++k) {
			if (GET_BIT(src->channel_selector, k)) {
				SET_BIT(dest->channel_selector, k);
				memcpy(&dest_state[k], &src_state[k], sizeof(struct dither_state));
			}
		}
		return dest;
	}
	return NULL;
}

int effect_is_dither(const struct effect *e)
{
	if (e->run == dither_effect_run) return 1;
	return 0;
}

void dither_effect_set_params(struct effect *e, int bits, int enabled)
{
	struct dither_state *state = (struct dither_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (GET_BIT(e->channel_selector, k)) {
			if (state[k].flags & DITHER_FLAG_NOISE_BITS_AUTO) {
				if (!enabled || bits < 2 || bits > 32)
					state[k].flags &= ~(DITHER_FLAG_ENABLE);
				else {
					dither_set_noise_bits(&state[k], (double) bits);
					state[k].flags |= DITHER_FLAG_ENABLE;
				}
			}
			if (state[k].flags & DITHER_FLAG_QUANTIZE_BITS_AUTO)
				dither_set_quantize_bits(&state[k], (bits < 2) ? 32 : bits);  /* FIXME: perhaps allow no quantization? */
		}
	}
}

#define ARG_IS_AUTO(s) (strcmp((s), "auto") == 0)

struct effect * dither_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	struct effect *e;
	struct dither_state *state;
	enum dither_type d_type = DITHER_TYPE_FLAT;
	enum dither_flags d_flags = DITHER_FLAG_ENABLE;
	double noise_bits = HUGE_VAL;
	int quantize_bits = 0;

	if (argc > 4) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	int shape_arg = 0, qb_arg = 0, nb_arg = 0;
	if (argc == 2) {
		if (get_dither_type_info(argv[1], 0)) shape_arg = 1;
		else nb_arg = 1;
	}
	else {
		if (argc == 3) {
			if (get_dither_type_info(argv[1], 0)) shape_arg = 1;
			else qb_arg = 1;
		}
		else if (argc == 4) {
			shape_arg = 1;
			qb_arg = 2;
		}
		nb_arg = argc - 1;
	}

	if (shape_arg) {
		struct dither_type_info *dt_info = get_dither_type_info(argv[shape_arg], istream->fs);
		if (dt_info) d_type = dt_info->type;
		else {
			LOG_FMT(LL_ERROR, "%s: warning: invalid shape for fs=%d: %s", argv[0], istream->fs, argv[shape_arg]);
			d_type = DITHER_TYPE_SLOPED;
		}
	}
	if (qb_arg) {
		if (ARG_IS_AUTO(argv[qb_arg])) d_flags |= DITHER_FLAG_QUANTIZE_BITS_AUTO;
		else {
			quantize_bits = strtol(argv[qb_arg], &endptr, 0);
			CHECK_ENDPTR(argv[qb_arg], endptr, "quantize_bits", return NULL);
			if (quantize_bits < 2 || quantize_bits > 32) {
				LOG_FMT(LL_ERROR, "%s: error: %s must be within [2,32]", argv[0], "quantize_bits");
				return NULL;
			}
		}
	}
	if (nb_arg && !ARG_IS_AUTO(argv[nb_arg])) {
		noise_bits = strtod(argv[nb_arg], &endptr);
		CHECK_ENDPTR(argv[nb_arg], endptr, "noise_bits", return NULL);
		if (!isfinite(noise_bits)) {
			LOG_FMT(LL_ERROR, "%s: error: %s is invalid: %g", argv[0], "bits", noise_bits);
			return NULL;
		}
		if (!qb_arg && !(d_flags & DITHER_FLAG_QUANTIZE_BITS_AUTO)) {
			const double v = rint(noise_bits);
			quantize_bits = (int) MAXIMUM(MINIMUM(v, 32.0), 2.0);
		}
	}
	else {
		d_flags |= DITHER_FLAG_NOISE_BITS_AUTO;
		if (!qb_arg) d_flags |= DITHER_FLAG_QUANTIZE_BITS_AUTO;
	}

	if (LOGLEVEL(LL_VERBOSE)) {
		dsp_log_acquire();
		dsp_log_printf("%s: info: shape=%s", argv[0], get_dither_type_name(d_type));
		if (d_flags & DITHER_FLAG_QUANTIZE_BITS_AUTO) dsp_log_printf(" quantize_bits=%s", "auto");
		else dsp_log_printf(" quantize_bits=%d", quantize_bits);
		if (d_flags & DITHER_FLAG_NOISE_BITS_AUTO) dsp_log_printf(" bits=%s", "auto");
		else dsp_log_printf(" bits=%g", noise_bits);
		dsp_log_printf("\n");
		dsp_log_release();
	}

	/* set reasonable defaults when unset (should only happen if auto) */
	if (quantize_bits == 0) quantize_bits = 16;
	if (noise_bits == HUGE_VAL) noise_bits = 16.0;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(e->channel_selector, channel_selector, istream->channels);
	e->run = dither_effect_run;
	e->reset = dither_effect_reset;
	e->destroy = dither_effect_destroy;
	e->merge = dither_effect_merge;

	state = calloc(istream->channels, sizeof(struct dither_state));
	for (int k = 0; k < istream->channels; ++k)
		if (GET_BIT(e->channel_selector, k))
			dither_init(&state[k], quantize_bits, noise_bits, d_type, d_flags);

	e->data = state;
	return e;
}
