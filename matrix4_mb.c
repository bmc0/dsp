/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <complex.h>
#include <math.h>
#include "matrix4_mb.h"
#include "biquad.h"
#include "cap5.h"
#include "util.h"
#include "fir.h"

#define DOWNSAMPLE_FACTOR  32
#define EVENT_THRESH_MAX  3.6
#define EVENT_THRESH_MIN  1.4
#define NORM_ACCOM_FACTOR 0.6
#include "matrix4_common.h"

#define N_BANDS 13

#if N_BANDS == 11
static const double fb_fdiv[]   = { 175, 329.29, 542.52, 837.21, 1244.5, 1807.4, 2585.3, 3660.5, 5146.4, 7200 };
static const double fb_fc[]     = { 114.68, 245.92, 427.29, 677.97, 1024.4, 1503.2, 2165, 3079.5, 4343.5, 6090.3, 8504.6 };
static const int    fb_ap_idx[] = { 5, 6, 7, 8, 9, 3, 2, 1, 0, 2, 3, 0, 3, 8, 9, 6, 5, 6, 9 };
#define BAND_WEIGHT_IDX_MULT 1.0
#elif N_BANDS == 12
static const double fb_fdiv[]   = { 175, 329.29, 542.52, 837.21, 1244.5, 1807.4, 2585.3, 3660.5, 5146.4, 7200, 10038 };
static const double fb_fc[]     = { 114.68, 245.92, 427.29, 677.97, 1024.4, 1503.2, 2165, 3079.5, 4343.5, 6090.3, 8504.6, 11841 };
static const int    fb_ap_idx[] = { 6, 7, 8, 9, 10, 4, 3, 2, 1, 0, 3, 4, 1, 0, 1, 4, 9, 10, 7, 6, 7, 10 };
#define BAND_WEIGHT_IDX_MULT 1.0
#elif N_BANDS == 13
static const double fb_fdiv[]   = { 170, 316.39, 516.52, 790.1, 1164.1, 1675.4, 2374.3, 3329.8, 4636.1, 6421.7, 8862.9, 12200 };
static const double fb_fc[]     = { 112.28, 237.49, 408.65, 642.64, 962.52, 1399.8, 1997.6, 2814.8, 3932, 5459.3, 7547.1, 10401, 14303 };
static const int    fb_ap_idx[] = { 6, 7, 8, 9, 10, 11, 4, 3, 2, 1, 0, 3, 4, 1, 0, 1, 4, 9, 10, 11, 7, 6, 7, 11, 9 };
#define BAND_WEIGHT_IDX_MULT 0.95
#else
#error "unsupported number of bands"
#endif

static const double fshape_lf[] = { 10, M_SQRT1_2, 180, 0.4 };
static const double fshape_hf[] = { 0.46, 0.5, 14000, 0.5 };  /* note: [0] is multiplied by fs */

#define PHASE_LIN_MAX_LEN      50.0  /* maximum filter length in milliseconds */
#define PHASE_LIN_TRUNC_THRESH 1e-6  /* truncation threshold */

#define DO_FILTER_BANK_TEST 0

#ifndef BAND_WEIGHT_IDX_MULT
	#define BAND_WEIGHT_IDX_MULT (11.0/N_BANDS)
#endif

struct fshape_state {
	struct biquad_state lf, hf;
};

struct filter_bank_frame {
	sample_t s[N_BANDS];
};

struct filter_bank {
	struct cap5_state f[LENGTH(fb_fdiv)];
	struct ap2_state ap[LENGTH(fb_ap_idx)];
	sample_t s[N_BANDS];
};

struct matrix4_band {
	struct smooth_state sm;
	struct event_state ev;
	struct axes ax, ax_ev;
	struct {
		struct cs_interp_state ll, lr, rl, rr;
		struct cs_interp_state lsl, lsr, rsl, rsr;
	} m_interp;
	struct ewma_state bg_cs, ev_thresh;
	double ev_thresh_max, ev_thresh_min;
	double surr_mult, shape_mult;
#ifndef LADSPA_FRONTEND
	struct steering_bar lr_bar, cs_bar;
#endif
};

struct matrix4_mb_state {
	int s, c0, c1;
	char has_output, is_draining, disable;
	enum status_type status_type;
	struct fshape_state fshape[2], inv_fshape[4];
	struct filter_bank fb[2];
	struct matrix4_band band[N_BANDS];
	sample_t **bufs;
	struct filter_bank_frame *fb_buf[2];
	double base_surr_mult;
	struct event_config evc;
	calc_matrix_coefs_func calc_matrix_coefs;
	ssize_t len, p, fb_buf_len, fb_buf_p;
	ssize_t drain_frames, fade_frames, fade_p;
};

