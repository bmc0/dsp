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
static const double fb_freqs[] = { 250.0, 500.0, 1000.0, 2000.0, 4000.0 };
static const int fb_ap_idx[]   = { 3, 4, 1, 0, 0, 4 };
static const double fb_bp[2]   = { 125.0, 8000.0 };
#define PHASE_LIN_FILTER_LEN 15
#elif N_BANDS == 10
static const double fb_freqs[] = { 249.172, 437.245, 701.191, 1070.98, 1588.74, 2313.53, 3328.04, 4748.02, 6735.46 };
static const int fb_ap_idx[]   = { 5, 6, 7, 8, 3, 2, 1, 0, 2, 3, 0, 3, 7, 8, 5, 8 };
static const double fb_bp[2]   = { 125.0, 9500.0 };
#define PHASE_LIN_FILTER_LEN 16
#elif N_BANDS == 12
static const double fb_freqs[] = { 236.079, 381.191, 572.544, 824.554, 1156.29, 1592.89, 2167.43, 2923.48, 3918.34, 5227.44, 6950.02 };
static const int fb_ap_idx[]   = { 6, 7, 8, 9, 10, 4, 3, 2, 1, 0, 3, 4, 1, 0, 1, 4, 9, 10, 7, 6, 7, 10 };
static const double fb_bp[2]   = { 125.0, 9500.0 };
#define PHASE_LIN_FILTER_LEN 18
#else
#error "unsupported number of bands"
#endif

#define DO_FILTER_BANK_TEST 0

struct filter_bank {
	struct cap5_state f[LENGTH(fb_freqs)];
	struct ap2_state ap[LENGTH(fb_ap_idx)];
	struct biquad_state hp, lp;  /* applied to lowest and highest band of s_bp, respectively */
	sample_t s[N_BANDS], s_bp[N_BANDS];
};

struct matrix4_band {
	struct smooth_state sm;
	struct event_state ev;
	struct ewma_state drift[4];
	struct axes ax, ax_ev;
	double fl_boost, fr_boost;
	#if DOWNSAMPLE_FACTOR > 1
		double lsl_m[2], lsr_m[2], rsl_m[2], rsr_m[2];
	#endif
};

struct matrix4_mb_state {
	int s, c0, c1;
	char has_output, is_draining, disable, show_status, do_dir_boost;
	struct filter_bank fb[2];
	struct matrix4_band band[N_BANDS];
	sample_t **bufs;
	sample_t *fb_buf[2][N_BANDS];
	sample_t norm_mult, surr_mult;
	struct event_config evc;
	#if DOWNSAMPLE_FACTOR > 1
		double fl_boost[2], fr_boost[2];
	#else
		double fl_boost, fr_boost;
	#endif
	ssize_t len, p, drain_frames, fade_frames, fade_p;
};

static void filter_bank_init(struct filter_bank *fb, double fs)
{
	for (int i = 0; i < LENGTH(fb_freqs); ++i)
		cap5_init(&fb->f[i], fs, fb_freqs[i]);
	for (int i = 0; i < LENGTH(fb_ap_idx); ++i)
		fb->ap[i] = fb->f[fb_ap_idx[i]].a1;
	biquad_init_using_type(&fb->hp, BIQUAD_HIGHPASS, fs, fb_bp[0], 0.5, 0, 0, BIQUAD_WIDTH_Q);
	biquad_init_using_type(&fb->lp, BIQUAD_LOWPASS, fs, fb_bp[1], 0.5, 0, 0, BIQUAD_WIDTH_Q);
}

static void filter_bank_run(struct filter_bank *fb, sample_t s)
{
#if N_BANDS == 6
	cap5_run(&fb->f[2], s, &fb->s[2], &fb->s[3]);  /* split in the middle (xover 2) */
	fb->s[2] = ap2_run(&fb->ap[0], fb->s[2]);      /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[1], fb->s[2]);      /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[2], fb->s[3]);      /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[3], fb->s[3]);      /* xover 0 ap */

	cap5_run(&fb->f[1], fb->s[2], &fb->s[1], &fb->s[2]);  /* split at xover 1 */
	fb->s[2] = ap2_run(&fb->ap[4], fb->s[2]);             /* xover 0 ap */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]); /* split at xover 3 */
	fb->s[3] = ap2_run(&fb->ap[5], fb->s[3]);            /* xover 4 ap */

	cap5_run(&fb->f[0], fb->s[1], &fb->s[0], &fb->s[1]); /* split at xover 0 */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]); /* split at xover 4 */
