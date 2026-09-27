/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "util.h"
#include "fir.h"
#include "fir_p.h"

#define DOWNSAMPLE_FACTOR 32
#define NORM_ACCOM_FACTOR 0.6
#define DIFF_OVERSHOOT    1.02
#include "matrix4_common.h"

#define BASE_ORD_NOTCH_SCALE_F0 700.0  /* -3dB point; Gaussian lowpass */
#define EVENT_THRESH_MAX 3.6
#define EVENT_THRESH_MIN 1.4

#define PHASE_LIN_TRUNC_THRESH 1e-6
#define DO_FILTER_BANK_TEST 0
#define FILTER_BANK_TEST_ANALYSIS 0
#define STATUS_SHOW_ALIGN 1

#if DEBUG_POWER_ERROR
	#include "smf.h"
#endif
#include "matrix4_filter_bank.c"

#define REPORT_EVENT_LEVELS 0
#define EVENT_TARGET_BASE   (EVENT_THRESH_MAX*0.15)
#define EVENT_TARGET_EXP1   0.17
#define EVENT_TARGET_EXP2   0.28

struct matrix4_mb_input_state {
	int c0, c1;
	ssize_t lookahead_frames;
};

struct ax_buf_frame {
	struct axes ax, ax_ev, ax_dpwr, diff_last, ev_dir;
	double pwrcmp_factor, ev_ds_diff, align_syn_dt, rs_scale;
	int ev_hold, ev_maybe, ev_unaligned, ev_realign;
};

struct ch_align_an_state {
	union {
		struct ap1_state ap;
		sample_t s[2];
	};
	int dt;
};

struct ch_align_syn_state {
	struct ap1_state ap[4];
	int dt[4];
};

struct xtc_state {
	struct ap1_state ap[2];
	sample_t g, (*buf)[2];
	int p, len;
};

struct matrix4_band {
	struct smooth_state sm;
	struct envs env, pwr_env;
	struct delay_est_state de;
	struct event_state ev;
	struct ax_buf_frame ax_f;
	struct ch_align_an_state align_an;
	struct ch_align_syn_state align_syn;
	struct {
		struct cs_interp_state ll, lr, rl, rr;
		struct cs_interp_state lsl, lsr, rsl, rsr;
		struct cs_interp_state sg, rg, xg;
		struct cs_interp_state amb, sdir, rdir;
	} m_interp;
	struct cs_interp_state pf_ap_c0[2];
	struct ap1_state pf_ap[2];
	struct direct_path_state dp;
	struct ewma_state ev_thresh;
	struct xtc_state xtc;
	struct ax_buf_frame *ax_buf;
	double ev_thresh_max, ev_thresh_min, contour, rear_shelf;
	double syn_dt, syn_tol_r, align_xfade_scale;
	int ax_buf_p, ax_buf_len, align_xfade_p;
#if DEBUG_POWER_ERROR
	struct ewma_state pwr_err[2];
	struct smf_state pwr_err_sm;
#endif
#if REPORT_EVENT_LEVELS
	double evl_sum_sq, evl_pk;
	ssize_t evl_samples;
	const struct filter_bank_params *fbp;
#endif
#ifdef DSP_STATUSLINES
	struct steering_bar lr_bar, cs_bar;
	struct statusline_state statusline;
#endif
};

struct matrix4_mb_state {
	int s, c0, c1, n_bands;
	char disable, do_phase_flip, do_direct_path, do_dpwr_decouple, have_rears;
	enum status_type status_type;
	enum channel_layout layout_id;
	struct fshape_state fshape[2];
	struct filter_bank fb;
	struct matrix4_band band[FB_MAX_BANDS];
	struct event_config evc;
	struct delay_est_hist de_hist;
	struct phase_flip_params pf_params;
	calc_matrix_coefs_func calc_matrix_coefs;
	sample_t *syn_buf;
	double matrix_adj[2], surr_mult[2], contour_pwrcmp;
	ssize_t drain_frames, latency_frames, align_latency_frames;
	int syn_buf_p, syn_buf_len, align_ref_syn, fade_frames, fade_p;
#if DEBUG_POWER_ERROR
	FILE *pwr_err_file;
#endif
#ifdef DSP_STATUSLINES
	int statuslines_registered;
#endif
};

#if DO_FILTER_BANK_TEST
static sample_t * matrix4_mb_test_fb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
	#if FILTER_BANK_TEST_ANALYSIS
		const double s_an[2] = {
			fshape_run(&state->fshape[0], ibuf[i*e->istream.channels + state->c0]),
			fshape_run(&state->fshape[1], ibuf[i*e->istream.channels + state->c1]),
		};
		filter_bank_run_an(&state->fb, s_an);
		for (int k = 0; k < state->n_bands; ++k) {
			state->fb.an.s[k][0] *= state->fb.an.mask[k]+1;
			state->fb.an.s[k][1] *= state->fb.an.mask[k]+1;
			if (state->fb.an.p & state->fb.an.mask[k])
				state->fb.an.s[k][1] = state->fb.an.s[k][0] = 0.0;
		}
	#else
		const double s_syn[2] = {
			ibuf[i*e->istream.channels + state->c0],
			ibuf[i*e->istream.channels + state->c1],
		};
		filter_bank_run_syn(&state->fb, s_syn);
	#endif
		double out_l = 0.0, out_r = 0.0, out_s = 0.0;
		for (int k = 0; k < state->n_bands; ++k) {
			struct matrix4_band *band = &state->band[k];
			const double ct1 = (band->contour-1.0)*state->contour_pwrcmp + 1.0;
			const double norm_mult = CALC_NORM_MULT(state->surr_mult[0]*ct1);
		#if FILTER_BANK_TEST_ANALYSIS
			out_l += state->fb.an.s[k][0]*norm_mult;
			out_r += state->fb.an.s[k][1]*norm_mult;
			out_s += state->fb.an.s[k][0]*norm_mult*state->surr_mult[0]*band->contour;
		#else
			out_l += state->fb.syn[0].s[k]*norm_mult;
			out_r += state->fb.syn[1].s[k]*norm_mult;
			out_s += state->fb.syn[0].s[k]*norm_mult*state->surr_mult[0]*band->contour;
		#endif
		}
		for (int k = 0; k < e->istream.channels; ++k) {
			if (k == state->c0)
				obuf[i*e->ostream.channels + k] = out_l;
			else if (k == state->c1)
				obuf[i*e->ostream.channels + k] = out_r;
			else
				obuf[i*e->ostream.channels + k] = ibuf[i*e->istream.channels + k];
		}
		for (int k = 0; k < state->n_bands; ++k)
		#if FILTER_BANK_TEST_ANALYSIS
			obuf[i*e->ostream.channels + e->istream.channels + k] = state->fb.an.s[k][0];
		#else
			obuf[i*e->ostream.channels + e->istream.channels + k] = state->fb.syn[0].s[k];
		#endif
		obuf[i*e->ostream.channels + e->istream.channels + state->n_bands] = out_s;
	}
	return obuf;
}