static void fshape_filter_init(struct biquad_state *b, double fs, const double p[4], int is_hf, int is_inv)
{
	const int type = (is_hf) ? BIQUAD_LOWPASS_TRANSFORM : BIQUAD_HIGHPASS_TRANSFORM;
	const double f0 = (is_hf) ? fs*p[0] : p[0];
	if (is_inv) biquad_init_using_type(b, type, fs, p[2], p[3], f0, p[1], BIQUAD_WIDTH_Q);
	else biquad_init_using_type(b, type, fs, f0, p[1], p[2], p[3], BIQUAD_WIDTH_Q);
}

static void fshape_init(struct fshape_state *state, double fs, const double lfp[4], const double hfp[4], int is_inv)
{
	fshape_filter_init(&state->lf, fs, lfp, 0, is_inv);
	fshape_filter_init(&state->hf, fs, hfp, 1, is_inv);
}

static inline sample_t fshape_run(struct fshape_state *state, sample_t s)
{
	return biquad(&state->hf, biquad(&state->lf, s));
}

static void filter_bank_init(struct filter_bank *fb, double fs, enum filter_bank_type fb_type, double fb_stop[2])
{
	double complex ap[3];
	switch (fb_type) {
	case FILTER_BANK_TYPE_BUTTERWORTH:
		cap5_butterworth_ap(ap);
		break;
	case FILTER_BANK_TYPE_CHEBYSHEV1:
		cap5_chebyshev_ap(0, fb_stop[0], ap);
		break;
	case FILTER_BANK_TYPE_CHEBYSHEV2:
		cap5_chebyshev_ap(1, fb_stop[0], ap);
		break;
	case FILTER_BANK_TYPE_ELLIPTIC:
		cap5_elliptic_ap(fb_stop[0], fb_stop[1], ap);
		break;
	}
	for (int i = 0; i < LENGTH(fb_fdiv); ++i)
		cap5_init(&fb->f[i], fs, fb_fdiv[i], ap);
	for (int i = 0; i < LENGTH(fb_ap_idx); ++i)
		fb->ap[i] = fb->f[fb_ap_idx[i]].a1;
}

