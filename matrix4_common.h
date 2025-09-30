/*
 * This file is part of dsp.
 *
 * Copyright (c) 2020-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_MATRIX4_COMMON_H
#define DSP_MATRIX4_COMMON_H

#include <float.h>
#include <math.h>
#include <string.h>
#include "effect.h"
#include "ewma.h"
#include "smf.h"
#include "util.h"

#define EVENT_THRESH          1.8
#define EVENT_END_THRESH      0.2
#define ENV_SMOOTH_TIME      30.0
#define EVENT_SMOOTH_TIME    30.0
#define ACCOM_TIME          300.0
#define RISE_TIME_FAST       30.0
#define RISE_TIME_SLOW      100.0
#define NORM_TIME           160.0
#define NORM_CROSSFEED        0.1
#define ORD_FACTOR_DECAY     10.0
#define EVENT_SAMPLE_TIME    30.0
#define EVENT_MAX_HOLD_TIME 200.0
#define EVENT_MIN_HOLD_TIME  50.0
#define EVENT_MASK_TIME     100.0
#define DELAY_TIME (EVENT_SAMPLE_TIME + RISE_TIME_FAST*0.75)
#define ORD_SENS_ERR         10.0
#define ORD_SENS_LEVEL       10.0
#define DIFF_SENS_ERR        10.0
#define DIFF_SENS_LEVEL       3.0

/* note: implemented in matrix4{,_mb}.c */
#define DIR_BOOST_RT0       100.0
#define DIR_BOOST_SENS_RISE   0.1
#define DIR_BOOST_SENS_FALL   0.01

#define FILTER_BANK_TYPE_DEFAULT FILTER_BANK_TYPE_ELLIPTIC
#define DIR_BOOST_TYPE_DEFAULT   DIR_BOOST_TYPE_SIMPLE

#define DIR_BOOST_MIN_BAND_WEIGHT_DEFAULT 0.5
#define DIR_BOOST_MAX_BAND_WEIGHT_DEFAULT 0.9

/* fade parameters when toggling effect via signal() */
#define FADE_TIME 500.0
/* 1 = linear; 2 = quarter sine; 3 = half sine; 4 = double-exponential sigmoid */
#define FADE_TYPE 3

#ifndef DOWNSAMPLE_FACTOR
	#define DOWNSAMPLE_FACTOR 1
#endif
#ifndef NORM_ACCOM_FACTOR
	#define NORM_ACCOM_FACTOR 0.9
#endif

/* 1 = linear; 2 = parabolic 2x; 3 = cubic B-spline; 4 = polyphase FIR (blackman window) */
#ifndef CS_INTERP_TYPE
#if DOWNSAMPLE_FACTOR == 4 || DOWNSAMPLE_FACTOR == 2
	#define CS_INTERP_TYPE 4
#else
	#define CS_INTERP_TYPE 2
#endif
#endif

#define DEBUG_PRINT_MIN_RISE_TIME 0

struct envs {
	double l, r, sum, diff;
};

struct smooth_state {
	struct ewma_state env[4], pwr_env[4];
};

struct axes {
	double lr, cs;
};

struct matrix_coefs {
	double lsl, lsr, rsl, rsr;
	double dir_boost;
};

struct event_state {
	char sample, hold;
	enum {
		EVENT_FLAG_L = 1<<0,
		EVENT_FLAG_R = 1<<1,
		EVENT_FLAG_USE_ORD = 1<<2,
		EVENT_FLAG_FUSE = 1<<3,
		EVENT_FLAG_END = 1<<4,
	} flags[2];
	struct ewma_state accom[6], norm[2], slow[4], smooth[2], avg[4];
	struct ewma_state drift[4];
	struct axes dir, drift_last[2], *ord_buf;
	struct envs *env_buf, *adapt_buf;
	double thresh, end_thresh, clip_thresh, max, weight;
	double ord_factor, adj;
	ssize_t t, t_sample, t_hold;
	ssize_t ord_count, diff_count, early_count, ignore_count;
	ssize_t buf_len, buf_p;
	#if DEBUG_PRINT_MIN_RISE_TIME
		double max_ord_scale, max_diff_scale, fs;
	#endif
};

struct event_config {
	ssize_t sample_frames, max_hold_frames, min_hold_frames;
	double ord_factor_c;
};