static void matrix4_mb_test_fb_effect_destroy(struct effect *e)
{
	free(e->data);
}

#else

static sample_t * matrix4_mb_input_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_input_state *state = (struct matrix4_mb_input_state *) e->data;
	const int in_ch = e->istream.channels, out_ch = e->ostream.channels;
	sample_t *ibuf_p = ibuf, *obuf_p = obuf;
	for (ssize_t i = 0; i < *frames; ++i) {
		memcpy(obuf_p, ibuf_p, sizeof(sample_t)*e->istream.channels);
		obuf_p[in_ch+0] = ibuf_p[state->c0];
		obuf_p[in_ch+1] = ibuf_p[state->c1];
		ibuf_p += in_ch;
		obuf_p += out_ch;
	}
	return obuf;
}

static void matrix4_mb_input_effect_destroy(struct effect *e)
{
	free(e->data);
}

static void matrix4_mb_input_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct matrix4_mb_input_state *state = (struct matrix4_mb_input_state *) e->data;
	req_delay[e->istream.channels+0] -= state->lookahead_frames;
	req_delay[e->istream.channels+1] -= state->lookahead_frames;
}

static struct effect * matrix4_mb_input_effect_init(const struct stream_info *istream,
	const char *channel_selector, int c0, int c1, ssize_t lookahead_frames)
{
	static const char *name = "matrix4_mb_input";
	struct matrix4_mb_input_state *state = NULL;

	struct effect *e = calloc(1, sizeof(struct effect));
	if (check_alloc(name, e)) goto fail;
	e->name = name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + 2;
	if (effect_set_channel_selector(e, channel_selector)) goto fail;

	state = calloc(1, sizeof(struct matrix4_mb_input_state));
	if (check_alloc(name, state)) goto fail;
	e->data = state;

	e->run = matrix4_mb_input_effect_run;
	e->destroy = matrix4_mb_input_effect_destroy;
	e->channel_offsets = matrix4_mb_input_effect_channel_offsets;

	state->c0 = c0;
	state->c1 = c1;
	state->lookahead_frames = lookahead_frames;

	return e;

	fail:
	destroy_effect(e);
	return NULL;
}

#define CH_ALIGN_LATENCY 1
static inline void ch_align_set(double dt, int *r_dt, struct ap1_state *ap)
{
	*r_dt = (int) (dt-0.1);
	const double dt_frac = dt-(*r_dt);
	ap->c0 = (1.0-dt_frac)/(1.0+dt_frac);
}

static void ch_align_an_state_init(struct ch_align_an_state *a, struct delay_est_state *de)
{
	if (de->buf) {
		ch_align_set(CH_ALIGN_LATENCY, &a->dt, &a->ap);
		ap1_reset(&a->ap);
	}
	else a->s[1] = a->s[0] = 0.0;
}

static inline void ch_align_an_set(struct ch_align_an_state *a, struct delay_est_state *de, double dt)
{
	if (!de->buf) return;
	ch_align_set(fabs(dt)+CH_ALIGN_LATENCY, &a->dt, &a->ap);
	if (signbit(dt)) a->dt = -a->dt;
	ap1_reset(&a->ap);
}

static inline void ch_align_an_run(struct ch_align_an_state *a, struct delay_est_state *de, sample_t *s0, sample_t *s1)
{
	if (de->buf) {
		const int p = de->p-1;
		if (a->dt < 0) {
			*s0 = de->buf[CBUF_WRAP(p-CH_ALIGN_LATENCY, de->len)][0];
			*s1 = ap1_run(&a->ap, de->buf[CBUF_WRAP(p+a->dt, de->len)][1]);
		}
		else {
			*s0 = ap1_run(&a->ap, de->buf[CBUF_WRAP(p-a->dt, de->len)][0]);
			*s1 = de->buf[CBUF_WRAP(p-CH_ALIGN_LATENCY, de->len)][1];
		}
	}
	else {
		sample_t rs[2] = { a->s[0], a->s[1] };
		a->s[0] = *s0; a->s[1] = *s1;
		*s0 = rs[0]; *s1 = rs[1];
	}
}

static void ch_align_syn_state_init(struct ch_align_syn_state *a, int ref)
{
	for (int i = 0; i < 4; ++i) {
		ch_align_set(ref, &a->dt[i], &a->ap[i]);
		ap1_reset(&a->ap[i]);
	}
}

static inline void ch_align_syn_xfade_start(struct ch_align_syn_state *a, int ref, double dt)
{
	a->ap[0] = a->ap[2]; a->ap[1] = a->ap[3];
	a->dt[0] = a->dt[2]; a->dt[1] = a->dt[3];
	ch_align_set(ref-dt, &a->dt[2], &a->ap[2]);
	ch_align_set(ref+dt, &a->dt[3], &a->ap[3]);
	ap1_reset(&a->ap[2]); ap1_reset(&a->ap[3]);
}

static inline sample_t * syn_buf_get_frame(struct matrix4_mb_state *state, int dt)
{
	int p = state->syn_buf_p - dt;
	if (p < 0) p += state->syn_buf_len;
	return state->syn_buf + p*state->n_bands*2;
}

static inline void ch_align_syn_run(struct ch_align_syn_state *a, struct matrix4_mb_state *state,
	int k, double t, sample_t *s0, sample_t *s1)
{
	const sample_t s_dt2 = syn_buf_get_frame(state, a->dt[2])[state->n_bands+k];
	const sample_t s_dt3 = syn_buf_get_frame(state, a->dt[3])[k];
	*s0 = ap1_run(&a->ap[2], s_dt2);
	*s1 = ap1_run(&a->ap[3], s_dt3);
	if (t < 1.0) {
		const sample_t s_dt0 = syn_buf_get_frame(state, a->dt[0])[state->n_bands+k];
		const sample_t s_dt1 = syn_buf_get_frame(state, a->dt[1])[k];
		*s0 = (*s0)*t + ap1_run(&a->ap[0], s_dt0)*(1.0-t);
		*s1 = (*s1)*t + ap1_run(&a->ap[1], s_dt1)*(1.0-t);
	}
}