static void filter_bank_run(struct filter_bank *fb, sample_t s)
{
#if N_BANDS == 11
	cap5_run(&fb->f[4], s, &fb->s[4], &fb->s[5]);  /* split at xover 4 (1244.5Hz) */
	fb->s[4] = ap2_run(&fb->ap[0], fb->s[4]);  /* xover 5 ap */
	fb->s[4] = ap2_run(&fb->ap[1], fb->s[4]);  /* xover 6 ap */
	fb->s[4] = ap2_run(&fb->ap[2], fb->s[4]);  /* xover 7 ap */
	fb->s[4] = ap2_run(&fb->ap[3], fb->s[4]);  /* xover 8 ap */
	fb->s[4] = ap2_run(&fb->ap[4], fb->s[4]);  /* xover 9 ap */
	fb->s[5] = ap2_run(&fb->ap[5], fb->s[5]);  /* xover 3 ap */
	fb->s[5] = ap2_run(&fb->ap[6], fb->s[5]);  /* xover 2 ap */
	fb->s[5] = ap2_run(&fb->ap[7], fb->s[5]);  /* xover 1 ap */
	fb->s[5] = ap2_run(&fb->ap[8], fb->s[5]);  /* xover 0 ap */

	cap5_run(&fb->f[1], fb->s[4], &fb->s[1], &fb->s[2]);  /* split at xover 1 (329.29Hz) */
	fb->s[1] = ap2_run(&fb->ap[9], fb->s[1]);  /* xover 2 ap */
	fb->s[1] = ap2_run(&fb->ap[10], fb->s[1]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[1], &fb->s[0], &fb->s[1]);  /* split at xover 0 (175Hz) */

	cap5_run(&fb->f[2], fb->s[2], &fb->s[2], &fb->s[3]);  /* split at xover 2 (542.52Hz) */
	fb->s[2] = ap2_run(&fb->ap[12], fb->s[2]);  /* xover 3 ap */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (837.21Hz) */

	cap5_run(&fb->f[7], fb->s[5], &fb->s[7], &fb->s[8]);  /* split at xover 7 (3660.5Hz) */
	fb->s[7] = ap2_run(&fb->ap[13], fb->s[7]);  /* xover 8 ap */
	fb->s[7] = ap2_run(&fb->ap[14], fb->s[7]);  /* xover 9 ap */
	fb->s[8] = ap2_run(&fb->ap[15], fb->s[8]);  /* xover 6 ap */
	fb->s[8] = ap2_run(&fb->ap[16], fb->s[8]);  /* xover 5 ap */

	cap5_run(&fb->f[5], fb->s[7], &fb->s[5], &fb->s[6]);  /* split at xover 5 (1807.4Hz) */
	fb->s[5] = ap2_run(&fb->ap[17], fb->s[5]);  /* xover 6 ap */

	cap5_run(&fb->f[6], fb->s[6], &fb->s[6], &fb->s[7]);  /* split at xover 6 (2585.3Hz) */

	cap5_run(&fb->f[8], fb->s[8], &fb->s[8], &fb->s[9]);  /* split at xover 8 (5146.4Hz) */
	fb->s[8] = ap2_run(&fb->ap[18], fb->s[8]);  /* xover 9 ap */

	cap5_run(&fb->f[9], fb->s[9], &fb->s[9], &fb->s[10]);  /* split at xover 9 (7200Hz) */
#elif N_BANDS == 12
	cap5_run(&fb->f[5], s, &fb->s[5], &fb->s[6]);  /* split at xover 5 (1807.4Hz) */
	fb->s[5] = ap2_run(&fb->ap[0], fb->s[5]);  /* xover 6 ap */
	fb->s[5] = ap2_run(&fb->ap[1], fb->s[5]);  /* xover 7 ap */
	fb->s[5] = ap2_run(&fb->ap[2], fb->s[5]);  /* xover 8 ap */
	fb->s[5] = ap2_run(&fb->ap[3], fb->s[5]);  /* xover 9 ap */
	fb->s[5] = ap2_run(&fb->ap[4], fb->s[5]);  /* xover 10 ap */
	fb->s[6] = ap2_run(&fb->ap[5], fb->s[6]);  /* xover 4 ap */
	fb->s[6] = ap2_run(&fb->ap[6], fb->s[6]);  /* xover 3 ap */
	fb->s[6] = ap2_run(&fb->ap[7], fb->s[6]);  /* xover 2 ap */
	fb->s[6] = ap2_run(&fb->ap[8], fb->s[6]);  /* xover 1 ap */
	fb->s[6] = ap2_run(&fb->ap[9], fb->s[6]);  /* xover 0 ap */

	cap5_run(&fb->f[2], fb->s[5], &fb->s[2], &fb->s[3]);  /* split at xover 2 (542.52Hz) */
	fb->s[2] = ap2_run(&fb->ap[10], fb->s[2]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[12], fb->s[3]);  /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[13], fb->s[3]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 (175Hz) */
	fb->s[0] = ap2_run(&fb->ap[14], fb->s[0]);  /* xover 1 ap */

	cap5_run(&fb->f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 (329.29Hz) */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (837.21Hz) */
	fb->s[3] = ap2_run(&fb->ap[15], fb->s[3]);  /* xover 4 ap */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 (1244.5Hz) */

	cap5_run(&fb->f[8], fb->s[6], &fb->s[8], &fb->s[9]);  /* split at xover 8 (5146.4Hz) */
	fb->s[8] = ap2_run(&fb->ap[16], fb->s[8]);  /* xover 9 ap */
	fb->s[8] = ap2_run(&fb->ap[17], fb->s[8]);  /* xover 10 ap */
	fb->s[9] = ap2_run(&fb->ap[18], fb->s[9]);  /* xover 7 ap */
	fb->s[9] = ap2_run(&fb->ap[19], fb->s[9]);  /* xover 6 ap */

	cap5_run(&fb->f[6], fb->s[8], &fb->s[6], &fb->s[7]);  /* split at xover 6 (2585.3Hz) */
	fb->s[6] = ap2_run(&fb->ap[20], fb->s[6]);  /* xover 7 ap */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (3660.5Hz) */

	cap5_run(&fb->f[9], fb->s[9], &fb->s[9], &fb->s[10]);  /* split at xover 9 (7200Hz) */
	fb->s[9] = ap2_run(&fb->ap[21], fb->s[9]);  /* xover 10 ap */

	cap5_run(&fb->f[10], fb->s[10], &fb->s[10], &fb->s[11]);  /* split at xover 10 (10038Hz) */
#elif N_BANDS == 13
	cap5_run(&fb->f[5], s, &fb->s[5], &fb->s[6]);  /* split at xover 5 (1675.4Hz) */
	fb->s[5] = ap2_run(&fb->ap[0], fb->s[5]);  /* xover 6 ap */
	fb->s[5] = ap2_run(&fb->ap[1], fb->s[5]);  /* xover 7 ap */
	fb->s[5] = ap2_run(&fb->ap[2], fb->s[5]);  /* xover 8 ap */
	fb->s[5] = ap2_run(&fb->ap[3], fb->s[5]);  /* xover 9 ap */
	fb->s[5] = ap2_run(&fb->ap[4], fb->s[5]);  /* xover 10 ap */
	fb->s[5] = ap2_run(&fb->ap[5], fb->s[5]);  /* xover 11 ap */
	fb->s[6] = ap2_run(&fb->ap[6], fb->s[6]);  /* xover 4 ap */
	fb->s[6] = ap2_run(&fb->ap[7], fb->s[6]);  /* xover 3 ap */
	fb->s[6] = ap2_run(&fb->ap[8], fb->s[6]);  /* xover 2 ap */
	fb->s[6] = ap2_run(&fb->ap[9], fb->s[6]);  /* xover 1 ap */
	fb->s[6] = ap2_run(&fb->ap[10], fb->s[6]);  /* xover 0 ap */

	cap5_run(&fb->f[2], fb->s[5], &fb->s[2], &fb->s[3]);  /* split at xover 2 (516.52Hz) */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[12], fb->s[2]);  /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[13], fb->s[3]);  /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[14], fb->s[3]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 (170Hz) */
	fb->s[0] = ap2_run(&fb->ap[15], fb->s[0]);  /* xover 1 ap */

	cap5_run(&fb->f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 (316.39Hz) */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (790.1Hz) */
	fb->s[3] = ap2_run(&fb->ap[16], fb->s[3]);  /* xover 4 ap */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 (1164.1Hz) */

	cap5_run(&fb->f[8], fb->s[6], &fb->s[8], &fb->s[9]);  /* split at xover 8 (4636.1Hz) */
	fb->s[8] = ap2_run(&fb->ap[17], fb->s[8]);  /* xover 9 ap */
	fb->s[8] = ap2_run(&fb->ap[18], fb->s[8]);  /* xover 10 ap */
	fb->s[8] = ap2_run(&fb->ap[19], fb->s[8]);  /* xover 11 ap */
	fb->s[9] = ap2_run(&fb->ap[20], fb->s[9]);  /* xover 7 ap */
	fb->s[9] = ap2_run(&fb->ap[21], fb->s[9]);  /* xover 6 ap */

	cap5_run(&fb->f[6], fb->s[8], &fb->s[6], &fb->s[7]);  /* split at xover 6 (2374.3Hz) */
	fb->s[6] = ap2_run(&fb->ap[22], fb->s[6]);  /* xover 7 ap */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (3329.8Hz) */

	cap5_run(&fb->f[10], fb->s[9], &fb->s[10], &fb->s[11]);  /* split at xover 10 (8862.9Hz) */
	fb->s[10] = ap2_run(&fb->ap[23], fb->s[10]);  /* xover 11 ap */
	fb->s[11] = ap2_run(&fb->ap[24], fb->s[11]);  /* xover 9 ap */

	cap5_run(&fb->f[9], fb->s[10], &fb->s[9], &fb->s[10]);  /* split at xover 9 (6421.7Hz) */

	cap5_run(&fb->f[11], fb->s[11], &fb->s[11], &fb->s[12]);  /* split at xover 11 (12200Hz) */
#endif
}