enum filter_bank_type {
	FILTER_BANK_TYPE_BUTTERWORTH = 0,
	FILTER_BANK_TYPE_CHEBYSHEV1,
	FILTER_BANK_TYPE_CHEBYSHEV2,
	FILTER_BANK_TYPE_ELLIPTIC,
};

enum dir_boost_type {
	DIR_BOOST_TYPE_NONE = 0,
	DIR_BOOST_TYPE_SIMPLE,
	DIR_BOOST_TYPE_BAND,
	DIR_BOOST_TYPE_COMBINED,
};

struct matrix4_config {
	int n_channels, opt_str_idx, c0, c1;
	double surr_mult, fb_stop[2], db_band_weight[2];
	ssize_t surr_delay_frames;
	char show_status, enable_signal, do_phase_lin;
	enum filter_bank_type fb_type;
	enum dir_boost_type db_type;
};

#define CALC_NORM_MULT(x) (1.0 / sqrt(1.0 + (x)*(x)))
#define TO_DEGREES(x) ((x)*M_1_PI*180.0)
#define TIME_TO_FRAMES(x, fs) lround((x)/1000.0 * (fs))
#define NEAR_POS_ZERO(x) ((x) < DBL_MIN)
#define ANGLE(n, d, expr) ((NEAR_POS_ZERO(n) && NEAR_POS_ZERO(d)) ? M_PI_4 : (NEAR_POS_ZERO(d)) ? M_PI_2 : atan(expr))
#define CALC_LR(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)
#define CALC_CS(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)
#define DOWNSAMPLED_FS(fs) (((double) (fs)) / DOWNSAMPLE_FACTOR)

int get_args_and_channels(const struct effect_info *, const struct stream_info *, const char *, int, const char *const *, struct matrix4_config *);
int parse_effect_opts(const char *const *, const struct stream_info *, struct matrix4_config *);
struct effect * matrix4_delay_effect_init(const struct effect_info *, const struct stream_info *, ssize_t);

#ifndef DSP_MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS
static inline double err_scale(double a, double b, double err, double max_err_gain)
{
	if (NEAR_POS_ZERO(a) && NEAR_POS_ZERO(b))
		return 1.0;
	const double n = a+b;
	const double d = MAXIMUM(MINIMUM(a, b), n/max_err_gain);
	if (NEAR_POS_ZERO(d))
		return 1.0 + err*max_err_gain;
	return 1.0 + err*n/d;
}

static inline double drift_scale(const struct axes *ax0, const struct axes *ax1, const struct envs *env, double sens_err, double sens_level)
{
	const double lr_err = fabs(ax1->lr - ax0->lr) / M_PI_2;
	const double cs_err = fabs(ax1->cs - ax0->cs) / M_PI_2;
	const double lr_scale = err_scale(env->l, env->r, lr_err*sens_err, sens_level);
	const double cs_scale = err_scale(env->sum, env->diff, cs_err*sens_err, sens_level);
	return MAXIMUM(lr_scale, cs_scale);
}

#if DOWNSAMPLE_FACTOR > 1
#if CS_INTERP_TYPE == 1
/* linear */
#define CS_INTERP_PEEK(s) ((s)->y[1])
#define CS_INTERP_DELAY_FRAMES (1*DOWNSAMPLE_FACTOR)
struct cs_interp_state {
	double c0, y[2];
};

static inline void cs_interp_insert(struct cs_interp_state *s, double x)
{
	double *y = s->y;
	y[0] = y[1];
	y[1] = x;
	s->c0 = y[1]-y[0];
}

static inline double cs_interp(const struct cs_interp_state *s, int x)
{
	const double t = x * (1.0/DOWNSAMPLE_FACTOR);
	return s->y[0] + t*s->c0;
}
#elif CS_INTERP_TYPE == 2
/* parabolic 2x -- Niemitalo, Olli, "Polynomial Interpolators for
 * High-Quality Resampling of Oversampled Audio," October 2001. */
#define CS_INTERP_PEEK(s) ((s)->y[2])
#define CS_INTERP_DELAY_FRAMES (3*DOWNSAMPLE_FACTOR)
struct cs_interp_state {
	double c[3];
	double y[4];
};

static void cs_interp_insert(struct cs_interp_state *s, double x)
{
	double *y = s->y, *c = s->c;
	memmove(y, y+1, sizeof(double)*3);
	y[3] = x;
	const double a = y[2]-y[0];
	c[0] = 1.0/2.0*y[1] + 1.0/4.0*(y[0]+y[2]);
	c[1] = 1.0/2.0*a;
	c[2] = 1.0/4.0*(y[3]-y[1]-a);
}