#define XTC_FTZ ((2e3*DBL_MIN)/DBL_EPSILON)
static int xtc_state_init(struct xtc_state *xtc, double itd, double ild)
{
	memset(xtc, 0, sizeof(struct xtc_state));
	if (ild < 1e-3) return 0;
	const int itd_int = (int) (itd-0.1);
	const double itd_frac = itd-itd_int;
	xtc->ap[0].c0 = (1.0-itd_frac)/(1.0+itd_frac);
	xtc->ap[0].o0 = xtc->ap[0].i0 = XTC_FTZ;
	xtc->ap[1] = xtc->ap[0];
	xtc->g = ild;
	xtc->len = itd_int;
	xtc->buf = calloc(xtc->len, sizeof(double [2]));
	if (check_alloc(__func__, xtc->buf)) return 1;
	//LOG_FMT(LL_VERBOSE, "%s: itd=%g ild=%g", __func__, itd, ild);
	return 0;
}

static inline void xtc_run(struct xtc_state *xtc, double g, double *l, double *r)
{
	const double s[2] = { *l, *r }, d[2] = {
		(ap1_run(&xtc->ap[0], xtc->buf[xtc->p][0]+XTC_FTZ)-XTC_FTZ)*xtc->g,
		(ap1_run(&xtc->ap[1], xtc->buf[xtc->p][1]+XTC_FTZ)-XTC_FTZ)*xtc->g,
	};
	xtc->buf[xtc->p][0] = s[0]*g-d[1]; *l = s[0]-d[1];
	xtc->buf[xtc->p][1] = s[1]*g-d[0]; *r = s[1]-d[0];
	xtc->p = CBUF_NEXT(xtc->p, xtc->len);
}

