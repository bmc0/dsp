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
#define DIFF_OVERSHOOT    1.01
#include "matrix4_common.h"

#define BASE_ORD_NOTCH_SCALE_F0 700.0  /* -3dB point; Gaussian lowpass */
#define EVENT_THRESH_MAX 3.6
#define EVENT_THRESH_MIN 1.4

#define PHASE_LIN_TRUNC_THRESH 1e-6
#define DO_FILTER_BANK_TEST 0

#if DEBUG_POWER_ERROR
	#include "smf.h"
#endif
#include "matrix4_filter_bank.c"

#define REPORT_EVENT_LEVELS 0
#define EVENT_TARGET_BASE   (EVENT_THRESH_MAX*0.15)
#define EVENT_TARGET_EXP1   0.17
#define EVENT_TARGET_EXP2   0.28

struct matrix4_band {
	struct smooth_state sm;
	struct event_state ev;
	struct axes ax, ax_ev, ax_dpwr;
	struct {
		struct cs_interp_state ll, lr, rl, rr;
		struct cs_interp_state lsl, lsr, rsl, rsr;
		struct cs_interp_state sg, rg;
		struct cs_interp_state amb, sdir, rdir;
	} m_interp;
	struct cs_interp_state pf_ap_c0[2];
	struct ap1_state pf_ap[2];
	struct direct_path_state dp;
	struct ewma_state ev_thresh;
	double ev_thresh_max, ev_thresh_min, contour, rear_shelf;
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
	struct fshape_state fshape[2], inv_fshape[10];
	struct filter_bank fb[2];
	struct matrix4_band band[FB_MAX_BANDS];
	sample_t *fb_buf[2];
	struct event_config evc;
	struct phase_flip_params pf_params;
	calc_matrix_coefs_func calc_matrix_coefs;
	double cmc_param, surr_mult[2], contour_pwrcmp, freq_mask;
	ssize_t len, fb_buf_len, fb_buf_p;
	ssize_t fade_frames, fade_p;
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
		const double s0 = fshape_run(&state->fshape[0], ibuf[i*e->istream.channels + state->c0]);
		const double s1 = fshape_run(&state->fshape[1], ibuf[i*e->istream.channels + state->c1]);
		state->fb[0].run(&state->fb[0], s0);
		state->fb[1].run(&state->fb[1], s1);
		double out_l = 0.0, out_r = 0.0, out_s = 0.0;
		for (int k = 0; k < state->n_bands; ++k) {
			struct matrix4_band *band = &state->band[k];
			const double ct1 = (band->contour-1.0)*state->contour_pwrcmp + 1.0;
			const double norm_mult = CALC_NORM_MULT(state->surr_mult[0]*ct1);
			out_l += state->fb[0].s[k]*norm_mult;
			out_r += state->fb[1].s[k]*norm_mult;
			out_s += state->fb[0].s[k]*norm_mult*state->surr_mult[0]*band->contour;
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
		double s0_fb_fm = 0.0;
		for (int k = 0; k < state->n_bands; ++k)
			obuf[i*e->ostream.channels + e->istream.channels + k] = s0_fb_fm = state->fb[0].s[k] + state->freq_mask*s0_fb_fm;
		obuf[i*e->ostream.channels + e->istream.channels + state->n_bands] = out_s;
	}
	return obuf;
}

static void matrix4_mb_test_fb_effect_destroy(struct effect *e)
{
	free(e->data);
}

#else