#elif N_BANDS == 10
	cap5_run(&fb->f[4], s, &fb->s[4], &fb->s[5]);  /* split at xover 4 (1588.74Hz) */
	fb->s[4] = ap2_run(&fb->ap[0], fb->s[4]);  /* xover 5 ap */
	fb->s[4] = ap2_run(&fb->ap[1], fb->s[4]);  /* xover 6 ap */
	fb->s[4] = ap2_run(&fb->ap[2], fb->s[4]);  /* xover 7 ap */
	fb->s[4] = ap2_run(&fb->ap[3], fb->s[4]);  /* xover 8 ap */
	fb->s[5] = ap2_run(&fb->ap[4], fb->s[5]);  /* xover 3 ap */
	fb->s[5] = ap2_run(&fb->ap[5], fb->s[5]);  /* xover 2 ap */
	fb->s[5] = ap2_run(&fb->ap[6], fb->s[5]);  /* xover 1 ap */
	fb->s[5] = ap2_run(&fb->ap[7], fb->s[5]);  /* xover 0 ap */

	cap5_run(&fb->f[1], fb->s[4], &fb->s[1], &fb->s[2]);  /* split at xover 1 (437.245Hz) */
	fb->s[1] = ap2_run(&fb->ap[8], fb->s[1]);  /* xover 2 ap */
	fb->s[1] = ap2_run(&fb->ap[9], fb->s[1]);  /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[10], fb->s[2]);  /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[1], &fb->s[0], &fb->s[1]);  /* split at xover 0 (249.172Hz) */

	cap5_run(&fb->f[2], fb->s[2], &fb->s[2], &fb->s[3]);  /* split at xover 2 (701.191Hz) */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);  /* xover 3 ap */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (1070.98Hz) */

	cap5_run(&fb->f[6], fb->s[5], &fb->s[6], &fb->s[7]);  /* split at xover 6 (3328.04Hz) */
	fb->s[6] = ap2_run(&fb->ap[12], fb->s[6]);  /* xover 7 ap */
	fb->s[6] = ap2_run(&fb->ap[13], fb->s[6]);  /* xover 8 ap */
	fb->s[7] = ap2_run(&fb->ap[14], fb->s[7]);  /* xover 5 ap */

	cap5_run(&fb->f[5], fb->s[6], &fb->s[5], &fb->s[6]);  /* split at xover 5 (2313.53Hz) */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (4748.02Hz) */
	fb->s[7] = ap2_run(&fb->ap[15], fb->s[7]);  /* xover 8 ap */

	cap5_run(&fb->f[8], fb->s[8], &fb->s[8], &fb->s[9]);  /* split at xover 8 (6735.46Hz) */