static sample_t * matrix4_mb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		sample_t out_l = 0.0, out_r = 0.0, out_ls = 0.0, out_rs = 0.0, out_lr = 0.0, out_rr = 0.0;
		sample_t out_ls_dir = 0.0, out_rs_dir = 0.0, out_lr_dir = 0.0, out_rr_dir = 0.0;
		const sample_t s_syn[2] = {
			ibuf[i*e->istream.channels + state->c0],
			ibuf[i*e->istream.channels + state->c1],
		};
		const sample_t s_an[2] = {
			fshape_run(&state->fshape[0], ibuf[(i+1)*e->istream.channels-2]),
			fshape_run(&state->fshape[1], ibuf[(i+1)*e->istream.channels-1]),
		};
		filter_bank_run_syn(&state->fb, s_syn);
		filter_bank_run_an(&state->fb, s_an);
	#if DOWNSAMPLE_FACTOR > 1
		state->s = (state->s + 1 >= DOWNSAMPLE_FACTOR) ? 0 : state->s + 1;
	#endif

		for (int k = 0; k < state->n_bands; ++k) {
			struct matrix4_band *band = &state->band[k];
			if (!(state->fb.an.p & state->fb.an.mask[k])) {
				delay_est_insert(&band->de, state->fb.an.s[k][0], state->fb.an.s[k][1]);
				sample_t s0a_an = state->fb.an.s[k][0], s1a_an = state->fb.an.s[k][1];
				if (state->syn_buf) ch_align_an_run(&band->align_an, &band->de, &s0a_an, &s1a_an);
				calc_input_envs(&band->sm, s0a_an, s1a_an, &band->env, &band->pwr_env);
			}
		}

	#if DOWNSAMPLE_FACTOR > 1
		if (state->s == 0) {
	#else
		if (1) {
	#endif
			int n_angles = 0;
			struct {
				struct axes ax;
				double dt, tol_r;
			} angles[FB_MAX_BANDS];
			/* find bands with possible events */
			for (int k = 0; k < state->n_bands; ++k) {
				struct matrix4_band *band = &state->band[k];
				if (band->ax_f.ev_maybe) {
					angles[n_angles].ax = band->ax_f.diff_last;
					angles[n_angles].dt = band->syn_dt;
					angles[n_angles].tol_r = band->syn_tol_r;
					++n_angles;
				}
			}

			double cur_fade_mult = 1.0;
			if (state->fade_p > 0) {
				cur_fade_mult = fade_mult(state->fade_p, state->fade_frames, state->disable);
				--state->fade_p;
			}
			else if (state->disable) cur_fade_mult = 0.0;

			for (int k = 0; k < state->n_bands; ++k) {
				struct matrix4_band *band = &state->band[k];
				/* modulate event threshold based on the number of
				   bands with similar differential steering angles */
				double ev_thresh_fact = 0.0;
				if (band->ax_f.ev_maybe) {
					for (int j = 0; j < n_angles; ++j) {
						const double d_lr = fabs(angles[j].ax.lr - band->ax_f.diff_last.lr);
						const double d_cs = fabs(angles[j].ax.cs - band->ax_f.diff_last.cs);
						ev_thresh_fact += smoothstep(1.0 - MAXIMUM(d_lr, d_cs)*(16/M_PI)) *
							smoothstep(2.0-fabs(band->syn_dt-angles[j].dt)*MINIMUM(band->syn_tol_r, angles[j].tol_r));
					}
					ev_thresh_fact -= 1.0;
				}
				const double ev_thresh = ewma_run_set_max(&band->ev_thresh,
					band->ev_thresh_max - (band->ev_thresh_max-band->ev_thresh_min)*ev_thresh_fact/(state->n_bands-1));

				struct ax_buf_frame ax_f = {0};
				struct event_state *ev = &band->ev;
				process_events(ev, &state->evc, &band->env, &band->pwr_env, &band->de, &state->de_hist,
					ev_thresh*(1.0/EVENT_THRESH), &ax_f.ax, &ax_f.ax_ev, &ax_f.ax_dpwr);
				ax_f.diff_last = ev->diff_last;
				ax_f.ev_dir = ev->dir;
				ax_f.pwrcmp_factor = ewma_get_last(&ev->pwrcmp_factor);
				ax_f.ev_ds_diff = ev->ds_diff;
				ax_f.rs_scale = ewma_get_last(&ev->rs_scale);
				ax_f.ev_hold = !!ev->t_hold;
				ax_f.ev_maybe = ((ev->slope_last[0] > 0.0 && ev->last[0] > band->ev_thresh_min)
					|| (ev->slope_last[1] > 0.0 && ev->last[1] > band->ev_thresh_min));
				ax_f.ev_unaligned = ev->unaligned;
				ax_f.ev_realign = ev->realign;
				if (ax_f.ev_realign) {
					ax_f.align_syn_dt = ev->align_dt[0] * (state->fb.an.mask[k]+1);
					ch_align_an_set(&band->align_an, &band->de, ev->align_dt[0]);
					ev->realign = 0;
				}
				if (band->ax_buf_len > 0) {
					band->ax_f = band->ax_buf[band->ax_buf_p];
					band->ax_buf[band->ax_buf_p] = ax_f;
					band->ax_buf_p = CBUF_NEXT(band->ax_buf_p, band->ax_buf_len);
				}
				else band->ax_f = ax_f;
				double matrix_adj = state->matrix_adj[0];
				if (state->do_direct_path) {
					double r_pan[4];
					surr_direct_pan(&band->dp, band->ax_f.ev_hold, band->ax_f.ev_dir.cs, band->ax_f.ev_ds_diff,
						&band->ax_f.ax, band->ax_f.ev_unaligned, state->have_rears, r_pan);
					cs_interp_insert(&band->m_interp.amb, r_pan[0]);
					cs_interp_insert(&band->m_interp.sdir, r_pan[1]);
					if (state->have_rears) cs_interp_insert(&band->m_interp.rdir, r_pan[2]);
					matrix_adj = state->matrix_adj[0]*(1.0-r_pan[3]) + state->matrix_adj[1]*r_pan[3];
				}
				#if REPORT_EVENT_LEVELS
					if (band->evl_samples >= 0) {
						band->evl_sum_sq += ev->last[0]*ev->last[0];
						band->evl_sum_sq += ev->last[1]*ev->last[1];
						band->evl_pk = MAXIMUM(band->evl_pk, ev->last[0]);
						band->evl_pk = MAXIMUM(band->evl_pk, ev->last[1]);
					}
					band->evl_samples += 2;
				#endif

				const double w = smoothstep(band->ax_f.ax.cs*(-2/M_PI_4));
				const double rs_scale = band->ax_f.rs_scale, wct = w*rs_scale;
				const double sm_f[2] = { state->surr_mult[0]*cur_fade_mult, state->surr_mult[1]*cur_fade_mult };
				const double surr_mult_rear = rs_scale*sm_f[1] + (1.0-rs_scale)*sm_f[0];
				const double surr_mult = w*surr_mult_rear + (1.0-w)*sm_f[0];
				const double ct_pcf = state->contour_pwrcmp * band->ax_f.pwrcmp_factor;
				const double ct0 = wct + (1.0-wct)*band->contour;
				const double ct1 = (ct0-1.0)*ct_pcf + 1.0;
				const double ct2 = ct0/ct1;

				struct axes *ax_dpwr = (state->do_dpwr_decouple) ? &band->ax_f.ax_dpwr : &band->ax_f.ax;
				struct matrix_coefs m = {0};
				struct cmc_params cmc_p = {
					.surr_mult = { surr_mult*ct1, surr_mult_rear },
					.rs = rs_scale,
					.adj = matrix_adj,
				};
				state->calc_matrix_coefs(&band->ax_f.ax, ax_dpwr, &cmc_p, &m, NULL, 0);

				cs_interp_insert(&band->m_interp.ll, m.ll);
				cs_interp_insert(&band->m_interp.lr, m.lr);
				cs_interp_insert(&band->m_interp.rl, m.rl);
				cs_interp_insert(&band->m_interp.rr, m.rr);

				cs_interp_insert(&band->m_interp.lsl, m.lsl*ct2);
				cs_interp_insert(&band->m_interp.lsr, m.lsr*ct2);
				cs_interp_insert(&band->m_interp.rsl, m.rsl*ct2);
				cs_interp_insert(&band->m_interp.rsr, m.rsr*ct2);

				if (state->have_rears) {
					const double rsw = smoothstep(band->ax_f.ax.cs*(-2/M_PI_4)-1.0)*rs_scale;
					const double sg = rsw*band->rear_shelf + (1.0-rsw);
					const double s_norm = CALC_NORM_MULT(sg);
					cs_interp_insert(&band->m_interp.sg, sg*s_norm);
					cs_interp_insert(&band->m_interp.rg, s_norm);
				}
				if (band->xtc.buf) {
					const double xg = (band->ax_f.ax.cs > 0.0) ? sin(2.0*band->ax_f.ax.cs) : 0.0;
					cs_interp_insert(&band->m_interp.xg, xg*cur_fade_mult);
				}
				if (state->do_phase_flip) {
					const double pf_pos_rs = phase_flip_pos_rs(&band->ax_f.ax);
					cs_interp_insert(&band->pf_ap_c0[0], phase_flip_ap1_c0(&state->pf_params, 1.0-pf_pos_rs));
					cs_interp_insert(&band->pf_ap_c0[1], phase_flip_ap1_c0(&state->pf_params, pf_pos_rs));
				}
				if (state->syn_buf && band->ax_f.ev_realign) {
					band->syn_dt = band->ax_f.align_syn_dt;
					ch_align_syn_xfade_start(&band->align_syn, state->align_ref_syn, band->syn_dt);
					const double align_xfade_frames = e->istream.fs * (RISE_TIME_FAST*1.183/1000.0)/band->ax_f.ev_ds_diff;
					band->align_xfade_p = lrint(align_xfade_frames) + CS_INTERP_DELAY_FRAMES;
					band->align_xfade_scale = 1.0/align_xfade_frames;
				}
			}
		}

		sample_t *syn_ref = NULL;
		if (state->syn_buf) {
			sample_t *syn_f = state->syn_buf + state->syn_buf_p*state->n_bands*2;
			memcpy(syn_f, state->fb.syn[0].s, sizeof(sample_t)*state->n_bands);
			memcpy(syn_f + state->n_bands, state->fb.syn[1].s, sizeof(sample_t)*state->n_bands);
			syn_ref = syn_buf_get_frame(state, state->align_ref_syn);
		}
		for (int k = 0; k < state->n_bands; ++k) {
			struct matrix4_band *band = &state->band[k];
			sample_t s0_d_fb, s1_d_fb, s0x_d_fb, s1x_d_fb;
			if (syn_ref) {
				s0_d_fb = syn_ref[k];
				s1_d_fb = syn_ref[state->n_bands+k];
				double t = 1.0;
				if (band->align_xfade_p > 0) {
					const double x = band->align_xfade_p*band->align_xfade_scale;
					t = (x < 1.0) ? 1.0-x*x : 0.0;
					--band->align_xfade_p;
				}
				ch_align_syn_run(&band->align_syn, state, k, t, &s1x_d_fb, &s0x_d_fb);
			}
			else {
				s0x_d_fb = s0_d_fb = state->fb.syn[0].s[k];
				s1x_d_fb = s1_d_fb = state->fb.syn[1].s[k];
			}

			sample_t b_l = s0_d_fb*cs_interp(&band->m_interp.ll, state->s) + s1x_d_fb*cs_interp(&band->m_interp.lr, state->s);
			sample_t b_r = s0x_d_fb*cs_interp(&band->m_interp.rl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rr, state->s);
			sample_t b_ls = s0_d_fb*cs_interp(&band->m_interp.lsl, state->s) + s1x_d_fb*cs_interp(&band->m_interp.lsr, state->s);
			sample_t b_rs = s0x_d_fb*cs_interp(&band->m_interp.rsl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rsr, state->s);

		#if DEBUG_POWER_ERROR
		#ifdef DSP_STATUSLINES
			if (state->status_type || state->pwr_err_file) {
		#else
			if (state->pwr_err_file) {
		#endif
				const double pwr_in = ewma_run(&band->pwr_err[0], s0_d_fb*s0_d_fb + s1_d_fb*s1_d_fb);
				const double pwr_out = ewma_run(&band->pwr_err[1], b_l*b_l + b_r*b_r + b_ls*b_ls + b_rs*b_rs);
				const double pwr_ratio = MAXIMUM(pwr_out, DBL_MIN)/MAXIMUM(pwr_in, DBL_MIN);
				const double pwr_ratio_sm = smf_run(&band->pwr_err_sm, pwr_ratio);
				if (state->s == 0 && state->pwr_err_file)
					fprintf(state->pwr_err_file, "%.15e%c", pwr_ratio_sm, (k==state->n_bands-1)?'\n':' ');
			}
		#endif

			if (band->xtc.buf) xtc_run(&band->xtc, cs_interp(&band->m_interp.xg, state->s), &b_l, &b_r);
			out_l += b_l; out_r += b_r;
			sample_t b_ls_pf = b_ls, b_rs_pf = b_rs;
			if (state->do_phase_flip) {
				band->pf_ap[0].c0 = cs_interp(&band->pf_ap_c0[0], state->s);
				band->pf_ap[1].c0 = cs_interp(&band->pf_ap_c0[1], state->s);
				b_ls_pf = ap1_run(&band->pf_ap[0], b_ls_pf+1e-15)-1e-15;
				b_rs_pf = ap1_run(&band->pf_ap[1], b_rs_pf+1e-15)-1e-15;
			}
			sample_t b_lr = b_ls, b_rr = b_rs, b_lr_pf = 0.0, b_rr_pf = 0.0;
			if (state->have_rears) {
				const sample_t m_sg = cs_interp(&band->m_interp.sg, state->s);
				const sample_t m_rg = cs_interp(&band->m_interp.rg, state->s);
				b_lr_pf = b_ls_pf*m_rg; b_rr_pf = b_rs_pf*m_rg;
				b_ls_pf *= m_sg; b_rs_pf *= m_sg;
			}
			if (state->do_direct_path) {
				const sample_t m_amb = cs_interp(&band->m_interp.amb, state->s);
				const sample_t m_sdir = cs_interp(&band->m_interp.sdir, state->s);
				out_ls += b_ls_pf*m_amb; out_rs += b_rs_pf*m_amb;
				out_ls_dir += b_ls*m_sdir; out_rs_dir -= b_rs*m_sdir;
				if (state->have_rears) {
					const sample_t m_rdir = cs_interp(&band->m_interp.rdir, state->s);
					out_lr += b_lr_pf*m_amb; out_rr += b_rr_pf*m_amb;
					out_lr_dir += b_lr*m_rdir; out_rr_dir -= b_rr*m_rdir;
				}
			}
			else {
				out_ls += b_ls_pf; out_rs += b_rs_pf;
				out_lr += b_lr_pf; out_rr += b_rr_pf;
			}
		}

		sample_t *ibuf_p = &ibuf[i*e->istream.channels], *obuf_p = &obuf[i*e->ostream.channels];
		for (int k = 0; k < e->istream.channels-2; ++k) {
			if (k == state->c0) obuf_p[k] = out_l;
			else if (k == state->c1) obuf_p[k] = out_r;
			else obuf_p[k] = ibuf_p[k];
		}
		obuf_p += e->istream.channels-2;
		obuf_p[0] = out_ls;
		obuf_p[1] = out_rs;
		if (state->have_rears) {
			obuf_p[2] = out_lr;
			obuf_p[3] = out_rr;
			if (state->do_direct_path) {
				obuf_p[4] = out_ls_dir;
				obuf_p[5] = out_rs_dir;
				obuf_p[6] = out_lr_dir;
				obuf_p[7] = out_rr_dir;
			}
		}
		else if (state->do_direct_path) {
			obuf_p[2] = out_ls_dir;
			obuf_p[3] = out_rs_dir;
		}

		state->syn_buf_p = CBUF_NEXT(state->syn_buf_p, state->syn_buf_len);
	}
#ifdef DSP_STATUSLINES
	if (state->status_type) {
		dsp_statuslines_acquire();
		if (!state->statuslines_registered) {
			for (int i = 0; i < state->n_bands; ++i)
				dsp_statusline_register(&state->band[i].statusline);
			state->statuslines_registered = 1;
		}
		if (state->status_type == STATUS_TYPE_TEXT) {
			for (int i = 0; i < state->n_bands; ++i) {
				struct matrix4_band *band = &state->band[i];
				snprintf(band->statusline.s, LENGTH(band->statusline.s),
					"%s%s: band %2d: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); "
					"adj: %05.3f; thresh: %05.3f; pwrcmp: %05.3f; ord: %zd; diff: %zd; early: %zd; ign: %zd"
				#if STATUS_SHOW_ALIGN
					"; dt: %+.1fus%s"
				#endif
				#if DEBUG_POWER_ERROR
					"; pwr_err: %+05.2fdB"
				#endif
					, e->name, (state->disable) ? " [off]" : "", i,
					TO_DEGREES(band->ax_f.ax.lr), TO_DEGREES(band->ax_f.ax_ev.lr), TO_DEGREES(band->ax_f.ax.cs), TO_DEGREES(band->ax_f.ax_ev.cs),
					band->ev.adj, ewma_get_last(&band->ev_thresh), state->contour_pwrcmp*band->ax_f.pwrcmp_factor,
					band->ev.ord_count, band->ev.diff_count, band->ev.early_count, band->ev.ignore_count
				#if STATUS_SHOW_ALIGN
					, band->syn_dt*(1000.0*1000.0)/e->istream.fs, (band->ev.unaligned) ? "*" : ""
				#endif
				#if DEBUG_POWER_ERROR
					, 10.0*log10(smf_get_last(&band->pwr_err_sm))
				#endif
				);
			}
		}
		else {
			for (int i = 0; i < state->n_bands; ++i) {
				struct matrix4_band *band = &state->band[i];
				draw_steering_bar(band->ax_f.ax.lr, band->ax_f.ev_hold, &band->lr_bar);
				draw_steering_bar(band->ax_f.ax.cs, band->ax_f.ev_hold, &band->cs_bar);
				snprintf(band->statusline.s, LENGTH(band->statusline.s),
					"%s%s: band %2d: L[%s]R; C[%s]S; ord: %zd; diff: %zd; ign: %zd"
				#if STATUS_SHOW_ALIGN
					"; dt: %+.1fus%s"
				#endif
				#if DEBUG_POWER_ERROR
					"; pwr_err: %+05.2fdB"
				#endif
					, e->name, (state->disable) ? " [off]" : "", i,
					band->lr_bar.s, band->cs_bar.s, band->ev.ord_count, band->ev.diff_count, band->ev.ignore_count
				#if STATUS_SHOW_ALIGN
					, band->syn_dt*(1000.0*1000.0)/e->istream.fs, (band->ev.unaligned) ? "*" : ""
				#endif
				#if DEBUG_POWER_ERROR
					, 10.0*log10(smf_get_last(&band->pwr_err_sm))
				#endif
				);
			}
		}
		dsp_statuslines_release();
	}
#endif

	return obuf;
}