static sample_t * matrix4_mb_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		double cur_fade_mult = 1.0;
		if (state->fade_p > 0) {
			cur_fade_mult = fade_mult(state->fade_p, state->fade_frames, state->disable);
			--state->fade_p;
		}
		else if (state->disable) cur_fade_mult = 0.0;

		int n_angles = 0;
		struct axes angles[FB_MAX_BANDS];
		sample_t out_l = 0.0, out_r = 0.0, out_ls = 0.0, out_rs = 0.0, out_lr = 0.0, out_rr = 0.0;
		sample_t out_ls_dir = 0.0, out_rs_dir = 0.0, out_lr_dir = 0.0, out_rr_dir = 0.0;
		const sample_t s0 = fshape_run(&state->fshape[0], ibuf[i*e->istream.channels + state->c0]);
		const sample_t s1 = fshape_run(&state->fshape[1], ibuf[i*e->istream.channels + state->c1]);
		state->fb[0].run(&state->fb[0], s0);
		state->fb[1].run(&state->fb[1], s1);
		#if DOWNSAMPLE_FACTOR > 1
		state->s = (state->s + 1 >= DOWNSAMPLE_FACTOR) ? 0 : state->s + 1;
		if (state->s == 0) {
		#else
		if (1) {
		#endif
			/* find bands with possible events */
			for (int k = 0; k < state->n_bands; ++k) {
				struct matrix4_band *band = &state->band[k];
				struct event_state *ev = &band->ev;
				if ((ev->slope_last[0] > 0.0 && ev->last[0] > band->ev_thresh_min)
						|| (ev->slope_last[1] > 0.0 && ev->last[1] > band->ev_thresh_min))
					angles[n_angles++] = ev->diff_last;
			}
		}
		const ssize_t fb_buf_fp = state->fb_buf_p*state->n_bands;
		sample_t s0_fb_fm = 0.0, s1_fb_fm = 0.0;
		for (int k = 0; k < state->n_bands; ++k) {
			struct matrix4_band *band = &state->band[k];

			s0_fb_fm = state->fb[0].s[k] + state->freq_mask*s0_fb_fm;
			s1_fb_fm = state->fb[1].s[k] + state->freq_mask*s1_fb_fm;
			const sample_t s0_d_fb = state->fb_buf[0][fb_buf_fp+k];
			const sample_t s1_d_fb = state->fb_buf[1][fb_buf_fp+k];

			struct envs env, pwr_env;
			calc_input_envs(&band->sm, s0_fb_fm, s1_fb_fm, &env, &pwr_env);

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
					band->ev_thresh_max - (band->ev_thresh_max-band->ev_thresh_min)*ev_thresh_fact/(state->n_bands-1));

				process_events(&band->ev, &state->evc, &env, &pwr_env, ev_thresh*(1.0/EVENT_THRESH), &band->ax, &band->ax_ev, &band->ax_dpwr);
				#if REPORT_EVENT_LEVELS
					if (band->evl_samples >= 0) {
						band->evl_sum_sq += ev->last[0]*ev->last[0];
						band->evl_sum_sq += ev->last[1]*ev->last[1];
						band->evl_pk = MAXIMUM(band->evl_pk, ev->last[0]);
						band->evl_pk = MAXIMUM(band->evl_pk, ev->last[1]);
					}
					band->evl_samples += 2;
				#endif

				const double w = smoothstep(band->ax.cs*(-2/M_PI_4));
				const double surr_mult = (w*state->surr_mult[1] + (1.0-w)*state->surr_mult[0])*cur_fade_mult;
				const double ct_pcf = state->contour_pwrcmp * ewma_get_last(&band->ev.pwrcmp_factor);
				const double ct0 = w + (1.0-w)*band->contour;
				const double ct1 = (ct0-1.0)*ct_pcf + 1.0;
				const double ct2 = ct0/ct1;

				struct matrix_coefs m = {0};
				state->calc_matrix_coefs(&band->ax, (state->do_dpwr_decouple) ? &band->ax_dpwr : &band->ax,
					surr_mult*ct1, state->surr_mult[1]*cur_fade_mult, state->cmc_param, &m, NULL, 0);

				cs_interp_insert(&band->m_interp.ll, m.ll);
				cs_interp_insert(&band->m_interp.lr, m.lr);
				cs_interp_insert(&band->m_interp.rl, m.rl);
				cs_interp_insert(&band->m_interp.rr, m.rr);

				cs_interp_insert(&band->m_interp.lsl, m.lsl*ct2);
				cs_interp_insert(&band->m_interp.lsr, m.lsr*ct2);
				cs_interp_insert(&band->m_interp.rsl, m.rsl*ct2);
				cs_interp_insert(&band->m_interp.rsr, m.rsr*ct2);

				if (state->have_rears) {
					const double rsw = smoothstep(band->ax.cs*(-2/M_PI_4)-1.0);
					const double sg = rsw*band->rear_shelf + (1.0-rsw);
					const double s_norm = CALC_NORM_MULT(sg);
					cs_interp_insert(&band->m_interp.sg, sg*s_norm);
					cs_interp_insert(&band->m_interp.rg, s_norm);
				}
				if (state->do_phase_flip) {
					const double pf_pos_rs = phase_flip_pos_rs(&band->ax);
					cs_interp_insert(&band->pf_ap_c0[0], phase_flip_ap1_c0(&state->pf_params, 1.0-pf_pos_rs));
					cs_interp_insert(&band->pf_ap_c0[1], phase_flip_ap1_c0(&state->pf_params, pf_pos_rs));
				}
				if (state->do_direct_path) {
					double r_pan[3];
					surr_direct_pan(&band->dp, &band->ev, &band->ax, state->have_rears, r_pan);
					cs_interp_insert(&band->m_interp.amb, r_pan[0]);
					cs_interp_insert(&band->m_interp.sdir, r_pan[1]);
					if (state->have_rears) cs_interp_insert(&band->m_interp.rdir, r_pan[2]);
				}
			}

			sample_t b_l = s0_d_fb*cs_interp(&band->m_interp.ll, state->s) + s1_d_fb*cs_interp(&band->m_interp.lr, state->s);
			sample_t b_r = s0_d_fb*cs_interp(&band->m_interp.rl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rr, state->s);
			sample_t b_ls = s0_d_fb*cs_interp(&band->m_interp.lsl, state->s) + s1_d_fb*cs_interp(&band->m_interp.lsr, state->s);
			sample_t b_rs = s0_d_fb*cs_interp(&band->m_interp.rsl, state->s) + s1_d_fb*cs_interp(&band->m_interp.rsr, state->s);

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

			out_l += b_l;
			out_r += b_r;
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

			state->fb_buf[0][fb_buf_fp+k] = state->fb[0].s[k];
			state->fb_buf[1][fb_buf_fp+k] = state->fb[1].s[k];
		}

		out_l = fshape_run(&state->inv_fshape[0], out_l);
		out_r = fshape_run(&state->inv_fshape[1], out_r);
		sample_t *ibuf_p = &ibuf[i*e->istream.channels], *obuf_p = &obuf[i*e->ostream.channels];
		for (int k = 0; k < e->istream.channels; ++k) {
			if (k == state->c0) obuf_p[k] = out_l;
			else if (k == state->c1) obuf_p[k] = out_r;
			else obuf_p[k] = ibuf_p[k];
		}
		obuf_p += e->istream.channels;
		obuf_p[0] = fshape_run(&state->inv_fshape[2], out_ls+(1e-15/324))-1e-15;
		obuf_p[1] = fshape_run(&state->inv_fshape[3], out_rs+(1e-15/324))-1e-15;
		if (state->have_rears) {
			obuf_p[2] = fshape_run(&state->inv_fshape[4], out_lr+(1e-15/324))-1e-15;
			obuf_p[3] = fshape_run(&state->inv_fshape[5], out_rr+(1e-15/324))-1e-15;
			if (state->do_direct_path) {
				obuf_p[4] = fshape_run(&state->inv_fshape[6], out_ls_dir+(1e-15/324))-1e-15;
				obuf_p[5] = fshape_run(&state->inv_fshape[7], out_rs_dir+(1e-15/324))-1e-15;
				obuf_p[6] = fshape_run(&state->inv_fshape[8], out_lr_dir+(1e-15/324))-1e-15;
				obuf_p[7] = fshape_run(&state->inv_fshape[9], out_rr_dir+(1e-15/324))-1e-15;
			}
		}
		else if (state->do_direct_path) {
			obuf_p[2] = fshape_run(&state->inv_fshape[4], out_ls_dir+(1e-15/324))-1e-15;
			obuf_p[3] = fshape_run(&state->inv_fshape[5], out_rs_dir+(1e-15/324))-1e-15;
		}

		state->fb_buf_p = CBUF_NEXT(state->fb_buf_p, state->fb_buf_len);
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
				#if DEBUG_POWER_ERROR
					"; pwr_err: %+05.2fdB"
				#endif
					, e->name, (state->disable) ? " [off]" : "", i,
					TO_DEGREES(band->ax.lr), TO_DEGREES(band->ax_ev.lr), TO_DEGREES(band->ax.cs), TO_DEGREES(band->ax_ev.cs),
					band->ev.adj, ewma_get_last(&band->ev_thresh), state->contour_pwrcmp*ewma_get_last(&band->ev.pwrcmp_factor),
					band->ev.ord_count, band->ev.diff_count, band->ev.early_count, band->ev.ignore_count
				#if DEBUG_POWER_ERROR
					, 10.0*log10(smf_get_last(&band->pwr_err_sm))
				#endif
				);
			}
		}
		else {
			for (int i = 0; i < state->n_bands; ++i) {
				struct matrix4_band *band = &state->band[i];
				draw_steering_bar(band->ax.lr, band->ev.hold, &band->lr_bar);
				draw_steering_bar(band->ax.cs, band->ev.hold, &band->cs_bar);
				snprintf(band->statusline.s, LENGTH(band->statusline.s),
					"%s%s: band %2d: L[%s]R; C[%s]S; ord: %zd; diff: %zd; ign: %zd"
				#if DEBUG_POWER_ERROR
					"; pwr_err: %+05.2fdB"
				#endif
					, e->name, (state->disable) ? " [off]" : "", i,
					band->lr_bar.s, band->cs_bar.s, band->ev.ord_count, band->ev.diff_count, band->ev.ignore_count
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
	state->fb_buf_p = 0;
	memset(state->fb_buf[0], 0, state->fb_buf_len * sizeof(sample_t) * state->n_bands);
	memset(state->fb_buf[1], 0, state->fb_buf_len * sizeof(sample_t) * state->n_bands);
	filter_bank_reset(&state->fb[0]);
	filter_bank_reset(&state->fb[1]);
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
	drain_samples[state->c0] += state->fb_buf_len;
	drain_samples[state->c1] += state->fb_buf_len;
	for (int i = e->istream.channels; i < e->ostream.channels; ++i)
		drain_samples[i] += state->fb_buf_len;
}

static void matrix4_mb_effect_destroy(struct effect *e)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	free(state->fb_buf[0]);
	free(state->fb_buf[1]);
	for (int i = 0; i < state->n_bands; ++i)
		event_state_cleanup(&state->band[i].ev);
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

static void matrix4_mb_effect_channel_deps(struct effect *e, char **deps)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	SET_BIT(deps[state->c0], state->c1);
	SET_BIT(deps[state->c1], state->c0);
	for (int i = e->istream.channels; i < e->ostream.channels; ++i) {
		SET_BIT(deps[i], state->c0);
		SET_BIT(deps[i], state->c1);
	}
}

static void matrix4_mb_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct matrix4_mb_state *state = (struct matrix4_mb_state *) e->data;
	latency[state->c0] += state->len;
	latency[state->c1] += state->len;
	for (int i = e->istream.channels; i < e->ostream.channels; ++i) latency[i] += state->len;
}
#endif