#if DO_FILTER_BANK_TEST
sample_t * matrix4_mb_test_fb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		const double s0 = fshape_run(&state->fshape[0], ibuf[i*e->istream.channels + state->c0]);
		const double s1 = fshape_run(&state->fshape[1], ibuf[i*e->istream.channels + state->c1]);
		filter_bank_run(&state->fb[0], s0);
		filter_bank_run(&state->fb[1], s1);
		double out_l = 0.0, out_r = 0.0, out_s = 0.0;
		for (int k = 0; k < N_BANDS; ++k) {
			struct matrix4_band *band = &state->band[k];
			const double norm_mult = CALC_NORM_MULT(band->surr_mult);
			out_l += state->fb[0].s[k]*norm_mult;
			out_r += state->fb[1].s[k]*norm_mult;
			out_s += state->fb[0].s[k]*norm_mult*band->surr_mult*band->shape_mult;
		}
		out_l = fshape_run(&state->inv_fshape[0], out_l);
		out_r = fshape_run(&state->inv_fshape[1], out_r);
		out_s = fshape_run(&state->inv_fshape[2], out_s);
		for (int k = 0; k < e->istream.channels; ++k) {
			if (k == state->c0)
				obuf[i*e->ostream.channels + k] = out_l;
			else if (k == state->c1)
				obuf[i*e->ostream.channels + k] = out_r;
			else
				obuf[i*e->ostream.channels + k] = ibuf[i*e->istream.channels + k];
		}
		for (int k = 0; k < N_BANDS; ++k) {
			struct matrix4_band *band = &state->band[k];
			const double norm_mult = CALC_NORM_MULT(band->surr_mult);
			obuf[i*e->ostream.channels + e->istream.channels + k] = state->fb[0].s[k]*norm_mult*band->surr_mult*band->shape_mult;
		}
		obuf[i*e->ostream.channels + e->istream.channels + N_BANDS] = out_s;
	}
	return obuf;
}