static void matrix4_mb_effect_reset(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	filter_bank_reset(&state->fb);
	if (state->syn_buf) {
		memset(state->syn_buf, 0, sizeof(sample_t)*state->syn_buf_len*state->n_bands*2);
		for (int i = 0; i < state->n_bands; ++i) state->band[i].align_xfade_p = 0;
	}
}

static void matrix4_mb_effect_signal(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	state->disable = !state->disable;
	state->fade_p = state->fade_frames - state->fade_p;
	if (!state->status_type)
		LOG_FMT(LL_NORMAL, "%s: %s", e->name, (state->disable) ? "disabled" : "enabled");
}

static void matrix4_mb_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	drain_samples[state->c0] += state->drain_frames; drain_samples[state->c1] += state->drain_frames;
	for (int i = e->istream.channels-2; i < e->ostream.channels; ++i) drain_samples[i] += state->drain_frames;
}

static void matrix4_mb_effect_destroy(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (int i = 0; i < state->n_bands; ++i) {
		delay_est_state_cleanup(&state->band[i].de);
		event_state_cleanup(&state->band[i].ev);
		free(state->band[i].ax_buf);
	}
	delay_est_hist_cleanup(&state->de_hist);
	free(state->syn_buf);
#if DEBUG_POWER_ERROR
	if (state->pwr_err_file)
		fclose(state->pwr_err_file);
#endif
#ifdef DSP_STATUSLINES
	if (state->statuslines_registered) {
		dsp_statuslines_acquire();
		for (int i = 0; i < state->n_bands; ++i)
			dsp_statusline_unregister(&state->band[i].statusline);
		dsp_statuslines_release();
	}
#endif
#if REPORT_EVENT_LEVELS
	if (state->band[0].evl_samples > 0) {
		dsp_log_acquire();
		dsp_log_printf("%s: %s: event trigger levels (dBr):\n", dsp_globals.prog_name, e->name);
		for (int i = 0; i < state->n_bands; ++i) {
			struct matrix4_band *band = &state->band[i];
			const double pk_dBr = 20.0*log10(band->evl_pk/band->ev_thresh_max);
			const double rms_dBr = 20.0*log10(sqrt(band->evl_sum_sq/band->evl_samples)/band->ev_thresh_max);
			dsp_log_printf("  band %d: peak: %g; RMS: %g; crest: %g\n", i, pk_dBr, rms_dBr, pk_dBr-rms_dBr);
		}
		dsp_log_printf("%s: %s: suggested thresh_scale:\n  {", dsp_globals.prog_name, e->name);
		for (int i = 0; i < state->n_bands; ++i) {
			struct matrix4_band *band = &state->band[i];
			const double norm_fc = band->fbp->fc[i]/700.0, ws = band->fbp->width_scale;
			const double rms_target = EVENT_TARGET_BASE*pow(1.0/(1.0+norm_fc*norm_fc), EVENT_TARGET_EXP1)/pow(ws, EVENT_TARGET_EXP2);
			dsp_log_printf(" %.3g%c", sqrt(band->evl_sum_sq/band->evl_samples)/rms_target, (i < state->n_bands-1) ? ',' : ' ');
		}
		dsp_log_puts("}\n");
		dsp_log_release();
	}
#endif
	free(state);
}