struct effect * matrix4_mb_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e = NULL;
	struct matrix4_mb_state *state = NULL;
	struct matrix4_config config = {0};
	const struct filter_bank_params *fbp = &fb_params[0];

	if (matrix4_config_init(ei, istream, channel_selector, dir, argc, argv, 1, &config))
		goto fail;  /* may need to close config.pwr_err_file */
	if (config.fb_id[0]) {
		fbp = NULL;
		for (int i = 0; i < LENGTH(fb_params); ++i) {
			if (strcmp(config.fb_id, fb_params[i].id) == 0) {
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
	e->run = matrix4_mb_test_fb_effect_run;
	e->destroy = matrix4_mb_test_fb_effect_destroy;
#else
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels - 2 + config.channel_layout->nf
		+ config.channel_layout->ns*((config.dp_mode)?2:1);
	e->run = matrix4_mb_effect_run;
	e->reset = matrix4_mb_effect_reset;
	e->drain_samples = matrix4_mb_effect_drain_samples;
	e->destroy = matrix4_mb_effect_destroy;
	e->channel_deps = matrix4_mb_effect_channel_deps;
	e->channel_offsets = matrix4_mb_effect_channel_offsets;
#endif

	state = calloc(1, sizeof(struct matrix4_mb_state));
	if (check_alloc(ei->name, state)) goto fail;
	e->data = state;
	state->c0 = config.c0;
	state->c1 = config.c1;
	state->n_bands = fbp->n_bands;
#if !(DO_FILTER_BANK_TEST)
	state->status_type = config.status_type;
	state->do_phase_flip = !!config.do_phase_flip;
	state->do_direct_path = !!config.dp_mode;
	state->do_dpwr_decouple = !!config.do_dpwr_decouple;
	state->have_rears = (config.channel_layout->ns >= 4);
	state->calc_matrix_coefs = config.calc_matrix_coefs;
	state->cmc_param = config.calc_matrix_coefs_param;
	e->signal = (config.enable_signal) ? matrix4_mb_effect_signal : NULL;
#if DEBUG_POWER_ERROR
	state->pwr_err_file = config.pwr_err_file;
#endif

	phase_flip_init_params(&state->pf_params, istream->fs);
	for (int k = 0; k < state->n_bands; ++k) {
		struct matrix4_band *band = &state->band[k];
		smooth_state_init(&band->sm, istream);
		const double thresh_scale = (fbp->thresh_scale[k] > 0.0) ? fbp->thresh_scale[k] : 1.0;
		band->ev_thresh_max = EVENT_THRESH_MAX * thresh_scale;
		band->ev_thresh_min = EVENT_THRESH_MIN * thresh_scale;
		const double ns_fc = fbp->fc[k]/BASE_ORD_NOTCH_SCALE_F0;
		if (event_state_init(&band->ev, istream, band->ev_thresh_max*(1.0/EVENT_THRESH),
			exp(-3.465735902799727e-01*ns_fc*ns_fc))) goto fail;
		ewma_init(&band->ev_thresh, DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
		ewma_set(&band->ev_thresh, band->ev_thresh_max);
		const double pf_pos_rs = phase_flip_pos_rs(&band->ax);
		cs_interp_set(&band->pf_ap_c0[0], phase_flip_ap1_c0(&state->pf_params, 1.0-pf_pos_rs));
		cs_interp_set(&band->pf_ap_c0[1], phase_flip_ap1_c0(&state->pf_params, pf_pos_rs));
		ap1_reset(&band->pf_ap[0]);
		ap1_reset(&band->pf_ap[1]);
		cs_interp_set(&band->m_interp.amb, 1.0);
		cs_interp_set(&band->m_interp.sdir, 0.0);
		cs_interp_set(&band->m_interp.rdir, 0.0);
		direct_path_state_init(&band->dp, DOWNSAMPLED_FS(istream->fs), config.dp_mode);
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

	state->fb_buf_len = config.lookahead_frames;
#if DOWNSAMPLE_FACTOR > 1
	state->fb_buf_len += CS_INTERP_DELAY_FRAMES;
#endif
	state->fb_buf[0] = calloc(state->fb_buf_len, sizeof(sample_t) * state->n_bands);
	if (check_alloc(ei->name, state->fb_buf[0])) goto fail;
	state->fb_buf[1] = calloc(state->fb_buf_len, sizeof(sample_t) * state->n_bands);
	if (check_alloc(ei->name, state->fb_buf[1])) goto fail;
	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, istream->fs);
	event_config_init(&state->evc, istream, config.rear_ev_mask);
#endif
	fshape_init(&state->fshape[0], istream->fs, fshape_lf, fshape_hf, 0);
	state->fshape[1] = state->fshape[0];
	fshape_init(&state->inv_fshape[0], istream->fs, fshape_lf, fshape_hf, 1);
	for (int i = 1; i < LENGTH(state->inv_fshape); ++i)
		state->inv_fshape[i] = state->inv_fshape[0];

	if (filter_bank_init(&state->fb[0], fbp, istream->fs, config.fb_type, config.fb_stop)) {
		LOG_FMT(LL_ERROR, "%s: BUG: failed to initialize filter bank", ei->name);
		goto fail;
	}
	filter_bank_dup(&state->fb[1], &state->fb[0]);

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
			band->contour *= sqrt(1.0/(1.0+lowpass_norm_f2));
		}
		band->rear_shelf = sqrt((1.0+0.1*shelf_norm_f2)/(1.0+shelf_norm_f2));
		/* LOG_FMT(LL_VERBOSE, "%s: band %d: contour=%.4g", ei->name, k, band->contour); */
	}
	state->surr_mult[0] = config.surr_mult[0];
	state->surr_mult[1] = config.surr_mult[1];
	state->contour_pwrcmp = config.contour_pwrcmp;
	state->freq_mask = config.freq_mask;

	ssize_t phase_lin_frames = TIME_TO_FRAMES(fbp->fir_len, istream->fs);
	sample_t *filter = calloc(phase_lin_frames, sizeof(sample_t));
	if (check_alloc(ei->name, filter)) goto fail;
	filter[phase_lin_frames-1] = 1.0;
	for (int i = phase_lin_frames-1; i >= 0; --i)
		filter[i] = filter_bank_sum(&state->fb[1], filter[i]);
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
	struct effect *e_fir = (config.use_fir_p)
		? fir_p_effect_init_with_filter(ei, istream, channel_selector, &filter[zx], 1, phase_lin_frames, 0, 0)
		: fir_effect_init_with_filter(ei, istream, channel_selector, &filter[zx], 1, phase_lin_frames, 0, 0);
	free(filter);
	filter_bank_reset(&state->fb[1]);
	state->len = state->fb_buf_len + (phase_lin_frames - 1);  /* total delay */
	if (e_fir == NULL) goto fail;

	effect_list_append(e_fir, e);
	return e_fir;

	fail:
#if DEBUG_POWER_ERROR
	if (!state && config.pwr_err_file)
		fclose(state->pwr_err_file);
#endif
	if (state) e->destroy(e);
	free(e);
	return NULL;
}