void matrix4_mb_test_fb_effect_destroy(struct effect *e)
{
	free(e->data);
}

#else

sample_t * matrix4_mb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t oframes = 0;
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;

	for (ssize_t i = 0; i < *frames; ++i) {
		double cur_fade_mult = 1.0;
		if (state->fade_p > 0) {
			cur_fade_mult = fade_mult(state->fade_p, state->fade_frames, state->disable);
			--state->fade_p;
		}
		else if (state->disable) cur_fade_mult = 0.0;

		int n_angles = 0;
		struct axes angles[N_BANDS];
		sample_t out_l = 0.0, out_r = 0.0, out_ls = 0.0, out_rs = 0.0;
		const sample_t s0 = fshape_run(&state->fshape[0], ibuf[i*e->istream.channels + state->c0]);
		const sample_t s1 = fshape_run(&state->fshape[1], ibuf[i*e->istream.channels + state->c1]);
		filter_bank_run(&state->fb[0], s0);
		filter_bank_run(&state->fb[1], s1);
		#if DOWNSAMPLE_FACTOR > 1
		state->s = (state->s + 1 >= DOWNSAMPLE_FACTOR) ? 0 : state->s + 1;
		if (state->s == 0) {
		#else
		if (1) {
		#endif
			/* find bands with possible events */
			for (int k = 0; k < N_BANDS; ++k) {
				struct matrix4_band *band = &state->band[k];
				struct event_state *ev = &band->ev;
				if ((ev->slope_last[0] > 0.0 && ev->last[0] > band->ev_thresh_min)
						|| (ev->slope_last[1] > 0.0 && ev->last[1] > band->ev_thresh_min))
					angles[n_angles++] = ev->diff_last;
			}
		}
		for (int k = 0; k < N_BANDS; ++k) {
			struct matrix4_band *band = &state->band[k];

			const sample_t s0_fb = state->fb[0].s[k];
			const sample_t s1_fb = state->fb[1].s[k];
			const sample_t s0_d_fb = state->fb_buf[0][state->fb_buf_p].s[k];
			const sample_t s1_d_fb = state->fb_buf[1][state->fb_buf_p].s[k];

			struct envs env, pwr_env;
			calc_input_envs(&band->sm, s0_fb, s1_fb, &env, &pwr_env);

			#if DOWNSAMPLE_FACTOR > 1
			if (state->s == 0) {
			#else
			if (1) {
			#endif
				/* modulate event threshold based on the number of
				   bands with similar differential steering angles */
				struct event_state *ev = &band->ev;
				double ev_thresh_fact = 0.0;
				if ((ev->slope_last[0] > 0.0 && ev->last[0] > band->ev_thresh_min)
						|| (ev->slope_last[1] > 0.0 && ev->last[1] > band->ev_thresh_min)) {
					for (int j = 0; j < n_angles; ++j) {
						const double d_lr = fabs(angles[j].lr - ev->diff_last.lr);
						const double d_cs = fabs(angles[j].cs - ev->diff_last.cs);
						ev_thresh_fact += smoothstep(1.0 - MAXIMUM(d_lr, d_cs)*(16/M_PI));
					}
					ev_thresh_fact -= 1.0;
				}
				const double ev_thresh = ewma_run_set_max(&band->ev_thresh,
					band->ev_thresh_max - (band->ev_thresh_max-band->ev_thresh_min)*ev_thresh_fact*(1.0/(N_BANDS-1)));

				process_events(&band->ev, &state->evc, &env, &pwr_env, ev_thresh*(1.0/EVENT_THRESH), &band->ax, &band->ax_ev);
				norm_axes(&band->ax);

				const double w = ewma_run_set_min(&band->bg_cs, smoothstep(band->ax.cs*(-2/M_PI_4))+1.0)-1.0;
				const double surr_mult = (w*state->base_surr_mult + (1.0-w)*band->surr_mult)*cur_fade_mult;
				const double shape_mult = w + band->shape_mult*(1.0-w);

				struct matrix_coefs m = {0};
				state->calc_matrix_coefs(&band->ax, surr_mult, state->base_surr_mult*cur_fade_mult, &m, NULL);

				cs_interp_insert(&band->m_interp.ll, m.ll);
				cs_interp_insert(&band->m_interp.lr, m.lr);
				cs_interp_insert(&band->m_interp.rl, m.rl);
				cs_interp_insert(&band->m_interp.rr, m.rr);

				cs_interp_insert(&band->m_interp.lsl, m.lsl*shape_mult);
				cs_interp_insert(&band->m_interp.lsr, m.lsr*shape_mult);
				cs_interp_insert(&band->m_interp.rsl, m.rsl*shape_mult);
				cs_interp_insert(&band->m_interp.rsr, m.rsr*shape_mult);
			}

			out_l += s0_d_fb*cs_interp(&band->m_interp.ll, state->s) + s1_d_fb*cs_interp(&band->m_interp.lr, state->s);
			out_r += s0_d_fb*cs_interp(&band->m_interp.rl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rr, state->s);

			out_ls += s0_d_fb*cs_interp(&band->m_interp.lsl, state->s) + s1_d_fb*cs_interp(&band->m_interp.lsr, state->s);
			out_rs += s0_d_fb*cs_interp(&band->m_interp.rsl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rsr, state->s);

			state->fb_buf[0][state->fb_buf_p].s[k] = state->fb[0].s[k];
			state->fb_buf[1][state->fb_buf_p].s[k] = state->fb[1].s[k];
		}

		out_l = fshape_run(&state->inv_fshape[0], out_l);
		out_r = fshape_run(&state->inv_fshape[1], out_r);
		out_ls = fshape_run(&state->inv_fshape[2], out_ls);
		out_rs = fshape_run(&state->inv_fshape[3], out_rs);

		if (state->has_output) {
			for (int k = 0; k < e->istream.channels; ++k) {
				if (k == state->c0)
					obuf[oframes*e->ostream.channels + k] = out_l;
				else if (k == state->c1)
					obuf[oframes*e->ostream.channels + k] = out_r;
				else
					obuf[oframes*e->ostream.channels + k] = state->bufs[k][state->p];
				state->bufs[k][state->p] = ibuf[i*e->istream.channels + k];
			}
			obuf[oframes*e->ostream.channels + e->istream.channels + 0] = out_ls;
			obuf[oframes*e->ostream.channels + e->istream.channels + 1] = out_rs;
			++oframes;
		}
		else {
			for (int k = 0; k < e->istream.channels; ++k) {
				#ifdef SYMMETRIC_IO
					obuf[oframes*e->ostream.channels + k] = 0.0;
				#endif
				state->bufs[k][state->p] = ibuf[i*e->istream.channels + k];
			}
			#ifdef SYMMETRIC_IO
				obuf[oframes*e->ostream.channels + e->istream.channels + 0] = 0.0;
				obuf[oframes*e->ostream.channels + e->istream.channels + 1] = 0.0;
				++oframes;
			#endif
		}
		state->p = CBUF_NEXT(state->p, state->len);
		state->fb_buf_p = CBUF_NEXT(state->fb_buf_p, state->fb_buf_len);
		if (state->p == 0)
			state->has_output = 1;
	}
	#ifndef LADSPA_FRONTEND
		/* TODO: Implement a proper way for effects to show status lines. */
		if (state->status_type) {
			dsp_log_acquire();
			if (state->status_type == STATUS_TYPE_TEXT) {
				for (int i = 0; i < N_BANDS; ++i) {
					struct matrix4_band *band = &state->band[i];
					dsp_log_printf("\n%s%s: band %2d: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); adj: %05.3f; thresh: %05.3f; ord: %zd; diff: %zd; early: %zd; ign: %zd\033[K\r",
						e->name, (state->disable) ? " [off]" : "", i,
						TO_DEGREES(band->ax.lr), TO_DEGREES(band->ax_ev.lr), TO_DEGREES(band->ax.cs), TO_DEGREES(band->ax_ev.cs),
						band->ev.adj, ewma_get_last(&band->ev_thresh), band->ev.ord_count, band->ev.diff_count, band->ev.early_count, band->ev.ignore_count);
				}
			}
			else {
				for (int i = 0; i < N_BANDS; ++i) {
					struct matrix4_band *band = &state->band[i];
					draw_steering_bar(band->ax.lr, band->ev.hold, &band->lr_bar);
					draw_steering_bar(band->ax.cs, band->ev.hold, &band->cs_bar);
					dsp_log_printf("\n%s%s: band %2d: L[%s]R; C[%s]S; ord: %zd; diff: %zd; ign: %zd\033[K\r",
						e->name, (state->disable) ? " [off]" : "", i,
						band->lr_bar.s, band->cs_bar.s, band->ev.ord_count, band->ev.diff_count, band->ev.ignore_count);
				}
			}
			dsp_log_printf("\033[%dA", N_BANDS);
			dsp_log_release();
		}
	#endif

	*frames = oframes;
	return obuf;
}

ssize_t matrix4_mb_effect_delay(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	return (state->has_output) ? state->len : state->p;
}

void matrix4_mb_effect_reset(struct effect *e)
{
	int i;
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	state->p = 0;
	state->fb_buf_p = 0;
	state->has_output = 0;
	for (i = 0; i < e->istream.channels; ++i)
		memset(state->bufs[i], 0, state->len * sizeof(sample_t));
	memset(state->fb_buf[0], 0, state->fb_buf_len * sizeof(struct filter_bank_frame));
	memset(state->fb_buf[1], 0, state->fb_buf_len * sizeof(struct filter_bank_frame));
}

void matrix4_mb_effect_signal(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	state->disable = !state->disable;
	state->fade_p = state->fade_frames - state->fade_p;
	if (!state->status_type)
		LOG_FMT(LL_NORMAL, "%s: %s", e->name, (state->disable) ? "disabled" : "enabled");
}

sample_t * matrix4_mb_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	sample_t *rbuf = buf1;
	if (!state->has_output && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->len;
			state->is_draining = 1;
		}
		if (state->drain_frames > 0) {
			*frames = MINIMUM(*frames, state->drain_frames);
			state->drain_frames -= *frames;
			memset(buf1, 0, *frames * e->ostream.channels * sizeof(sample_t));
			rbuf = matrix4_mb_effect_run(e, frames, buf1, buf2);
		}
		else
			*frames = -1;
	}
	return rbuf;
}

