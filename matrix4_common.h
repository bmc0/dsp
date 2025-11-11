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
#define DELAY_TIME (EVENT_SAMPLE_TIME + RISE_TIME_FAST*0.4)
#define ORD_SENS_ERR          5.0
#define ORD_SENS_WEIGHT       3.0
#define ORD_WEIGHT_THRESH     0.3
#define DIFF_SENS_WEIGHT      2.0
#define DIFF_WEIGHT_SCALE     2.5

#define SHELF_MULT_DEFAULT    1.0
#define SHELF_F0_DEFAULT    500.0
#define SHELF_PWRCMP_DEFAULT  1.0
#define LOWPASS_F0_DEFAULT    0.0

#define FILTER_BANK_TYPE_DEFAULT FILTER_BANK_TYPE_ELLIPTIC

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

/* 1 = linear; 2 = parabolic 2x; 3 = cubic B-spline; 4 = cubic Hermite; 5 = polyphase FIR (Blackman window) */
#ifndef CS_INTERP_TYPE
#if DOWNSAMPLE_FACTOR == 4 || DOWNSAMPLE_FACTOR == 2
	#define CS_INTERP_TYPE 5
#else
	#define CS_INTERP_TYPE 2
#endif
#endif

#define ENABLE_LOOKBACK 1
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
	double ll, lr, rl, rr;
	double lsl, lsr, rsl, rsr;
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
	struct ewma_state accom[6], norm[4], slow[2], smooth[2], avg[4];
	struct ewma_state drift[4], drift_scale[2];
	struct axes dir, diff_last, *ord_buf;
	#if ENABLE_LOOKBACK
		struct axes *diff_buf;
		double (*slope_buf)[2];
	#endif
	struct envs *env_buf;
	double last[2], slope_last[2], clip_thresh, max[2];
	double ord_factor, adj, ds_diff, *ds_ord_buf;
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

enum status_type {
	STATUS_TYPE_NONE = 0,
	STATUS_TYPE_BARS,
	STATUS_TYPE_TEXT,
};

enum filter_bank_type {
	FILTER_BANK_TYPE_BUTTERWORTH = 0,
	FILTER_BANK_TYPE_CHEBYSHEV1,
	FILTER_BANK_TYPE_CHEBYSHEV2,
	FILTER_BANK_TYPE_ELLIPTIC,
};

typedef void (*calc_matrix_coefs_func)(const struct axes *, double, double, struct matrix_coefs *, double *);

struct matrix4_config {
	int n_channels, opt_str_idx, c0, c1, enable_signal;
	double surr_mult, shelf_mult, shelf_f0, shelf_pwrcmp, lowpass_f0, fb_stop[2];
	ssize_t surr_delay_frames;
	enum status_type status_type;
	enum filter_bank_type fb_type;
	calc_matrix_coefs_func calc_matrix_coefs;
};

#define CALC_NORM_MULT(x) (1.0 / sqrt(1.0 + (x)*(x)))
#define TO_DEGREES(x) ((x)*M_1_PI*180.0)
#define TIME_TO_FRAMES(x, fs) lround((x)/1000.0 * (fs))
#define NEAR_POS_ZERO(x) ((x) < DBL_MIN)
#define ANGLE(n, d, expr) ((NEAR_POS_ZERO(n) && NEAR_POS_ZERO(d)) ? M_PI_4 : (NEAR_POS_ZERO(d)) ? M_PI_2 : atan(expr))
#define CALC_LR(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)
#define CALC_CS(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)
#define DOWNSAMPLED_FS(fs) (((double) (fs)) / DOWNSAMPLE_FACTOR)
#define CBUF_NEXT(x, len) (((x)+1<(len))?(x)+1:0)
#define CBUF_PREV(x, len) (((x)>0)?(x)-1:(len)-1)