static void matrix4_mb_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	latency[state->c0] += state->latency_frames + state->align_latency_frames;
	latency[state->c1] += state->latency_frames + state->align_latency_frames;
	req_delay[state->c0] += state->latency_frames;
	req_delay[state->c1] += state->latency_frames;
	for (int i = e->istream.channels-2; i < e->ostream.channels; ++i) {
		latency[i] += state->latency_frames + state->align_latency_frames;
		req_delay[i] += state->latency_frames;
	}
}

static const char * matrix4_mb_effect_channel_label(struct effect *e, int ch, int out)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	if (ch >= e->istream.channels-2) {
		if (!out) return (ch == e->istream.channels-2) ? "Lta" : "Rta";
		ch += 2;
	}
	return matrix4_common_channel_label(e, state->layout_id, state->c0, state->c1, ch, out);
}

static inline double gauss_mag(double f0, double f, int hp)
{
	if (hp) return 1.0-exp(-1.227947177299516*(f/f0)*(f/f0));
	else return exp(-3.465735902799727e-01*(f/f0)*(f/f0));
}
#endif

struct effect * matrix4_mb_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e = NULL, *e_fir = NULL;
	struct matrix4_mb_state *state = NULL;
	struct matrix4_config config = {0};
	const struct filter_bank_params *fbp = &fb_params[0];

	if (matrix4_config_init(ei, istream, channel_selector, dir, argc, argv, 1, &config))
		goto fail;  /* may need to close config.pwr_err_file */
	if (config.fb_id[0]) {
		fbp = NULL;
		for (int i = 0; i < LENGTH(fb_params); ++i) {
			if (strcmp(config.fb_id, fb_params[i].id_str) == 0) {
				fbp = &fb_params[i];
				break;
			}
		}
		if (!fbp) {
			LOG_FMT(LL_ERROR, "%s: error: unknown filter bank id: %s", ei->name, config.fb_id);
			goto fail;
		}
	}
	if (istream->fs < fbp->min_fs) {
		dsp_perror(DSP_ERANGE, ei->name, "input sample rate");
		goto fail;
	}

	e = calloc(1, sizeof(struct effect));
	if (check_alloc(ei->name, e)) goto fail;
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
#if DO_FILTER_BANK_TEST
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + fbp->n_bands + 1;
#else
	e->istream.channels = istream->channels + 2;
	e->ostream.channels = istream->channels - 2 + config.channel_layout->nf
		+ config.channel_layout->ns*((config.dp_mode)?2:1);