#elif N_BANDS == 12
	cap5_run(&fb->f[5], s, &fb->s[5], &fb->s[6]);  /* split in the middle (xover 5) */
	fb->s[5] = ap2_run(&fb->ap[0], fb->s[5]);      /* xover 6 ap */
	fb->s[5] = ap2_run(&fb->ap[1], fb->s[5]);      /* xover 7 ap */
	fb->s[5] = ap2_run(&fb->ap[2], fb->s[5]);      /* xover 8 ap */
	fb->s[5] = ap2_run(&fb->ap[3], fb->s[5]);      /* xover 9 ap */
	fb->s[5] = ap2_run(&fb->ap[4], fb->s[5]);      /* xover 10 ap */
	fb->s[6] = ap2_run(&fb->ap[5], fb->s[6]);      /* xover 4 ap */
	fb->s[6] = ap2_run(&fb->ap[6], fb->s[6]);      /* xover 3 ap */
	fb->s[6] = ap2_run(&fb->ap[7], fb->s[6]);      /* xover 2 ap */
	fb->s[6] = ap2_run(&fb->ap[8], fb->s[6]);      /* xover 1 ap */
	fb->s[6] = ap2_run(&fb->ap[9], fb->s[6]);      /* xover 0 ap */

	cap5_run(&fb->f[2], fb->s[5], &fb->s[2], &fb->s[3]);  /* split at xover 2 */
	fb->s[2] = ap2_run(&fb->ap[10], fb->s[2]);            /* xover 3 ap */
	fb->s[2] = ap2_run(&fb->ap[11], fb->s[2]);            /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->ap[12], fb->s[3]);            /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->ap[13], fb->s[3]);            /* xover 0 ap */

	cap5_run(&fb->f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 */
	fb->s[0] = ap2_run(&fb->ap[14], fb->s[0]);            /* xover 1 ap */

	cap5_run(&fb->f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 */

	cap5_run(&fb->f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 */
	fb->s[3] = ap2_run(&fb->ap[15], fb->s[3]);            /* xover 4 ap */

	cap5_run(&fb->f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 */

	cap5_run(&fb->f[8], fb->s[6], &fb->s[8], &fb->s[9]);  /* split at xover 8 */
	fb->s[8] = ap2_run(&fb->ap[16], fb->s[8]);            /* xover 9 ap */
	fb->s[8] = ap2_run(&fb->ap[17], fb->s[8]);            /* xover 10 ap */
	fb->s[9] = ap2_run(&fb->ap[18], fb->s[9]);            /* xover 7 ap */
	fb->s[9] = ap2_run(&fb->ap[19], fb->s[9]);            /* xover 6 ap */

	cap5_run(&fb->f[6], fb->s[8], &fb->s[6], &fb->s[7]);  /* split at xover 6 */
	fb->s[6] = ap2_run(&fb->ap[20], fb->s[6]);            /* xover 7 ap */

	cap5_run(&fb->f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 */

	cap5_run(&fb->f[9], fb->s[9], &fb->s[9], &fb->s[10]);  /* split at xover 9 */
	fb->s[9] = ap2_run(&fb->ap[21], fb->s[9]);             /* xover 10 ap */

	cap5_run(&fb->f[10], fb->s[10], &fb->s[10], &fb->s[11]);  /* split at xover 10 */
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
		const double s0 = (ibuf) ? ibuf[i*e->istream.channels + state->c0] : 0.0;
		const double s1 = (ibuf) ? ibuf[i*e->istream.channels + state->c1] : 0.0;
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
		double fl_boost = 0.0, fr_boost = 0.0, f_boost_norm = 0.0;
		sample_t out_ls = 0.0, out_rs = 0.0;
		const sample_t s0 = (ibuf) ? ibuf[i*e->istream.channels + state->c0] : 0.0;
		const sample_t s1 = (ibuf) ? ibuf[i*e->istream.channels + state->c1] : 0.0;
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
			const sample_t s0_d_fb = state->fb_buf[0][k][state->p];
			const sample_t s1_d_fb = state->fb_buf[1][k][state->p];
			state->fb_buf[0][k][state->p] = state->fb[0].s[k];
			state->fb_buf[1][k][state->p] = state->fb[1].s[k];

			struct envs env, pwr_env;
			calc_input_envs(&band->sm, s0_bp, s1_bp, &env, &pwr_env);

			#if DOWNSAMPLE_FACTOR > 1
			if (state->s == 0) {
			#endif
				process_events(&band->ev, &state->evc, &env, &pwr_env, band->drift, &band->ax, &band->ax_ev);
				norm_axes(&band->ax);

				struct matrix_coefs m = {0};
				calc_matrix_coefs(&band->ax, state->do_dir_boost, norm_mult, surr_mult, &m);
				band->fl_boost = m.fl_boost;
				band->fr_boost = m.fr_boost;

				fl_boost += m.fl_boost * m.fl_boost * pwr_env.sum;
				fr_boost += m.fr_boost * m.fr_boost * pwr_env.sum;
				f_boost_norm += pwr_env.sum;

			#if DOWNSAMPLE_FACTOR > 1
				band->lsl_m[0] = band->lsl_m[1];
				band->lsr_m[0] = band->lsr_m[1];
				band->rsl_m[0] = band->rsl_m[1];
				band->rsr_m[0] = band->rsr_m[1];
				band->lsl_m[1] = m.lsl;
				band->lsr_m[1] = m.lsr;
				band->rsl_m[1] = m.rsl;
				band->rsr_m[1] = m.rsr;
			}
			out_ls += s0_d_fb*oversample(band->lsl_m, state->s) + s1_d_fb*oversample(band->lsr_m, state->s);
			out_rs += s0_d_fb*oversample(band->rsl_m, state->s) + s1_d_fb*oversample(band->rsr_m, state->s);
			#else
			out_ls += s0_d_fb*m.lsl + s1_d_fb*m.lsr;
			out_rs += s0_d_fb*m.rsl + s1_d_fb*m.rsr;
			#endif
		}

		#if DOWNSAMPLE_FACTOR > 1
		if (state->s == 0) {
		#endif
			if (f_boost_norm > 0.0) {
				fl_boost = sqrt(fl_boost / f_boost_norm);
				fr_boost = sqrt(fr_boost / f_boost_norm);
			}
			else {
				fl_boost = 0.0;
				fr_boost = 0.0;
			}
		#if DOWNSAMPLE_FACTOR > 1
			state->fl_boost[0] = state->fl_boost[1];
			state->fr_boost[0] = state->fr_boost[1];
			state->fl_boost[1] = fl_boost;
			state->fr_boost[1] = fr_boost;
		}
		const double ll_m = norm_mult + oversample(state->fl_boost, state->s);
		const double rr_m = norm_mult + oversample(state->fr_boost, state->s);
		#else
		state->fl_boost = fl_boost;
		state->fr_boost = fr_boost;
		const double ll_m = norm_mult + fl_boost;
		const double rr_m = norm_mult + fr_boost;
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
				state->bufs[k][state->p] = (ibuf) ? ibuf[i*e->istream.channels + k] : 0.0;
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
				state->bufs[k][state->p] = (ibuf) ? ibuf[i*e->istream.channels + k] : 0.0;
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
			for (i = 0; i < N_BANDS; ++i) {
				fprintf(stderr, "\n%s%s: band %zd: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); dir_boost: l:%05.3f r:%05.3f; adj: %05.3f; ord: %zd; diff: %zd; early: %zd\033[K\r",
					e->name, (state->disable) ? " [off]" : "", i,
					TO_DEGREES(state->band[i].ax.lr), TO_DEGREES(state->band[i].ax_ev.lr), TO_DEGREES(state->band[i].ax.cs), TO_DEGREES(state->band[i].ax_ev.cs),
					state->band[i].fl_boost, state->band[i].fr_boost, state->band[i].ev.adj, state->band[i].ev.ord_count, state->band[i].ev.diff_count, state->band[i].ev.early_count);
			}
			fprintf(stderr, "\n%s%s: weighted RMS dir_boost: l:%05.3f r:%05.3f\033[K\r",
				e->name, (state->disable) ? " [off]" : "",
				#if DOWNSAMPLE_FACTOR > 1
					state->fl_boost[1], state->fr_boost[1]);
				#else
					state->fl_boost, state->fr_boost);
				#endif
			fprintf(stderr, "\033[%zdA", i+1);
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
	for (i = 0; i < N_BANDS; ++i) {
		memset(state->fb_buf[0][i], 0, state->len * sizeof(sample_t));
		memset(state->fb_buf[1][i], 0, state->len * sizeof(sample_t));
	}
}

void matrix4_mb_effect_signal(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	state->disable = !state->disable;
	state->fade_p = state->fade_frames - state->fade_p;
	if (!state->show_status)
		LOG_FMT(LL_NORMAL, "%s: %s", e->name, (state->disable) ? "disabled" : "enabled");
}

void matrix4_mb_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
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
			e->run(e, frames, NULL, obuf);
		}
		else
			*frames = -1;
	}
}