int get_args_and_channels(const struct effect_info *, const struct stream_info *, const char *, int, const char *const *, struct matrix4_config *);
int parse_effect_opts(const char *const *, const struct stream_info *, struct matrix4_config *);
void smooth_state_init(struct smooth_state *, const struct stream_info *);
void event_state_cleanup(struct event_state *);

#ifndef LADSPA_FRONTEND
struct steering_bar {
	char s[32];
	int e;
};
void draw_steering_bar(double, int, struct steering_bar *);
#endif

/* private functions */
void event_state_init_priv(struct event_state *, double, double);
void event_config_init_priv(struct event_config *, double);
void process_events_priv(struct event_state *, const struct event_config *, const struct envs *, const struct envs *, double, double, struct axes *, struct axes *);

struct effect * matrix4_delay_effect_init(const struct effect_info *, const struct stream_info *, ssize_t);

static inline double smoothstep_nc(double x)
{
	return x*x*(3.0-2.0*x);
}

static inline double smoothstep(double x)
{
	if (x >= 1.0) return 1.0;
	if (x <= 0.0) return 0.0;
	return smoothstep_nc(x);
}

#ifndef DSP_MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS
static void event_state_init(struct event_state *ev, const struct stream_info *istream)
{
	event_state_init_priv(ev, DOWNSAMPLED_FS(istream->fs), NORM_ACCOM_FACTOR);
}

static void event_config_init(struct event_config *evc, const struct stream_info *istream)
{
	event_config_init_priv(evc, DOWNSAMPLED_FS(istream->fs));
}

static void process_events(struct event_state *ev, const struct event_config *evc, const struct envs *env, const struct envs *pwr_env, double thresh_scale, struct axes *ax, struct axes *ax_ev)
{
	process_events_priv(ev, evc, env, pwr_env, NORM_ACCOM_FACTOR, thresh_scale, ax, ax_ev);
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

static inline void calc_input_envs(struct smooth_state *sm, double l, double r, struct envs *env, struct envs *pwr_env)
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

static inline void norm_axes(struct axes *ax)
{
	const double abs_sum = fabs(ax->lr)+fabs(ax->cs);
	if (abs_sum > M_PI_4) {
		const double norm = M_PI_4 / abs_sum;
		ax->lr *= norm;
		ax->cs *= norm;
	}
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
	c[0] = (1.0/2.0)*y[1] + (1.0/4.0)*(y[0]+y[2]);
	c[1] = (1.0/2.0)*a;
	c[2] = (1.0/4.0)*(y[3]-y[1]-a);
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
	c[0] = (1.0/6.0)*a + (2.0/3.0)*y[1];
	c[1] = (1.0/2.0)*(y[2]-y[0]);
	c[2] = (1.0/2.0)*a - y[1];
	c[3] = (1.0/2.0)*(y[1]-y[2]) + (1.0/6.0)*(y[3]-y[0]);
}

static inline double cs_interp(const struct cs_interp_state *s, int x)
{
	const double *c = s->c, t = x * (1.0/DOWNSAMPLE_FACTOR);
	return ((c[3]*t+c[2])*t+c[1])*t+c[0];
}
#elif CS_INTERP_TYPE == 4
/* cubic Hermite */
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
	c[0] = y[1];
	c[1] = (1.0/2.0)*(y[2]-y[0]);
	c[2] = y[0] - (5.0/2.0)*y[1] + 2.0*y[2] - (1.0/2.0)*y[3];
	c[3] = (1.0/2.0)*(y[3]-y[0]) + (3.0/2.0)*(y[1]-y[2]);
}

static inline double cs_interp(const struct cs_interp_state *s, int x)
{
	const double *c = s->c, t = x * (1.0/DOWNSAMPLE_FACTOR);
	return ((c[3]*t+c[2])*t+c[1])*t+c[0];
}
#elif CS_INTERP_TYPE == 5
/* polyphase FIR (Blackman window) */
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
#endif /* DSP_MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS */

#endif