static inline double cs_interp(const struct cs_interp_state *s, int x)
{
	const double *c = s->c, t = x * (1.0/DOWNSAMPLE_FACTOR);
	return (c[2]*t+c[1])*t+c[0];
}
#elif CS_INTERP_TYPE == 3
/* cubic B-spline */
#define CS_INTERP_PEEK(s) ((s)->y[2])
#define CS_INTERP_DELAY_FRAMES (3*DOWNSAMPLE_FACTOR)
struct cs_interp_state {
	double c[4];
	double y[4];
};

static void cs_interp_insert(struct cs_interp_state *s, double x)
{
	double *y = s->y, *c = s->c;
	memmove(y, y+1, sizeof(double)*3);
	y[3] = x;
	const double a = y[0]+y[2];
	c[0] = 1.0/6.0*a + 2.0/3.0*y[1];
	c[1] = 1.0/2.0*(y[2]-y[0]);
	c[2] = 1.0/2.0*a - y[1];
	c[3] = 1.0/2.0*(y[1]-y[2]) + 1.0/6.0*(y[3]-y[0]);
}

static inline double cs_interp(const struct cs_interp_state *s, int x)
{
	const double *c = s->c, t = x * (1.0/DOWNSAMPLE_FACTOR);
	return ((c[3]*t+c[2])*t+c[1])*t+c[0];
}
#elif CS_INTERP_TYPE == 4
/* polyphase FIR (blackman window) */
#define CS_INTERP_PEEK(s) ((s)->y[DOWNSAMPLE_FACTOR-1])
#if DOWNSAMPLE_FACTOR == 2
#define CS_INTERP_DELAY_FRAMES (4*DOWNSAMPLE_FACTOR-1)
struct cs_interp_state {
	double y[2], m[12];
	int p;
};

static void cs_interp_insert(struct cs_interp_state *state, double x)
{
	const double r[6] = {
		1.070924528086533e-02*x, 5.158730158730156e-02*x,
		1.349206349206349e-01*x, 2.499999999999999e-01*x,
		3.543701197984997e-01*x, 3.968253968253968e-01*x,
	};
	int p = state->p;
	double *m = state->m, *y = state->y;
	y[0]=m[p++]+r[0]; y[1]=m[p++]+r[1];
	memset(&m[state->p], 0, sizeof(double)*2);
	if (p==12) p=0;
	state->p = p;
	m[p++]+=r[2]; m[p++]+=r[3]; if (p==12) p=0;
	m[p++]+=r[4]; m[p++]+=r[5]; if (p==12) p=0;
	m[p++]+=r[4]; m[p++]+=r[3]; if (p==12) p=0;
	m[p++]+=r[2]; m[p++]+=r[1]; if (p==12) p=0;
	m[p++]+=r[0];
}
#elif DOWNSAMPLE_FACTOR == 4
#define CS_INTERP_DELAY_FRAMES (3*DOWNSAMPLE_FACTOR-1)
struct cs_interp_state {
	double y[4], m[16];
	int p;
};

static void cs_interp_insert(struct cs_interp_state *state, double x)
{
	const double r[8] = {
		8.707604904333586e-03*x, 3.955155321828940e-02*x,
		1.024343698348400e-01*x, 2.023809523809523e-01*x,
		3.302221271950125e-01*x, 4.604484467817105e-01*x,
		5.586358980658138e-01*x, 5.952380952380951e-01*x,
	};
	int p = state->p;
	double *m = state->m, *y = state->y;
	y[0]=m[p++]+r[0]; y[1]=m[p++]+r[1]; y[2]=m[p++]+r[2]; y[3]=m[p++]+r[3];
	memset(&m[state->p], 0, sizeof(double)*4);
	p &= 0xf;
	state->p = p;
	m[p++]+=r[4]; m[p++]+=r[5]; m[p++]+=r[6]; m[p++]+=r[7];
	p &= 0xf;
	m[p++]+=r[6]; m[p++]+=r[5]; m[p++]+=r[4]; m[p++]+=r[3];
	p &= 0xf;
	m[p++]+=r[2]; m[p++]+=r[1]; m[p++]+=r[0];
}
#elif DOWNSAMPLE_FACTOR == 8
#define CS_INTERP_DELAY_FRAMES (3*DOWNSAMPLE_FACTOR-1)
struct cs_interp_state {
	double y[8], m[32];
	int p;
};