#endif

	if (effect_set_channel_selector(e, NULL)) goto fail;
	/* note: analysis channel bits set later */
	SET_BIT(e->channel_selector, config.c0);
	SET_BIT(e->channel_selector, config.c1);
	state = calloc(1, sizeof(struct matrix4_mb_state));
	if (check_alloc(ei->name, state)) goto fail;
	e->data = state;

#if DO_FILTER_BANK_TEST
	e->run = matrix4_mb_test_fb_effect_run;
	e->destroy = matrix4_mb_test_fb_effect_destroy;
#else
	e->run = matrix4_mb_effect_run;
	e->reset = matrix4_mb_effect_reset;
	e->drain_samples = matrix4_mb_effect_drain_samples;
	e->destroy = matrix4_mb_effect_destroy;
	e->channel_offsets = matrix4_mb_effect_channel_offsets;
	e->channel_label = matrix4_mb_effect_channel_label;
#endif

	state->c0 = config.c0;
	state->c1 = config.c1;
	state->n_bands = fbp->n_bands;
	if (filter_bank_init(&state->fb, fbp, istream->fs, config.fb_type, config.fb_stop)) {
		LOG_FMT(LL_ERROR, "%s: BUG: failed to initialize filter bank", ei->name);
		goto fail;
	}
#if !(DO_FILTER_BANK_TEST)
	state->status_type = config.status_type;
	state->layout_id = config.channel_layout->id;
	state->do_phase_flip = !!config.do_phase_flip;
	state->do_direct_path = !!config.dp_mode;
	state->do_dpwr_decouple = !!config.do_dpwr_decouple;
	state->have_rears = (config.channel_layout->ns >= 4);
	state->calc_matrix_coefs = config.calc_matrix_coefs;
	state->matrix_adj[0] = config.matrix_adj[0];
	state->matrix_adj[1] = config.matrix_adj[1];
	e->signal = (config.enable_signal) ? matrix4_mb_effect_signal : NULL;
#if DEBUG_POWER_ERROR
	state->pwr_err_file = config.pwr_err_file;
