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
#include "ewma.h"
#include "biquad.h"
#include "cap5.h"
#include "util.h"
#ifdef HAVE_FFTW3
#include "fir.h"
#endif

#define DOWNSAMPLE_FACTOR   8
#define EVENT_THRESH      3.1
#define N_BANDS            10

#include "matrix4_common.h"

#if N_BANDS == 6
static const double fb_freqs[]   = { 300, 685.65, 1352.5, 2505.8, 4500 };
static const int    fb_ap_idx[]  = { 3, 4, 1, 0, 1, 4 };
static const double fb_bp[2]     = { 120, 8000 };  /* Q=0.7071 */
static const double fb_weights[] = { 0.191, 0.82, 1.22, 0.768, 1.4, 0.459 };
#elif N_BANDS == 10
static const double fb_freqs[]   = { 249, 439.14, 704.95, 1076.5, 1596, 2322.1, 3337.2, 4756.3, 6740 };
static const int    fb_ap_idx[]  = { 5, 6, 7, 8, 3, 2, 1, 0, 2, 3, 0, 3, 7, 8, 5, 8 };
static const double fb_bp[2]     = { 120, 9500 };  /* Q=0.7071 */
static const double fb_weights[] = { 0.146, 0.527, 1.02, 1.38, 0.774, 0.76, 1.4, 1.3, 0.568, 0.151 };
#elif N_BANDS == 11
static const double fb_freqs[]   = { 175, 329.29, 542.52, 837.21, 1244.5, 1807.4, 2585.3, 3660.5, 5146.4, 7200 };
static const int    fb_ap_idx[]  = { 5, 6, 7, 8, 9, 3, 2, 1, 0, 2, 3, 0, 3, 8, 9, 6, 5, 6, 9 };
static const double fb_bp[2]     = { 85, 10000 };  /* Q=0.7071 */
static const double fb_weights[] = { 0.0662, 0.31, 0.74, 1.24, 1.24, 0.616, 0.97, 1.47, 1.15, 0.427, 0.139 };
#elif N_BANDS == 12
static const double fb_freqs[]   = { 159, 305.57, 507.52, 785.8, 1169.2, 1697.6, 2425.6, 3428.8, 4811, 6715.6, 9340 };
static const int    fb_ap_idx[]  = { 6, 7, 8, 9, 10, 4, 3, 2, 1, 0, 3, 4, 1, 0, 1, 4, 9, 10, 7, 6, 7, 10 };
static const double fb_bp[2]     = { 80, 13000 };  /* Q=0.7071 */
static const double fb_weights[] = { 0.0536, 0.265, 0.675, 1.17, 1.32, 0.668, 0.849, 1.44, 1.27, 0.559, 0.145, 0.176 };
#else
#error "unsupported number of bands"
#endif

#define PHASE_LIN_MAX_LEN 30.0  /* maximum filter length in milliseconds */
#define PHASE_LIN_THRESH  1e-5  /* truncation threshold */

#define DO_FILTER_BANK_TEST 0

struct filter_bank_frame {
	sample_t s[N_BANDS];
};

struct filter_bank {
	struct cap5_state f[LENGTH(fb_freqs)];
	struct ap2_state ap[LENGTH(fb_ap_idx)];
	struct biquad_state hp, lp;  /* applied to lowest and highest band of s_bp, respectively */
	sample_t s[N_BANDS], s_bp[N_BANDS];
};

struct matrix4_band {
	struct smooth_state sm;
	struct event_state ev;
	struct axes ax, ax_ev;
	double dir_boost;
	#if DOWNSAMPLE_FACTOR > 1
		struct cs_interp_state lsl_m, lsr_m;
		struct cs_interp_state rsl_m, rsr_m;
	#endif
};