void matrix4_mb_effect_destroy(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (int i = 0; i < e->istream.channels; ++i)
		free(state->bufs[i]);
	free(state->fb_buf[0]);
	free(state->fb_buf[1]);
	free(state->bufs);
	#ifndef LADSPA_FRONTEND
		if (state->status_type) {
			dsp_log_acquire();
			for (int i = 0; i < N_BANDS; ++i) dsp_log_printf("\033[K\n");
			dsp_log_printf("\033[K\r\033[%dA", N_BANDS);
			dsp_log_release();
		}
	#endif
	for (int i = 0; i < N_BANDS; ++i)
		event_state_cleanup(&state->band[i].ev);
	free(state);
}
#endif

struct effect * matrix4_mb_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	struct matrix4_mb_state *state;
	struct matrix4_config config = {0};

	if (get_args_and_channels(ei, istream, channel_selector, argc, argv, &config))
		return NULL;
	if (parse_effect_opts(argv, istream, 1, &config))
		return NULL;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
#if DO_FILTER_BANK_TEST
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + N_BANDS + 1;
	e->run = matrix4_mb_test_fb_effect_run;
	e->destroy = matrix4_mb_test_fb_effect_destroy;
#else
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + 2;
	e->run = matrix4_mb_effect_run;
	e->delay = matrix4_mb_effect_delay;
	e->reset = matrix4_mb_effect_reset;
	e->drain2 = matrix4_mb_effect_drain2;
	e->destroy = matrix4_mb_effect_destroy;