void matrix4_mb_effect_destroy(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (int i = 0; i < e->istream.channels; ++i)
		free(state->bufs[i]);
	for (int i = 0; i < N_BANDS; ++i) {
		free(state->fb_buf[0][i]);
		free(state->fb_buf[1][i]);
	}
	free(state->bufs);
	#ifndef LADSPA_FRONTEND
		if (state->show_status) {
			for (int i = 0; i < N_BANDS+1; ++i) fprintf(stderr, "\033[K\n");
			fprintf(stderr, "\033[K\r\033[%dA", N_BANDS+1);
		}
	#endif
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
	e->drain = matrix4_mb_effect_drain;
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
		drift_init(state->band[k].drift, istream);
	}

	state->len = TIME_TO_FRAMES(DELAY_TIME, istream->fs);
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int i = 0; i < istream->channels; ++i)
		state->bufs[i] = calloc(state->len, sizeof(sample_t));
	for (int i = 0; i < N_BANDS; ++i) {
		state->fb_buf[0][i] = calloc(state->len, sizeof(sample_t));
		state->fb_buf[1][i] = calloc(state->len, sizeof(sample_t));
	}
	state->surr_mult = config.surr_mult;
	state->norm_mult = CALC_NORM_MULT(config.surr_mult);
	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, istream->fs);
	event_config_init(&state->evc, istream);
#endif
	filter_bank_init(&state->fb[0], istream->fs);
	state->fb[1] = state->fb[0];

#ifdef HAVE_FFTW3
	struct effect *e_fir = NULL;
	const ssize_t phase_lin_frames = (config.do_phase_lin) ? TIME_TO_FRAMES(PHASE_LIN_FILTER_LEN, istream->fs) : 1;
	if (config.do_phase_lin) {
		sample_t *phase_lin_filter = calloc(phase_lin_frames, sizeof(sample_t));
		filter_bank_run(&state->fb[1], 1.0);
		for (int k = 0; k < N_BANDS; ++k)
			phase_lin_filter[phase_lin_frames-1] += state->fb[1].s[k];
		for (int i = phase_lin_frames-2; i >= 0; --i) {
			filter_bank_run(&state->fb[1], 0.0);
			for (int k = 0; k < N_BANDS; ++k)
				phase_lin_filter[i] += state->fb[1].s[k];
		}
	#if DO_FILTER_BANK_TEST
		e_fir = fir_effect_init_with_filter(ei, istream, channel_selector, phase_lin_filter, 1, phase_lin_frames, 0);
	#else
		char *fir_channel_selector = NEW_SELECTOR(e->ostream.channels);
		SET_BIT(fir_channel_selector, istream->channels);
		SET_BIT(fir_channel_selector, istream->channels + 1);
		e_fir = fir_effect_init_with_filter(ei, &e->ostream, fir_channel_selector, phase_lin_filter, 1, phase_lin_frames, 0);
		free(fir_channel_selector);
	#endif
		free(phase_lin_filter);
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