struct matrix4_mb_state {
	int s, c0, c1;
	char has_output, is_draining, disable, show_status, do_dir_boost;
	struct filter_bank fb[2];
	struct matrix4_band band[N_BANDS];
	sample_t **bufs;
	struct filter_bank_frame *fb_buf[2];
	sample_t norm_mult, surr_mult;
	struct event_config evc;
	#if DOWNSAMPLE_FACTOR > 1
		struct cs_interp_state dir_boost;
	#else
		double dir_boost;
	#endif
	struct smf_state dir_boost_smooth;
	ssize_t len, p, drain_frames, fade_frames, fade_p;
};

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
	for (int i = 0; i < LENGTH(fb_freqs); ++i)
		cap5_init(&fb->f[i], fs, fb_freqs[i], ap);
	for (int i = 0; i < LENGTH(fb_ap_idx); ++i)
		fb->ap[i] = fb->f[fb_ap_idx[i]].a1;
	biquad_init_using_type(&fb->hp, BIQUAD_HIGHPASS, fs, fb_bp[0], 0.7071, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&fb->lp, BIQUAD_LOWPASS, fs, fb_bp[1], 0.7071, 0, 0, BIQUAD_WIDTH_Q);
}

static void filter_bank_run(struct filter_bank *fb, sample_t s)
{
#if N_BANDS == 6
	cap5_run(&fb->f[2], s, &fb->s[2], &fb->s[3]);  /* split at xover 2 (1352.5Hz) */
	fb->s[2] = ap2_run(&fb->ap[0], fb->s[2]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[1], fb->s[2]);  /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[2], fb->s[3]);  /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[3], fb->s[3]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 (300Hz) */
	fb->s[0] = ap2_run(&fb->ap[4], fb->s[0]);  /* xover 1 ap */

	cap5_run(&fb->f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 (685.65Hz) */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (2505.8Hz) */
	fb->s[3] = ap2_run(&fb->ap[5], fb->s[3]);  /* xover 4 ap */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 (4500Hz) */
#elif N_BANDS == 10
	cap5_run(&fb->f[4], s, &fb->s[4], &fb->s[5]);  /* split at xover 4 (1596Hz) */
	fb->s[4] = ap2_run(&fb->ap[0], fb->s[4]);  /* xover 5 ap */
	fb->s[4] = ap2_run(&fb->ap[1], fb->s[4]);  /* xover 6 ap */
	fb->s[4] = ap2_run(&fb->ap[2], fb->s[4]);  /* xover 7 ap */
	fb->s[4] = ap2_run(&fb->ap[3], fb->s[4]);  /* xover 8 ap */
	fb->s[5] = ap2_run(&fb->ap[4], fb->s[5]);  /* xover 3 ap */
	fb->s[5] = ap2_run(&fb->ap[5], fb->s[5]);  /* xover 2 ap */
	fb->s[5] = ap2_run(&fb->ap[6], fb->s[5]);  /* xover 1 ap */
	fb->s[5] = ap2_run(&fb->ap[7], fb->s[5]);  /* xover 0 ap */

	cap5_run(&fb->f[1], fb->s[4], &fb->s[1], &fb->s[2]);  /* split at xover 1 (439.14Hz) */
	fb->s[1] = ap2_run(&fb->ap[8], fb->s[1]);  /* xover 2 ap */
	fb->s[1] = ap2_run(&fb->ap[9], fb->s[1]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[10], fb->s[2]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[1], &fb->s[0], &fb->s[1]);  /* split at xover 0 (249Hz) */

	cap5_run(&fb->f[2], fb->s[2], &fb->s[2], &fb->s[3]);  /* split at xover 2 (704.95Hz) */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 3 ap */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (1076.5Hz) */

	cap5_run(&fb->f[6], fb->s[5], &fb->s[6], &fb->s[7]);  /* split at xover 6 (3337.2Hz) */
	fb->s[6] = ap2_run(&fb->ap[12], fb->s[6]);  /* xover 7 ap */
	fb->s[6] = ap2_run(&fb->ap[13], fb->s[6]);  /* xover 8 ap */
	fb->s[7] = ap2_run(&fb->ap[14], fb->s[7]);  /* xover 5 ap */

	cap5_run(&fb->f[5], fb->s[6], &fb->s[5], &fb->s[6]);  /* split at xover 5 (2322.1Hz) */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (4756.3Hz) */
	fb->s[7] = ap2_run(&fb->ap[15], fb->s[7]);  /* xover 8 ap */

	cap5_run(&fb->f[8], fb->s[8], &fb->s[8], &fb->s[9]);  /* split at xover 8 (6740Hz) */
#elif N_BANDS == 11
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
	cap5_run(&fb->f[5], s, &fb->s[5], &fb->s[6]);  /* split at xover 5 (1697.6Hz) */
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

	cap5_run(&fb->f[2], fb->s[5], &fb->s[2], &fb->s[3]);  /* split at xover 2 (507.52Hz) */
	fb->s[2] = ap2_run(&fb->ap[10], fb->s[2]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[12], fb->s[3]);  /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[13], fb->s[3]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 (159Hz) */
	fb->s[0] = ap2_run(&fb->ap[14], fb->s[0]);  /* xover 1 ap */

	cap5_run(&fb->f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 (305.57Hz) */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (785.8Hz) */
	fb->s[3] = ap2_run(&fb->ap[15], fb->s[3]);  /* xover 4 ap */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 (1169.2Hz) */

	cap5_run(&fb->f[8], fb->s[6], &fb->s[8], &fb->s[9]);  /* split at xover 8 (4811Hz) */
	fb->s[8] = ap2_run(&fb->ap[16], fb->s[8]);  /* xover 9 ap */
	fb->s[8] = ap2_run(&fb->ap[17], fb->s[8]);  /* xover 10 ap */
	fb->s[9] = ap2_run(&fb->ap[18], fb->s[9]);  /* xover 7 ap */
	fb->s[9] = ap2_run(&fb->ap[19], fb->s[9]);  /* xover 6 ap */

	cap5_run(&fb->f[6], fb->s[8], &fb->s[6], &fb->s[7]);  /* split at xover 6 (2425.6Hz) */
	fb->s[6] = ap2_run(&fb->ap[20], fb->s[6]);  /* xover 7 ap */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (3428.8Hz) */

	cap5_run(&fb->f[9], fb->s[9], &fb->s[9], &fb->s[10]);  /* split at xover 9 (6715.6Hz) */
	fb->s[9] = ap2_run(&fb->ap[21], fb->s[9]);  /* xover 10 ap */

	cap5_run(&fb->f[10], fb->s[10], &fb->s[10], &fb->s[11]);  /* split at xover 10 (9340Hz) */
#endif

	fb->s_bp[0] = biquad(&fb->hp, fb->s[0]);
	for (int i = 1; i < N_BANDS-1; ++i)
		fb->s_bp[i] = fb->s[i];
	fb->s_bp[N_BANDS-1] = biquad(&fb->lp, fb->s[N_BANDS-1]);
}

#if DO_FILTER_BANK_TEST
sample_t * matrix4_mb_test_fb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		const double s0 = ibuf[i*e->istream.channels + state->c0];
		const double s1 = ibuf[i*e->istream.channels + state->c1];
		filter_bank_run(&state->fb[0], s0);
		filter_bank_run(&state->fb[1], s1);
		double out_l = 0.0, out_r = 0.0;
		for (int k = 0; k < N_BANDS; ++k) {
			out_l += state->fb[0].s[k];
			out_r += state->fb[1].s[k];
		}
		for (int k = 0; k < e->istream.channels; ++k) {
			if (k == state->c0)
				obuf[i*e->ostream.channels + k] = out_l;
			else if (k == state->c1)
				obuf[i*e->ostream.channels + k] = out_r;
			else
				obuf[i*e->ostream.channels + k] = ibuf[i*e->istream.channels + k];
		}
		for (int k = 0; k < N_BANDS; ++k)
			obuf[i*e->ostream.channels + e->istream.channels + k] = state->fb[0].s[k];
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
	ssize_t i, k, oframes = 0;
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;

	for (i = 0; i < *frames; ++i) {
		double norm_mult = state->norm_mult, surr_mult = state->surr_mult;
		double dir_boost = 0.0, dir_boost_norm = 0.0;
		sample_t out_ls = 0.0, out_rs = 0.0;
		const sample_t s0 = ibuf[i*e->istream.channels + state->c0];
		const sample_t s1 = ibuf[i*e->istream.channels + state->c1];
		const sample_t s0_d = state->bufs[state->c0][state->p];
		const sample_t s1_d = state->bufs[state->c1][state->p];

		if (state->fade_p > 0) {
			surr_mult *= fade_mult(state->fade_p, state->fade_frames, state->disable);
			norm_mult = CALC_NORM_MULT(surr_mult);
			--state->fade_p;
		}
		else if (state->disable) {
			norm_mult = 1.0;
			surr_mult = 0.0;
		}

		filter_bank_run(&state->fb[0], s0);
		filter_bank_run(&state->fb[1], s1);
		#if DOWNSAMPLE_FACTOR > 1
			state->s = (state->s + 1 >= DOWNSAMPLE_FACTOR) ? 0 : state->s + 1;
		#endif
		for (k = 0; k < N_BANDS; ++k) {
			struct matrix4_band *band = &state->band[k];

			const sample_t s0_bp = state->fb[0].s_bp[k];
			const sample_t s1_bp = state->fb[1].s_bp[k];
			const sample_t s0_d_fb = state->fb_buf[0][state->p].s[k];
			const sample_t s1_d_fb = state->fb_buf[1][state->p].s[k];
			state->fb_buf[0][state->p].s[k] = state->fb[0].s[k];
			state->fb_buf[1][state->p].s[k] = state->fb[1].s[k];

			struct envs env, pwr_env;
			calc_input_envs(&band->sm, s0_bp, s1_bp, &env, &pwr_env);

			#if DOWNSAMPLE_FACTOR > 1
			if (state->s == 0) {
			#endif
				const struct envs pwr_env_d = band->ev.pwr_env_buf[band->ev.buf_p];
				process_events(&band->ev, &state->evc, &env, &pwr_env, &band->ax, &band->ax_ev);
				norm_axes(&band->ax);

				struct matrix_coefs m = {0};
				calc_matrix_coefs(&band->ax, state->do_dir_boost, norm_mult, surr_mult, &m);
				band->dir_boost = m.dir_boost;

				const double weight = pwr_env_d.sum * fb_weights[k];
				dir_boost += m.dir_boost * m.dir_boost * weight;
				dir_boost_norm += weight;

			#if DOWNSAMPLE_FACTOR > 1
				cs_interp_insert(&band->lsl_m, m.lsl);
				cs_interp_insert(&band->lsr_m, m.lsr);
				cs_interp_insert(&band->rsl_m, m.rsl);
				cs_interp_insert(&band->rsr_m, m.rsr);
			}
			out_ls += s0_d_fb*cs_interp(&band->lsl_m, state->s) + s1_d_fb*cs_interp(&band->lsr_m, state->s);
			out_rs += s0_d_fb*cs_interp(&band->rsl_m, state->s) + s1_d_fb*cs_interp(&band->rsr_m, state->s);
			#else
			out_ls += s0_d_fb*m.lsl + s1_d_fb*m.lsr;
			out_rs += s0_d_fb*m.rsl + s1_d_fb*m.rsr;
			#endif
		}

		#if DOWNSAMPLE_FACTOR > 1
		if (state->s == 0) {
		#endif
			dir_boost = smf_asym_run(&state->dir_boost_smooth,
				(dir_boost_norm > 0.0) ? sqrt(dir_boost / dir_boost_norm) : 0.0);
		#if DOWNSAMPLE_FACTOR > 1
			cs_interp_insert(&state->dir_boost, dir_boost);
		}
		const double dir_boost_interp = cs_interp(&state->dir_boost, state->s);
		const double ll_m = norm_mult + dir_boost_interp;
		const double rr_m = norm_mult + dir_boost_interp;
		#else
		state->dir_boost = dir_boost;
		const double ll_m = norm_mult + dir_boost;
		const double rr_m = norm_mult + dir_boost;
		#endif
		const double lr_m = 0.0, rl_m = 0.0;

		const sample_t out_l = s0_d*ll_m + s1_d*lr_m;
		const sample_t out_r = s0_d*rl_m + s1_d*rr_m;

		if (state->has_output) {
			for (k = 0; k < e->istream.channels; ++k) {
				if (k == state->c0)
					obuf[oframes*e->ostream.channels + k] = out_l;
				else if (k == state->c1)
					obuf[oframes*e->ostream.channels + k] = out_r;
				else
					obuf[oframes*e->ostream.channels + k] = state->bufs[k][state->p];
				state->bufs[k][state->p] = ibuf[i*e->istream.channels + k];
			}
			obuf[oframes*e->ostream.channels + k + 0] = out_ls;
			obuf[oframes*e->ostream.channels + k + 1] = out_rs;
			++oframes;
		}
		else {
			for (k = 0; k < e->istream.channels; ++k) {
				#ifdef SYMMETRIC_IO
					obuf[oframes*e->ostream.channels + k] = 0.0;
				#endif
				state->bufs[k][state->p] = ibuf[i*e->istream.channels + k];
			}
			#ifdef SYMMETRIC_IO
				obuf[oframes*e->ostream.channels + k + 0] = 0.0;
				obuf[oframes*e->ostream.channels + k + 1] = 0.0;
				++oframes;
			#endif
		}
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
		if (state->p == 0)
			state->has_output = 1;
	}
	#ifndef LADSPA_FRONTEND
		/* TODO: Implement a proper way for effects to show status lines. */
		if (state->show_status) {
			dsp_log_acquire();
			for (i = 0; i < N_BANDS; ++i) {
				dsp_log_printf("\n%s%s: band %zd: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); dir_boost: %05.3f; adj: %05.3f; ord: %zd; diff: %zd; early: %zd\033[K\r",
					e->name, (state->disable) ? " [off]" : "", i,
					TO_DEGREES(state->band[i].ax.lr), TO_DEGREES(state->band[i].ax_ev.lr), TO_DEGREES(state->band[i].ax.cs), TO_DEGREES(state->band[i].ax_ev.cs),
					state->band[i].dir_boost, state->band[i].ev.adj, state->band[i].ev.ord_count, state->band[i].ev.diff_count, state->band[i].ev.early_count);
			}
			dsp_log_printf("\n%s%s: weighted RMS dir_boost: %05.3f\033[K\r",
				e->name, (state->disable) ? " [off]" : "",
				#if DOWNSAMPLE_FACTOR > 1
					CS_INTERP_PEEK(&state->dir_boost));
				#else
					state->dir_boost);
				#endif
			dsp_log_printf("\033[%zdA", i+1);
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
	state->has_output = 0;
	for (i = 0; i < e->istream.channels; ++i)
		memset(state->bufs[i], 0, state->len * sizeof(sample_t));
	memset(state->fb_buf[0], 0, state->len * sizeof(struct filter_bank_frame));
	memset(state->fb_buf[1], 0, state->len * sizeof(struct filter_bank_frame));
}