static void cs_interp_insert(struct cs_interp_state *state, double x)
{
	const double r[16] = {
		2.093882380528402e-03*x, 8.707604904333586e-03*x,
		2.076182645115152e-02*x, 3.955155321828940e-02*x,
		6.642869577439980e-02*x, 1.024343698348400e-01*x,
		1.479431407089482e-01*x, 2.023809523809523e-01*x,
		2.640683323852150e-01*x, 3.302221271950125e-01*x,
		3.971252630479725e-01*x, 4.604484467817105e-01*x,
		5.156842147264761e-01*x, 5.586358980658138e-01*x,
		5.858946445253084e-01*x, 5.952380952380952e-01*x,
	};
	int p = state->p;
	double *m = state->m, *y = state->y;
	y[0]=m[p++]+r[0]; y[1]=m[p++]+r[1]; y[2]=m[p++]+r[2]; y[3]=m[p++]+r[3];
	y[4]=m[p++]+r[4]; y[5]=m[p++]+r[5]; y[6]=m[p++]+r[6]; y[7]=m[p++]+r[7];
	memset(&m[state->p], 0, sizeof(double)*8);
	p &= 0x1f;
	state->p = p;
	m[p++]+=r[8];  m[p++]+=r[9];  m[p++]+=r[10]; m[p++]+=r[11];
	m[p++]+=r[12]; m[p++]+=r[13]; m[p++]+=r[14]; m[p++]+=r[15];
	p &= 0x1f;
	m[p++]+=r[14]; m[p++]+=r[13]; m[p++]+=r[12]; m[p++]+=r[11];
	m[p++]+=r[10]; m[p++]+=r[9];  m[p++]+=r[8];  m[p++]+=r[7];
	p &= 0x1f;
	m[p++]+=r[6]; m[p++]+=r[5]; m[p++]+=r[4]; m[p++]+=r[3];
	m[p++]+=r[2]; m[p++]+=r[1]; m[p++]+=r[0];
}
#else
	#error "unsupported DOWNSAMPLE_FACTOR"
#endif
static inline double cs_interp(const struct cs_interp_state *state, int x)
{
	return state->y[x];
}
#else
	#error "illegal CS_INTERP_TYPE"
#endif
#else
/* dummy definitions for DOWNSAMPLE_FACTOR=1 */
struct cs_interp_state {
	double m;
};
#define cs_interp_insert(s, x) do { (s)->m = x; } while(0)
#define cs_interp(s, x) ((s)->m)
#define CS_INTERP_PEEK(s) ((s)->m)
#endif