#endif

	state = calloc(1, sizeof(struct matrix4_mb_state));
	state->c0 = config.c0;
	state->c1 = config.c1;
#if !(DO_FILTER_BANK_TEST)
	state->status_type = config.status_type;
	state->calc_matrix_coefs = config.calc_matrix_coefs;
	e->signal = (config.enable_signal) ? matrix4_mb_effect_signal : NULL;

	for (int k = 0; k < N_BANDS; ++k) {
		struct matrix4_band *band = &state->band[k];
		smooth_state_init(&band->sm, istream);
		event_state_init(&band->ev, istream);
		const double x = MAXIMUM(k-1, 0)*0.15*BAND_WEIGHT_IDX_MULT;
		const double ev_thresh_mult = 1.0-(x/(x+1.0))*1.46*0.6;
		band->ev_thresh_max = EVENT_THRESH_MAX * ev_thresh_mult;
		band->ev_thresh_min = EVENT_THRESH_MIN * ev_thresh_mult;
		ewma_init(&band->ev_thresh, DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
		ewma_set(&band->ev_thresh, band->ev_thresh_max);
		ewma_init(&band->bg_cs, DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(ACCOM_TIME*2.0));
		ewma_set(&band->bg_cs, 1.0);
	}

	state->fb_buf_len = TIME_TO_FRAMES(DELAY_TIME, istream->fs);
#if DOWNSAMPLE_FACTOR > 1
	state->fb_buf_len += CS_INTERP_DELAY_FRAMES;
#endif
	state->fb_buf[0] = calloc(state->fb_buf_len, sizeof(struct filter_bank_frame));
	state->fb_buf[1] = calloc(state->fb_buf_len, sizeof(struct filter_bank_frame));
	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, istream->fs);
	event_config_init(&state->evc, istream);