void matrix4_mb_effect_signal(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	state->disable = !state->disable;
	state->fade_p = state->fade_frames - state->fade_p;
	if (!state->show_status)
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
		if (state->show_status) {
			dsp_log_acquire();
			for (int i = 0; i < N_BANDS+1; ++i) dsp_log_printf("\033[K\n");
			dsp_log_printf("\033[K\r\033[%dA", N_BANDS+1);
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
	if (parse_effect_opts(argv, istream, &config))
		return NULL;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
#if DO_FILTER_BANK_TEST
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + N_BANDS;
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
	state->show_status = config.show_status;
	state->do_dir_boost = config.do_dir_boost;
	e->signal = (config.enable_signal) ? matrix4_mb_effect_signal : NULL;

	for (int k = 0; k < N_BANDS; ++k) {
		smooth_state_init(&state->band[k].sm, istream);
		event_state_init(&state->band[k].ev, istream);
	}
	smf_asym_init(&state->dir_boost_smooth, DOWNSAMPLED_FS(istream->fs),
		SMF_RISE_TIME(DIR_BOOST_RT0), DIR_BOOST_SENS_RISE, DIR_BOOST_SENS_FALL);

#if DOWNSAMPLE_FACTOR > 1
	state->len = TIME_TO_FRAMES(DELAY_TIME, istream->fs) + CS_INTERP_DELAY_FRAMES;
#else
	state->len = TIME_TO_FRAMES(DELAY_TIME, istream->fs);
#endif
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int i = 0; i < istream->channels; ++i)
		state->bufs[i] = calloc(state->len, sizeof(sample_t));
	state->fb_buf[0] = calloc(state->len, sizeof(struct filter_bank_frame));
	state->fb_buf[1] = calloc(state->len, sizeof(struct filter_bank_frame));
	state->surr_mult = config.surr_mult;
	state->norm_mult = CALC_NORM_MULT(config.surr_mult);
	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, istream->fs);
	event_config_init(&state->evc, istream);