static void smooth_state_init(struct smooth_state *sm, const struct stream_info *istream)
{
	for (int i = 0; i < 4; ++i) ewma_init(&sm->env[i], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&sm->pwr_env[i], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
}

static void event_state_init(struct event_state *ev, const struct stream_info *istream, double thresh_scale)
{
	for (int i = 0; i < 6; ++i) ewma_init(&ev->accom[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(ACCOM_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->norm[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(NORM_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&ev->slow[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(RISE_TIME_SLOW));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->smooth[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(EVENT_SMOOTH_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&ev->avg[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->drift[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(ACCOM_TIME*2.0));
	for (int i = 2; i < 4; ++i) ewma_init(&ev->drift[i], DOWNSAMPLED_FS(istream->fs), EWMA_RISE_TIME(RISE_TIME_FAST*2.0));
	ev->t_hold = -2;
	ev->buf_len = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, DOWNSAMPLED_FS(istream->fs));
	ev->ord_buf = calloc(ev->buf_len, sizeof(struct axes));
	ev->env_buf = calloc(ev->buf_len, sizeof(struct envs));
	ev->adapt_buf = calloc(ev->buf_len, sizeof(struct envs));
	ev->thresh = EVENT_THRESH * thresh_scale;
	ev->end_thresh = EVENT_END_THRESH * thresh_scale;
	ev->clip_thresh = EVENT_THRESH * 10.0 * thresh_scale;
	#if DEBUG_PRINT_MIN_RISE_TIME
		ev->max_diff_scale = ev->max_ord_scale = 1.0;
		ev->fs = DOWNSAMPLED_FS(istream->fs);
	#endif
}

static void event_state_cleanup(struct event_state *ev)
{
	free(ev->ord_buf);
	free(ev->env_buf);
	free(ev->adapt_buf);
	#if DEBUG_PRINT_MIN_RISE_TIME
		#define EWMA_CONST_TO_RT(x, fs) (-1.0/log(1.0-(x))/(fs)*1000.0*2.1972)
		#define EWMA_RT_TO_CONST(x, fs) (1.0-exp(-1.0/((fs)*((x)/1000.0/2.1972))))
		LOG_FMT(LL_VERBOSE, "%s(): minimum rise time: ord=%gms; diff=%gms", __func__,
			EWMA_CONST_TO_RT(EWMA_RT_TO_CONST(ACCOM_TIME*2.0, ev->fs)*ev->max_ord_scale, ev->fs),
			EWMA_CONST_TO_RT(EWMA_RT_TO_CONST(RISE_TIME_FAST*2.0, ev->fs)*ev->max_diff_scale, ev->fs));
	#endif
}

static void event_config_init(struct event_config *evc, const struct stream_info *istream)
{
	evc->sample_frames = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, DOWNSAMPLED_FS(istream->fs));
	evc->max_hold_frames = TIME_TO_FRAMES(EVENT_MAX_HOLD_TIME, DOWNSAMPLED_FS(istream->fs));
	evc->min_hold_frames = TIME_TO_FRAMES(EVENT_MIN_HOLD_TIME, DOWNSAMPLED_FS(istream->fs));
	evc->ord_factor_c = exp(-1.0/(DOWNSAMPLED_FS(istream->fs)*ORD_FACTOR_DECAY));
}

static inline double fade_mult(ssize_t pos, ssize_t n, int is_out)
{
	double fade = (double) (n-pos) / n;
	if (is_out) fade = 1.0 - fade;
#if FADE_TYPE == 1
	return fade;
#elif FADE_TYPE == 2
	return sin(fade*M_PI_2);
#elif FADE_TYPE == 3
	return (1.0 - cos(fade*M_PI)) * 0.5;
#elif FADE_TYPE == 4
	return (fade <= 0.5) ? 4.0 * fade*fade*fade : 1.0 - 4.0 * (1.0-fade)*(1.0-fade)*(1.0-fade);
#else
	#error "illegal FADE_TYPE"
#endif
}

static void calc_input_envs(struct smooth_state *sm, double l, double r, struct envs *env, struct envs *pwr_env)
{
	const double sum = l+r, diff = l-r;

	env->l = ewma_run(&sm->env[0], fabs(l));
	env->r = ewma_run(&sm->env[1], fabs(r));
	env->sum = ewma_run(&sm->env[2], fabs(sum));
	env->diff = ewma_run(&sm->env[3], fabs(diff));

	pwr_env->l = ewma_run(&sm->pwr_env[0], l*l);
	pwr_env->r = ewma_run(&sm->pwr_env[1], r*r);
	pwr_env->sum = ewma_run(&sm->pwr_env[2], sum*sum);
	pwr_env->diff = ewma_run(&sm->pwr_env[3], diff*diff);
}

static void process_events(struct event_state *ev, const struct event_config *evc, const struct envs *env, const struct envs *pwr_env, struct axes *ax, struct axes *ax_ev)
{
	const struct axes ord = {
		.lr = CALC_LR(env->l, env->r, env->l/env->r),
		.cs = CALC_CS(env->sum, env->diff, env->sum/env->diff),
	};
	const struct envs adapt = {
		.l = pwr_env->l - ewma_run_set_max(&ev->accom[0], pwr_env->l),
		.r = pwr_env->r - ewma_run_set_max(&ev->accom[1], pwr_env->r),
		.sum = pwr_env->sum - ewma_run_set_max(&ev->accom[2], pwr_env->sum),
		.diff = pwr_env->diff - ewma_run_set_max(&ev->accom[3], pwr_env->diff),
	};
	const struct axes diff = {
		.lr = CALC_LR(adapt.l, adapt.r, sqrt(adapt.l/adapt.r)),
		.cs = CALC_CS(adapt.sum, adapt.diff, sqrt(adapt.sum/adapt.diff)),
	};
	const struct axes ord_d = ev->ord_buf[ev->buf_p];
	ev->ord_buf[ev->buf_p] = ord;
	const struct envs env_d = ev->env_buf[ev->buf_p];
	ev->env_buf[ev->buf_p] = *env;
	const struct envs adapt_d = ev->adapt_buf[ev->buf_p];
	ev->adapt_buf[ev->buf_p] = adapt;
	ev->buf_p = (ev->buf_p + 1 >= ev->buf_len) ? 0 : ev->buf_p + 1;

	ev->adj = 1.0 - ev->ord_factor/20.0;
	ev->adj = (ev->adj > 0.5) ? ev->adj : 0.5;
	ev->ord_factor *= evc->ord_factor_c;

	const double l_pwr_xf = pwr_env->l*(1.0-NORM_CROSSFEED) + pwr_env->r*NORM_CROSSFEED;
	const double r_pwr_xf = pwr_env->r*(1.0-NORM_CROSSFEED) + pwr_env->l*NORM_CROSSFEED;
	const double l_norm_div = ewma_run(&ev->norm[0], fabs(l_pwr_xf - ewma_run(&ev->slow[0], l_pwr_xf)*NORM_ACCOM_FACTOR*ev->adj));
	const double r_norm_div = ewma_run(&ev->norm[1], fabs(r_pwr_xf - ewma_run(&ev->slow[1], r_pwr_xf)*NORM_ACCOM_FACTOR*ev->adj));
	ewma_run_scale_asym(&ev->accom[4], pwr_env->l, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	ewma_run_scale_asym(&ev->accom[5], pwr_env->r, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	const double l_mask = MAXIMUM(pwr_env->l - ewma_get_last(&ev->accom[4]), 0.0);
	const double r_mask = MAXIMUM(pwr_env->r - ewma_get_last(&ev->accom[5]), 0.0);
	const double l_mask_norm = (!NEAR_POS_ZERO(l_norm_div)) ? l_mask / l_norm_div : (NEAR_POS_ZERO(l_mask)) ? 0.0 : ev->clip_thresh;
	const double r_mask_norm = (!NEAR_POS_ZERO(r_norm_div)) ? r_mask / r_norm_div : (NEAR_POS_ZERO(r_mask)) ? 0.0 : ev->clip_thresh;
	const double l_mask_norm_sm = ewma_run(&ev->smooth[0], MINIMUM(l_mask_norm, ev->clip_thresh));
	const double r_mask_norm_sm = ewma_run(&ev->smooth[1], MINIMUM(r_mask_norm, ev->clip_thresh));
	const double l_event = (l_mask_norm_sm - ewma_run(&ev->slow[2], l_mask_norm_sm)) * ev->adj;
	const double r_event = (r_mask_norm_sm - ewma_run(&ev->slow[3], r_mask_norm_sm)) * ev->adj;

	if (!ev->sample && (l_event > ev->thresh || r_event > ev->thresh)) {
		ev->sample = 1;
		ev->flags[1] = 0;
		ev->flags[1] |= (l_event >= r_event) ? EVENT_FLAG_L : 0;
		ev->flags[1] |= (r_event >= l_event) ? EVENT_FLAG_R : 0;
		ev->t_sample = ev->t;
		if (ev->t - ev->t_hold > 1) {
			ewma_set(&ev->avg[0], ord.lr);
			ewma_set(&ev->avg[1], ord.cs);
			ewma_set(&ev->avg[2], ord.lr);
			ewma_set(&ev->avg[3], ord.cs);
			ev->max = 0.0;
		}
		else ev->flags[1] |= EVENT_FLAG_FUSE;
	}

	if (ev->sample) {
		ewma_run(&ev->avg[0], ord.lr);
		ewma_run(&ev->avg[1], ord.cs);
		ewma_run(&ev->avg[2], diff.lr);
		ewma_run(&ev->avg[3], diff.cs);
		if (l_event > ev->max) ev->max = l_event;
		if (r_event > ev->max) ev->max = r_event;
		if (ev->t - ev->t_sample >= evc->sample_frames) {
			ev->sample = 0;
			if (fabs(ewma_get_last(&ev->avg[2]))+fabs(ewma_get_last(&ev->avg[3])) > M_PI_4*1.001)
				ev->flags[1] |= EVENT_FLAG_USE_ORD;
			const double wx = (ev->max - ev->thresh) / (ev->thresh*0.3);
			const double new_ev_weight = (wx >= 1.0) ? 1.0 : wx*wx*(3.0-2.0*wx);
			if (((ev->flags[1] & EVENT_FLAG_FUSE) && (ev->flags[1] & EVENT_FLAG_USE_ORD) && !(ev->flags[0] & EVENT_FLAG_USE_ORD))
					|| (ev->hold && new_ev_weight < ev->weight)) {
				++ev->ignore_count;
				/* LOG_FMT(LL_VERBOSE, "%s(): ignoring event: lr: %+06.2f°; cs: %+06.2f°",
					__func__, TO_DEGREES(ev->dir.lr), TO_DEGREES(ev->dir.cs)); */
			}
			else {
				if (ev->hold && new_ev_weight != ev->weight) {
					/* needed to avoid discontinuities if ev->weight changes */
					ewma_set(&ev->drift[0], ewma_set(&ev->drift[2],
						ev->drift_last[1].lr*ev->weight + ev->drift_last[0].lr*(1.0-ev->weight)));
					ewma_set(&ev->drift[1], ewma_set(&ev->drift[3],
						ev->drift_last[1].cs*ev->weight + ev->drift_last[0].cs*(1.0-ev->weight)));
				}
				ev->hold = 1;
				ev->t_hold = ev->t;
				ev->dir.lr = ewma_get_last(&ev->avg[2]);
				ev->dir.cs = ewma_get_last(&ev->avg[3]);
				if (ev->flags[1] & EVENT_FLAG_USE_ORD) {
					ev->dir.lr = ewma_get_last(&ev->avg[0]);
					ev->dir.cs = ewma_get_last(&ev->avg[1]);
					ev->ord_factor += 1.0;
					if (!(ev->flags[1] & EVENT_FLAG_FUSE))
						++ev->ord_count;
				}
				else if (!(ev->flags[1] & EVENT_FLAG_FUSE))
					++ev->diff_count;
				ev->flags[0] = ev->flags[1];
				ev->weight = new_ev_weight;
				/* LOG_FMT(LL_VERBOSE, "%s(): event: type: %4s; lr: %+06.2f°; cs: %+06.2f°",
					__func__, (ev->flags[1] & EVENT_FLAG_USE_ORD) ? "ord" : "diff",
					TO_DEGREES(ev->dir.lr), TO_DEGREES(ev->dir.cs)); */
			}
		}
	}
	const double ds_ord = drift_scale(&ev->drift_last[0], &ord_d, &env_d, ORD_SENS_ERR, ORD_SENS_LEVEL);
	#if DEBUG_PRINT_MIN_RISE_TIME
		ev->max_ord_scale = MAXIMUM(ev->max_ord_scale, ds_ord);
	#endif
	ax->lr = ewma_run_scale(&ev->drift[0], ord_d.lr, ds_ord);
	ax->cs = ewma_run_scale(&ev->drift[1], ord_d.cs, ds_ord);
	ev->drift_last[0] = *ax;
	if (ev->hold) {
		const double ds_diff = drift_scale(&ev->drift_last[1], &ev->dir, (ev->flags[0] & EVENT_FLAG_USE_ORD) ? &env_d : &adapt_d, DIFF_SENS_ERR, DIFF_SENS_LEVEL);
		#if DEBUG_PRINT_MIN_RISE_TIME
			ev->max_diff_scale = MAXIMUM(ev->max_diff_scale, ds_diff);
		#endif
		ax_ev->lr = ewma_run_scale(&ev->drift[2], ev->dir.lr, ds_diff);
		ax_ev->cs = ewma_run_scale(&ev->drift[3], ev->dir.cs, ds_diff);
		ev->drift_last[1] = *ax_ev;
		ax->lr = ax_ev->lr*ev->weight + ax->lr*(1.0-ev->weight);
		ax->cs = ax_ev->cs*ev->weight + ax->cs*(1.0-ev->weight);
		if ((ev->flags[0] & EVENT_FLAG_L && l_mask_norm_sm <= ev->end_thresh)
				|| (ev->flags[0] & EVENT_FLAG_R && r_mask_norm_sm <= ev->end_thresh)) {
			ev->flags[0] |= EVENT_FLAG_END;
		}
		if ((ev->t - ev->t_hold >= evc->min_hold_frames && ev->flags[0] & EVENT_FLAG_END)
				|| ev->t - ev->t_hold >= evc->max_hold_frames) {
			if (ev->t - ev->t_hold < evc->max_hold_frames) ++ev->early_count;
			ev->hold = 0;
			ewma_set(&ev->drift[0], ewma_set(&ev->drift[2], ax->lr));
			ewma_set(&ev->drift[1], ewma_set(&ev->drift[3], ax->cs));
			ev->drift_last[0] = ev->drift_last[1] = *ax;
		}
	}
	else {
		ax_ev->lr = ax_ev->cs = 0.0;
		ewma_set(&ev->drift[2], ax->lr);
		ewma_set(&ev->drift[3], ax->cs);
		ev->drift_last[1] = *ax;
	}
	++ev->t;
}

static void norm_axes(struct axes *ax)
{
	const double abs_sum = fabs(ax->lr)+fabs(ax->cs);
	if (abs_sum > M_PI_4) {
		const double norm = M_PI_4 / abs_sum;
		ax->lr *= norm;
		ax->cs *= norm;
	}
}

static void calc_matrix_coefs(const struct axes *ax, int do_dir_boost, double norm_mult, double surr_mult, struct matrix_coefs *m)
{
	const double lr = ax->lr, cs = ax->cs;
	const double abs_lr = fabs(lr);

	/* The matrix coefficients during front steering are from
	   "Multichannel matrix surround decoders for two-eared listeners" by
	   David Griesinger (http://www.davidgriesinger.com/sur.pdf). I've
	   simplified the equations and corrected gsl so there is full
	   cancellation when |lr|+|cs|=45°.
	*/
	const double gl = 1.0+tan(abs_lr-M_PI_4);
	const double gsl = gl*gl;
	if (cs >= 0.0) {
		const double gc = 1.0+tan(cs-M_PI_4);
		if (lr >= 0.0) {
			m->lsl = 1.0-gsl-0.5*gc;
			m->lsr = -0.5*gc-gl;
			m->rsl = -0.5*gc;
			m->rsr = 1.0-0.5*gc;
		}
		else {
			m->lsl = 1.0-0.5*gc;
			m->lsr = -0.5*gc;
			m->rsl = -0.5*gc-gl;
			m->rsr = 1.0-gsl-0.5*gc;
		}
	}
	else {
		if (lr >= 0.0) {
			if (cs > -M_PI_4/2) {
				m->lsl = 1.0-gsl*(1.0+sin(3.0*cs));
				m->lsr = -gl*cos(3.0*cs);
			}
			else {
				m->lsl = 1.0-gsl*(1.0+sin(cs-M_PI_4));
				m->lsr = -gl*cos(cs-M_PI_4);
			}
			m->rsl = 0.0;
			m->rsr = 1.0;
		}
		else {
			m->lsl = 1.0;
			m->lsr = 0.0;
			if (cs > -M_PI_4/2) {
				m->rsl = -gl*cos(3.0*cs);
				m->rsr = 1.0-gsl*(1.0+sin(3.0*cs));
			}
			else {
				m->rsl = -gl*cos(cs-M_PI_4);
				m->rsr = 1.0-gsl*(1.0+sin(cs-M_PI_4));
			}
		}
	}

	/* Power correction and scaling */
	const double surr_gain = norm_mult*surr_mult;
	const double ls_m_scale = surr_gain/sqrt(m->lsl*m->lsl + m->lsr*m->lsr);
	const double rs_m_scale = surr_gain/sqrt(m->rsl*m->rsl + m->rsr*m->rsr);
	m->lsl *= ls_m_scale;
	m->lsr *= ls_m_scale;
	m->rsl *= rs_m_scale;
	m->rsr *= rs_m_scale;

	m->dir_boost = 0.0;
	if (do_dir_boost) {
		const double b_gc = (cs > 0.0) ? 1.0+tan(abs_lr+cs-M_PI_4) : gl;
		const double b_gc2 = b_gc*b_gc;
		const double b_lr_c0 = (1.0-b_gc2)*(1.0-b_gc2);
		const double b_lr_c1 = (b_gc2-b_gc)*(b_gc2-b_gc);
		const double b_lr_gu2 = surr_gain*surr_gain*(b_lr_c0+b_lr_c1)/(b_lr_c0+b_gc2);
		const double b_lr = sqrt(1.0-b_lr_gu2)-norm_mult;
		if (cs > 0.0) {
			const double b_cs_gu = surr_gain*(1.0-b_gc)/(1.0-b_gc+0.5*b_gc2);
			const double b_cs = sqrt(1.0-b_cs_gu*b_cs_gu)-norm_mult;
			const double b_cs_weight = cs/sqrt(lr*lr+cs*cs);  /* FIXME: slight error along diagonals */
			m->dir_boost = b_lr*(1.0-b_cs_weight) + b_cs*b_cs_weight;
		}
		else m->dir_boost = b_lr;
		if (cs < 0.0) m->dir_boost *= ((cs>-M_PI_4/2)?cos(3.0*cs):cos(cs-M_PI_4));
	}
}
#endif

#endif