#endif
	fshape_init(&state->fshape[0], istream->fs, fshape_lf, fshape_hf, 0);
	state->fshape[1] = state->fshape[0];
	fshape_init(&state->inv_fshape[0], istream->fs, fshape_lf, fshape_hf, 1);
	state->inv_fshape[1] = state->inv_fshape[0];
	state->inv_fshape[2] = state->inv_fshape[0];
	state->inv_fshape[3] = state->inv_fshape[0];

	filter_bank_init(&state->fb[0], istream->fs, config.fb_type, config.fb_stop);
	state->fb[1] = state->fb[0];

	const double shelf_mult2 = config.shelf_mult*config.shelf_mult;
	const double shelf_f02 = config.shelf_f0*config.shelf_f0;
	const double lowpass_f02 = config.lowpass_f0*config.lowpass_f0;
	for (int k = 0; k < N_BANDS; ++k) {
		struct matrix4_band *band = &state->band[k];
		const double fc2 = fb_fc[k]*fb_fc[k];
		const double shelf_norm_f2 = fc2/shelf_f02;
		const double shelf_mag = sqrt((1.0+shelf_mult2*shelf_norm_f2)/(1.0+shelf_norm_f2));
		band->surr_mult = (shelf_mag-1.0)*config.shelf_pwrcmp + 1.0;
		band->shape_mult = shelf_mag/band->surr_mult;
		band->surr_mult *= config.surr_mult[0];
		if (lowpass_f02 > 0.0) {
			const double lowpass_norm_f2 = fc2/lowpass_f02;
			band->shape_mult *= sqrt(1.0/(1.0+lowpass_norm_f2));
		}
		/* LOG_FMT(LL_VERBOSE, "%s: band %d: surr_mult=%.4g shape_mult=%.4g",
				argv[0], k, band->surr_mult, band->shape_mult); */
	}
	state->base_surr_mult = config.surr_mult[1];

	ssize_t phase_lin_frames = TIME_TO_FRAMES(PHASE_LIN_MAX_LEN, istream->fs);
	sample_t *filter = calloc(phase_lin_frames, sizeof(sample_t));
	for (int i = phase_lin_frames-1; i >= 0; --i) {
		filter_bank_run(&state->fb[1], (i == phase_lin_frames-1) ? 1.0 : 0.0);
		for (int k = 0; k < N_BANDS; ++k)
			filter[i] += state->fb[1].s[k];
	}
	int zx = 0;                      /* last zero crossing index */
	double integ = fabs(filter[0]);  /* unsigned integral since last zero crossing */
	const double trunc_thresh = PHASE_LIN_TRUNC_THRESH*PHASE_LIN_TRUNC_THRESH*istream->fs;
	for (int k = 1; integ < trunc_thresh && k < phase_lin_frames; ++k) {
		if (signbit(filter[k]) != signbit(filter[k-1])) {
			zx = k;
			integ = 0.0;
		}
		integ += fabs(filter[k]);
	}
	phase_lin_frames -= zx;
	struct effect *e_fir = fir_effect_init_with_filter(ei, istream, channel_selector, &filter[zx], 1, phase_lin_frames, 0);
	free(filter);
	state->fb[1] = state->fb[0];  /* reset */

#if !(DO_FILTER_BANK_TEST)
	state->len = state->fb_buf_len + (phase_lin_frames - 1);
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int i = 0; i < istream->channels; ++i)
		state->bufs[i] = calloc(state->len, sizeof(sample_t));
	struct effect *e_delay = matrix4_delay_effect_init(ei, &e->ostream, config.surr_delay_frames);
#endif

	e->data = state;
#if DO_FILTER_BANK_TEST
	if (e_fir == NULL) {
		matrix4_mb_test_fb_effect_destroy(e);
		free(e);
		return NULL;
	}
#else
	if (e_fir == NULL) {
		if (e_delay) e_delay->destroy(e_delay);
		matrix4_mb_effect_destroy(e);
		free(e);
		return NULL;
	}
	e->next = e_delay;
#endif
	e_fir->next = e;
	return e_fir;
}