#endif
	filter_bank_init(&state->fb[0], istream->fs, config.fb_type, config.fb_stop);
	state->fb[1] = state->fb[0];

#ifdef HAVE_FFTW3
	struct effect *e_fir = NULL;
	ssize_t phase_lin_frames = 1;
	if (config.do_phase_lin) {
		phase_lin_frames = TIME_TO_FRAMES(PHASE_LIN_MAX_LEN, istream->fs);
		sample_t *filter = calloc(phase_lin_frames, sizeof(sample_t));
		for (int i = phase_lin_frames-1; i >= 0; --i) {
			filter_bank_run(&state->fb[1], (i == phase_lin_frames-1) ? 1.0 : 0.0);
			for (int k = 0; k < N_BANDS; ++k)
				filter[i] += state->fb[1].s[k];
		}
		int zx = 0;                      /* last zero crossing index */
		double integ = fabs(filter[0]);  /* unsigned integral since last zero crossing */
		for (int k = 1; integ < PHASE_LIN_THRESH; ++k) {
			if (signbit(filter[k]) != signbit(filter[k-1])) {
				zx = k;
				integ = 0.0;
			}
			integ += fabs(filter[k]);
		}
		phase_lin_frames -= zx;
	#if DO_FILTER_BANK_TEST
		e_fir = fir_effect_init_with_filter(ei, istream, channel_selector, &filter[zx], 1, phase_lin_frames, 0);
	#else
		char *fir_channel_selector = NEW_SELECTOR(e->ostream.channels);
		SET_BIT(fir_channel_selector, istream->channels);
		SET_BIT(fir_channel_selector, istream->channels + 1);
		e_fir = fir_effect_init_with_filter(ei, &e->ostream, fir_channel_selector, &filter[zx], 1, phase_lin_frames, 0);
		free(fir_channel_selector);
	#endif
		free(filter);
		state->fb[1] = state->fb[0];  /* reset */
	}
#else
	const ssize_t phase_lin_frames = 1;
	if (config.do_phase_lin)
		LOG_FMT(LL_ERROR, "%s: warning: phase linearization not available", argv[0]);
#endif

#if !(DO_FILTER_BANK_TEST)
	const ssize_t surr_delay_frames = config.surr_delay_frames - phase_lin_frames + 1;
	struct effect *e_delay = matrix4_delay_effect_init(ei, &e->ostream, surr_delay_frames);
#endif

	e->data = state;
#ifdef HAVE_FFTW3
#if DO_FILTER_BANK_TEST
	if (e_fir) {
		e_fir->next = e;
		return e_fir;
	}
	return e;
#else
	if (e_fir) {
		e->next = e_fir;
		e_fir->next = e_delay;
	}
	else {
		e->next = e_delay;
	}
	return e;
#endif
#else
	e->next = e_delay;
	return e;
#endif
}