#endif

	int max_syn_dt = 0;
	if (config.uev_action == UNALIGNED_EVENT_ACTION_ALIGN) {
		max_syn_dt = ceil(DELAY_EST_MAX_DELTA/1000.0 * istream->fs);
		state->align_ref_syn = max_syn_dt + CH_ALIGN_LATENCY;
		state->syn_buf_len = max_syn_dt*2 + CH_ALIGN_LATENCY;
		state->syn_buf = calloc(state->syn_buf_len, sizeof(sample_t)*state->n_bands*2);
		if (check_alloc(ei->name, state->syn_buf)) goto fail;
	}
	if (config.uev_action && delay_est_hist_init(&state->de_hist,
		lround(pow(state->n_bands, 0.7)*DELAY_EST_HIST_BASE_N))) goto fail;
	phase_flip_init_params(&state->pf_params, istream->fs);
	const double an_tpeak_ref = fbp->an_tpeak[0] + ((state->syn_buf) ? (state->fb.an.mask[0]+1)*1000.0/istream->fs : 0.0);
	for (int k = 0; k < state->n_bands; ++k) {
		struct matrix4_band *band = &state->band[k];
		const double an_fs = (double) istream->fs / (state->fb.an.mask[k]+1);
		smooth_state_init(&band->sm, an_fs);
		if (config.uev_action && delay_est_state_init(&band->de, an_fs, fbp->fc[k], fbp->fdiv[k]-fbp->fdiv[k-1],
			MAXIMUM(MINIMUM(2.0-0.241*(log(fbp->fc[k])-3.4), 2.0), 0.5),
			DOWNSAMPLED_FS(istream->fs))) goto fail;
		band->syn_tol_r = (fbp->fc[k]*8.0)/istream->fs;  /* ±45°; for event threshold modulation */
		const double thresh_scale = (fbp->thresh_scale[k] > 0.0) ? fbp->thresh_scale[k] : 1.0;
		band->ev_thresh_max = EVENT_THRESH_MAX * thresh_scale;
		band->ev_thresh_min = EVENT_THRESH_MIN * thresh_scale;
		if (event_state_init(&band->ev, istream, band->ev_thresh_max*(1.0/EVENT_THRESH),
			gauss_mag(BASE_ORD_NOTCH_SCALE_F0, fbp->fc[k], 0))) goto fail;
		ewma_init(&band->ev_thresh, DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
		ewma_set(&band->ev_thresh, band->ev_thresh_max);
		const double an_tpeak = fbp->an_tpeak[k] + ((state->syn_buf) ? (state->fb.an.mask[k]+1)*1000.0/istream->fs : 0.0);
		band->ax_buf_len = TIME_TO_FRAMES((an_tpeak_ref-an_tpeak)*0.7, DOWNSAMPLED_FS(istream->fs));
		if (band->ax_buf_len > 0) {
			band->ax_buf = calloc(band->ax_buf_len, sizeof(struct ax_buf_frame));
			if (check_alloc(ei->name, band->ax_buf)) goto fail;
		}
		const double pf_pos_rs = phase_flip_pos_rs(&band->ax_f.ax);
		cs_interp_set(&band->pf_ap_c0[0], phase_flip_ap1_c0(&state->pf_params, 1.0-pf_pos_rs));
		cs_interp_set(&band->pf_ap_c0[1], phase_flip_ap1_c0(&state->pf_params, pf_pos_rs));
		ap1_reset(&band->pf_ap[0]);
		ap1_reset(&band->pf_ap[1]);
		cs_interp_set(&band->m_interp.amb, 1.0);
		cs_interp_set(&band->m_interp.sdir, 0.0);
		cs_interp_set(&band->m_interp.rdir, 0.0);
		direct_path_state_init(&band->dp, DOWNSAMPLED_FS(istream->fs), config.dp_mode);
		if (state->syn_buf) {
			ch_align_an_state_init(&band->align_an, &band->de);
			ch_align_syn_state_init(&band->align_syn, state->align_ref_syn);
		}
		if (config.do_xtc && xtc_state_init(&band->xtc,
			config.xtc_itd*1e-6*istream->fs*(1.0+smoothstep(1.0-(log(fbp->fc[k])-5.704)*0.4343)*0.47),
			config.xtc_gain*gauss_mag(config.xtc_bp[0], fbp->fc[k], 1)*gauss_mag(config.xtc_bp[1], fbp->fc[k], 0))) goto fail;
	#if DEBUG_POWER_ERROR
		ewma_init(&band->pwr_err[0], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
		ewma_init(&band->pwr_err[1], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
		smf_init(&band->pwr_err_sm, istream->fs, SMF_RISE_TIME(300.0), 0.1);
	#endif
	#if REPORT_EVENT_LEVELS
		band->evl_samples = -DOWNSAMPLED_FS(istream->fs)*2;  /* skip first 1s */
		band->fbp = fbp;
	#endif
	}

	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, DOWNSAMPLED_FS(istream->fs));
	event_config_init(&state->evc, istream, config.rear_ev_mask, config.uev_action);
#endif
	fshape_init(&state->fshape[0], istream->fs, fshape_lf, fshape_hf, 0);
	state->fshape[1] = state->fshape[0];

	const double shelf_mult2 = config.shelf_mult*config.shelf_mult;
	const double shelf_f02 = config.shelf_f0*config.shelf_f0;
	const double lowpass_f02 = config.lowpass_f0*config.lowpass_f0;
	for (int k = 0; k < state->n_bands; ++k) {
		struct matrix4_band *band = &state->band[k];
		const double fc2 = fbp->fc[k]*fbp->fc[k];
		const double shelf_norm_f2 = fc2/shelf_f02;
		band->contour = sqrt((1.0+shelf_mult2*shelf_norm_f2)/(1.0+shelf_norm_f2));
		if (lowpass_f02 > 0.0) {
			const double lowpass_norm_f2 = fc2/lowpass_f02;
			band->contour *= pow(1.0/(1.0+lowpass_norm_f2), config.lowpass_order/2.0);
		}
		band->rear_shelf = sqrt((1.0+0.1*shelf_norm_f2)/(1.0+shelf_norm_f2));
		/* LOG_FMT(LL_VERBOSE, "%s: band %d: contour=%.4g", ei->name, k, band->contour); */
	}
	state->surr_mult[0] = config.surr_mult[0];
	state->surr_mult[1] = config.surr_mult[1];
	state->contour_pwrcmp = config.contour_pwrcmp;

	ssize_t phase_lin_frames = TIME_TO_FRAMES(fbp->fir_len, istream->fs);
	sample_t *filter = calloc(phase_lin_frames, sizeof(sample_t));
	if (check_alloc(ei->name, filter)) goto fail;
	filter[phase_lin_frames-1] = 1.0;
	for (int i = phase_lin_frames-1; i >= 0; --i)
		filter[i] = filter_bank_sum(&state->fb, filter[i]);
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
	struct effect_info ei_fir = { .name = (config.use_fir_p) ? "fir_p (matrix4_mb)" : "fir (matrix4_mb)" };
	if (config.use_fir_p) e_fir = fir_p_effect_init_with_filter(&ei_fir, &e->istream, e->channel_selector, &filter[zx], 1, phase_lin_frames, phase_lin_frames-1, 0);
	else e_fir = fir_effect_init_with_filter(&ei_fir, &e->istream, e->channel_selector, &filter[zx], 1, phase_lin_frames, phase_lin_frames-1, 0);
	free(filter);
	filter_bank_reset(&state->fb);
	if (!e_fir) goto fail;

#if !(DO_FILTER_BANK_TEST)
	const ssize_t lookahead_frames = config.lookahead_frames + CS_INTERP_DELAY_FRAMES
		+ state->band[state->n_bands-1].ax_buf_len*DOWNSAMPLE_FACTOR - max_syn_dt;
	struct effect *e_input = matrix4_mb_input_effect_init(istream, channel_selector, state->c0, state->c1, lookahead_frames);
	if (!e_input) goto fail;

	/* analysis inputs */
	SET_BIT(e->channel_selector, istream->channels+0);
	SET_BIT(e->channel_selector, istream->channels+1);
	state->drain_frames = MAXIMUM(lookahead_frames-(phase_lin_frames-1), 0);
	state->latency_frames = MAXIMUM(lookahead_frames, phase_lin_frames-1);
	if (state->syn_buf) {
		state->align_latency_frames = max_syn_dt + CH_ALIGN_LATENCY;
		state->drain_frames += state->align_latency_frames;
	}

	effect_list_append(e_input, e_fir);
	effect_list_append(e_fir, e);
	return e_input;
#elif FILTER_BANK_TEST_ANALYSIS
	destroy_effect(e_fir);
	return e;
#else
	effect_list_append(e_fir, e);
	return e_fir;
#endif

	fail:
#if DEBUG_POWER_ERROR
	if (!state && config.pwr_err_file)
		fclose(state->pwr_err_file);
#endif
	destroy_effect(e_fir);
	destroy_effect(e);
	return NULL;
}
