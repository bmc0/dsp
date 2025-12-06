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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define DSP_MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS
#include "matrix4_common.h"
#include "dsp.h"

void calc_matrix_coefs_v1(const struct axes *, double, double, int, struct matrix_coefs *, double [2]);
void calc_matrix_coefs_v2(const struct axes *, double, double, int, struct matrix_coefs *, double [2]);
void calc_matrix_coefs_v3(const struct axes *, double, double, int, struct matrix_coefs *, double [2]);

int get_args_and_channels(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, int argc, const char *const *argv, struct matrix4_config *config)
{
	double surr_level[2] = {NAN, NAN};
	char *endptr;
	if (argc > 3) {
		print_effect_usage(ei);
		return 1;
	}
	int surr_level_idx = -1;
	config->opt_str_idx = -1;
	if (argc == 3) {
		config->opt_str_idx = 1;
		surr_level_idx = 2;
	}
	else if (argc == 2) {
		strtod(argv[1], &endptr);
		if (endptr == argv[1] || (*endptr != '\0' && *endptr != '/'))
			config->opt_str_idx = 1;
		else surr_level_idx = 1;
	}
	if (surr_level_idx > 0) {
		const char *arg = argv[surr_level_idx];
		double v = strtod(arg, &endptr);
		if (endptr != arg) surr_level[0] = v;
		if (*endptr == '/') {
			arg = endptr+1;
			surr_level[1] = strtod(arg, &endptr);
			CHECK_ENDPTR(arg, endptr, "surround_level_rear", return 1);
		}
		else {
			CHECK_ENDPTR(arg, endptr, "surround_level", return 1);
			if (!isnan(surr_level[0]))
				surr_level[1] = MINIMUM(surr_level[0]+6.02, 0.0);
		}
	}
	config->surr_mult[0] = (isnan(surr_level[0])) ? SURR_MULT_DEFAULT : pow(10.0, surr_level[0] / 20.0);
	config->surr_mult[1] = (isnan(surr_level[1])) ? SURR_MULT_REAR_DEFAULT : pow(10.0, surr_level[1] / 20.0);
	if (config->surr_mult[0] > 1.0 || config->surr_mult[1] > 1.0)
		LOG_FMT(LL_ERROR, "%s: warning: surround levels probably shouldn't be greater than 0dB", argv[0]);
	if (config->surr_mult[0] > config->surr_mult[1])
		LOG_FMT(LL_ERROR, "%s: warning: surround_level_rear probably shouldn't be lower than surround_level", argv[0]);

	if (istream->fs < 32000) {
		LOG_FMT(LL_ERROR, "%s: error: sample rate out of range", argv[0]);
		return 1;
	}
	config->n_channels = num_bits_set(channel_selector, istream->channels);
	if (config->n_channels != 2) {
		LOG_FMT(LL_ERROR, "%s: error: number of input channels must be 2", argv[0]);
		return 1;
	}
	config->c0 = config->c1 = -1;
	for (int i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (config->c0 == -1)
				config->c0 = i;
			else
				config->c1 = i;
		}
	}
	return 0;
}

static int is_opt(const char *opt, const char *name)
{
	size_t len = strlen(name);
	if (len > 1 && name[len-1] == '=')
		return (strncmp(opt, name, len-1) == 0 && (opt[len-1] == '\0' || opt[len-1] == '='));
	return (strcmp(opt, name) == 0);
}

static void set_fb_stop_default(struct matrix4_config *config)
{
	switch (config->fb_type) {
	case FILTER_BANK_TYPE_BUTTERWORTH:
		config->fb_stop[0] = 0.0;
		config->fb_stop[1] = 0.0;
		break;
	case FILTER_BANK_TYPE_CHEBYSHEV1:
	case FILTER_BANK_TYPE_CHEBYSHEV2:
		config->fb_stop[0] = 25.0;
		config->fb_stop[1] = 0.0;
		break;
	case FILTER_BANK_TYPE_ELLIPTIC:
		config->fb_stop[0] = 35.0;
		config->fb_stop[1] = 50.0;
		break;
	}
}

#define HANDLE_BOOLEAN_ARG(s) \
	do { \
		char *opt_arg = isolate(opt, '='); \
		if (*opt_arg == '\0' || strcmp(opt_arg, "true") == 0) (s) = 1; \
		else if (strcmp(opt_arg, "false") == 0) (s) = 0; \
		else { \
			LOG_FMT(LL_ERROR, "%s: error: unrecognized argument: %s", argv[0], opt_arg); \
			goto fail; \
		} \
	} while (0)
#define CONCAT(a, b) a ## b
#define GET_MATRIX_FUNC(id) CONCAT(calc_matrix_coefs_, id)

int parse_effect_opts(const char *const *argv, const struct stream_info *istream, const int is_mb, struct matrix4_config *config)
{
	char *opt_str = NULL;
	config->surr_delay_frames = TIME_TO_FRAMES(SURR_DELAY_DEFAULT, istream->fs);
	config->shelf_mult = SHELF_MULT_DEFAULT;
	config->shelf_f0 = SHELF_F0_DEFAULT;
	config->shelf_pwrcmp = (is_mb) ? SHELF_PWRCMP_MB_DEFAULT : SHELF_PWRCMP_DEFAULT;
	config->lowpass_f0 = LOWPASS_F0_DEFAULT;
	config->do_phase_flip = DO_PHASE_FLIP_DEFAULT;
	config->fb_type = FILTER_BANK_TYPE_DEFAULT;
	set_fb_stop_default(config);
	config->calc_matrix_coefs = GET_MATRIX_FUNC(MATRIX_ID_DEFAULT);
	if (config->opt_str_idx > 0) {
		opt_str = strdup(argv[config->opt_str_idx]);
		char *opt = opt_str, *next_opt, *endptr;
		while (*opt != '\0') {
			next_opt = isolate(opt, ',');
			if (*opt == '\0') /* do nothing */;
			else if (is_opt(opt, "show_status=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0' || strcmp(opt_arg, "bars") == 0)
					config->status_type = STATUS_TYPE_BARS;
				else if (strcmp(opt_arg, "text") == 0)
					config->status_type = STATUS_TYPE_TEXT;
				else if (strcmp(opt_arg, "none") == 0)
					config->status_type = STATUS_TYPE_NONE;
				else {
					LOG_FMT(LL_ERROR, "%s: error: unrecognized argument: %s", argv[0], opt_arg);
					goto fail;
				}
			}
			else if (is_opt(opt, "matrix=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') goto needs_arg;
				if (strcmp(opt_arg, "v1") == 0)
					config->calc_matrix_coefs = calc_matrix_coefs_v1;
				else if (strcmp(opt_arg, "v2") == 0)
					config->calc_matrix_coefs = calc_matrix_coefs_v2;
				else if (strcmp(opt_arg, "v3") == 0)
					config->calc_matrix_coefs = calc_matrix_coefs_v3;
				else {
					LOG_FMT(LL_ERROR, "%s: error: unrecognized matrix identifier: %s", argv[0], opt_arg);
					goto fail;
				}
			}
			else if (is_opt(opt, "shelf=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') goto needs_arg;
				char *opt_subarg = isolate(opt_arg, ':');
				char *opt_subarg1 = isolate(opt_subarg, ':');
				double shelf_gain = strtod(opt_arg, &endptr);
				CHECK_ENDPTR(opt_arg, endptr, "shelf: gain", goto fail);
				if (shelf_gain > 0.0)
					LOG_FMT(LL_ERROR, "%s: warning: shelf gain probably shouldn't be greater than 0dB", argv[0]);
				config->shelf_mult = pow(10.0, shelf_gain / 20.0);
				if (*opt_subarg != '\0') {
					config->shelf_f0 = parse_freq(opt_subarg, &endptr);
					CHECK_ENDPTR(opt_subarg, endptr, "shelf: f0", goto fail);
					CHECK_RANGE(config->shelf_f0 >= 100.0 && config->shelf_f0 <= 6000.0, "shelf: f0", goto fail);
				}
				if (*opt_subarg1 != '\0') {
					config->shelf_pwrcmp = strtod(opt_subarg1, &endptr);
					CHECK_ENDPTR(opt_subarg, endptr, "shelf: pwrcmp", goto fail);
					CHECK_RANGE(config->shelf_pwrcmp >= 0.0 && config->shelf_pwrcmp <= 1.0, "shelf: pwrcmp", goto fail);
				}
			}
			else if (is_opt(opt, "lowpass=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') goto needs_arg;
				if (strcmp(opt_arg, "none") == 0) config->lowpass_f0 = 0.0;
				else {
					config->lowpass_f0 = parse_freq(opt_arg, &endptr);
					CHECK_ENDPTR(opt_arg, endptr, "lowpass: f0", goto fail);
					CHECK_FREQ(config->lowpass_f0, istream->fs, "lowpass: f0", goto fail);
				}
			}
			else if (is_opt(opt, "phase_flip=")) {
				HANDLE_BOOLEAN_ARG(config->do_phase_flip);
			}
			else if (is_opt(opt, "signal=")) {
				HANDLE_BOOLEAN_ARG(config->enable_signal);
			}
			else if (is_opt(opt, "surround_delay=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') goto needs_arg;
				config->surr_delay_frames = parse_len(opt_arg, istream->fs, &endptr);
				CHECK_ENDPTR(opt_arg, endptr, "surround_delay", goto fail);
			}
			else if (is_opt(opt, "filter_type=")) {
				char *opt_arg = isolate(opt, '=');
				if (!is_mb) goto mb_only;
				if (*opt_arg == '\0') goto needs_arg;
				char *opt_subarg = isolate(opt_arg, ':');
				if (strcmp(opt_arg, "butterworth") == 0)
					config->fb_type = FILTER_BANK_TYPE_BUTTERWORTH;
				else if (strcmp(opt_arg, "chebyshev1") == 0)
					config->fb_type = FILTER_BANK_TYPE_CHEBYSHEV1;
				else if (strcmp(opt_arg, "chebyshev2") == 0)
					config->fb_type = FILTER_BANK_TYPE_CHEBYSHEV2;
				else if (strcmp(opt_arg, "elliptic") == 0)
					config->fb_type = FILTER_BANK_TYPE_ELLIPTIC;
				else {
					LOG_FMT(LL_ERROR, "%s: error: unrecognized filter bank type: %s", argv[0], opt_arg);
					goto fail;
				}
				set_fb_stop_default(config);
				if (*opt_subarg != '\0') {
					char *opt_subarg1 = isolate(opt_subarg, ':');
					switch (config->fb_type) {
					case FILTER_BANK_TYPE_CHEBYSHEV1:
					case FILTER_BANK_TYPE_CHEBYSHEV2:
						config->fb_stop[0] = strtod(opt_subarg, &endptr);
						CHECK_ENDPTR(opt_subarg, endptr, "stop_dB", goto fail);
						if (config->fb_stop[0] < 10.0) {
							LOG_FMT(LL_ERROR, "%s: error: %s: stopband attenuation must be at least 10dB", argv[0], opt_arg);
							goto fail;
						}
						if (*opt_subarg1 != '\0')
							LOG_FMT(LL_ERROR, "%s: warning: %s: ignoring argument: %s", argv[0], opt_arg, opt_subarg1);
						break;
					case FILTER_BANK_TYPE_ELLIPTIC:
						config->fb_stop[0] = strtod(opt_subarg, &endptr);
						CHECK_ENDPTR(opt_subarg, endptr, "stop_dB", goto fail);
						if (*opt_subarg1 != '\0') {
							config->fb_stop[1] = strtod(opt_subarg1, &endptr);
							CHECK_ENDPTR(opt_subarg1, endptr, "stop_dB", goto fail);
						}
						else config->fb_stop[1] = config->fb_stop[0];
						if (config->fb_stop[0] < 20.0 || config->fb_stop[1] < 20.0) {
							LOG_FMT(LL_ERROR, "%s: error: %s: stopband attenuation must be at least 20dB", argv[0], opt_arg);
							goto fail;
						}
						break;
					default:
						LOG_FMT(LL_ERROR, "%s: warning: %s: ignoring argument: %s", argv[0], opt_arg, opt_subarg);
					}
				}
			}
			else {
				LOG_FMT(LL_ERROR, "%s: error: unrecognized option: %s", argv[0], opt);
				goto fail;
				needs_arg:
				LOG_FMT(LL_ERROR, "%s: error: option requires argument: %s", argv[0], opt);
				goto fail;
				mb_only:
				LOG_FMT(LL_ERROR, "%s: warning: ignoring option: %s", argv[0], opt);
			}
			opt = next_opt;
		}
		free(opt_str);
	}
	return 0;

	fail:
	free(opt_str);
	return 1;
}

void smooth_state_init(struct smooth_state *sm, const struct stream_info *istream)
{
	for (int i = 0; i < 4; ++i) ewma_init(&sm->env[i], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&sm->pwr_env[i], istream->fs, EWMA_RISE_TIME(ENV_SMOOTH_TIME));
}

void event_state_init_priv(struct event_state *ev, double fs, double norm_accom_factor)
{
	for (int i = 0; i < 6; ++i) ewma_init(&ev->accom[i], fs, EWMA_RISE_TIME(ACCOM_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->norm[i], fs, EWMA_RISE_TIME(NORM_TIME));
	for (int i = 2; i < 4; ++i) ewma_init(&ev->norm[i], fs, EWMA_RISE_TIME(NORM_TIME*0.625));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->slow[i], fs, EWMA_RISE_TIME(RISE_TIME_SLOW));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->smooth[i], fs, EWMA_RISE_TIME(EVENT_SMOOTH_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&ev->avg[i], fs, EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->drift[i], fs, EWMA_RISE_TIME(ACCOM_TIME*2.0));
	for (int i = 2; i < 4; ++i) ewma_init(&ev->drift[i], fs, EWMA_RISE_TIME(RISE_TIME_FAST));
	ewma_init(&ev->drift_scale[0], fs, EWMA_RISE_TIME(RISE_TIME_FAST));
	ewma_set(&ev->drift_scale[0], 1.0);
	ewma_init(&ev->drift_scale[1], fs, EWMA_RISE_TIME(RISE_TIME_FAST*0.3));
	for (int i = 0; i < 2; ++i) biquad_init_using_type(&ev->drift_notch[i],
		BIQUAD_PEAK, fs, ORD_NOTCH_FREQ, 0.5, ORD_NOTCH_GAIN, 0, BIQUAD_WIDTH_Q);
	ev->t_hold = -2;
	ev->buf_len = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, fs);
	ev->ord_buf = calloc(ev->buf_len, sizeof(struct axes));
	#if ENABLE_LOOKBACK
		ev->diff_buf = calloc(ev->buf_len, sizeof(struct axes));
		ev->slope_buf = calloc(ev->buf_len, sizeof(double [2]));
	#endif
	ev->ds_ord_buf = calloc(ev->buf_len, sizeof(double));
	ev->clip_thresh = EVENT_THRESH * (10.0/MAXIMUM(1.0-NORM_ACCOM_FACTOR, 0.01));
	#if DEBUG_PRINT_MIN_RISE_TIME
		ev->max_diff_scale = ev->max_ord_scale = 1.0;
		ev->fs = fs;
	#endif
}

void event_state_cleanup(struct event_state *ev)
{
	free(ev->ord_buf);
	#if ENABLE_LOOKBACK
		free(ev->diff_buf);
		free(ev->slope_buf);
	#endif
	free(ev->ds_ord_buf);
	#if DEBUG_PRINT_MIN_RISE_TIME
		#define EWMA_CONST_TO_RT(x, fs) (-1.0/log(1.0-(x))/(fs)*1000.0*2.1972)
		#define EWMA_RT_TO_CONST(x, fs) (1.0-exp(-1.0/((fs)*((x)/1000.0/2.1972))))
		LOG_FMT(LL_VERBOSE, "%s(): minimum rise time: ord=%gms; diff=%gms", __func__,
			EWMA_CONST_TO_RT(EWMA_RT_TO_CONST(ACCOM_TIME*2.0, ev->fs)*ev->max_ord_scale, ev->fs),
			EWMA_CONST_TO_RT(EWMA_RT_TO_CONST(RISE_TIME_FAST*2.0, ev->fs)*ev->max_diff_scale, ev->fs));
	#endif
}

void event_config_init_priv(struct event_config *evc, double fs, double diff_overshoot)
{
	evc->sample_frames = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, fs);
	evc->max_hold_frames = TIME_TO_FRAMES(EVENT_MAX_HOLD_TIME, fs);
	evc->min_hold_frames = TIME_TO_FRAMES(EVENT_MIN_HOLD_TIME, fs);
	evc->ord_factor_c = exp(-1.0/(fs*ORD_FACTOR_DECAY));
	evc->diff_lim = M_PI_4*diff_overshoot;
}

void phase_flip_init_params(struct phase_flip_params *pf, double fs)
{
	pf->c[0] = 0.667829372575655;  /* log(1.95) */
	pf->c[1] = log(0.0005*(44100.0/fs));
}

static inline double drift_err_scale(const struct axes *ax0, const struct axes *ax1, double sens_err)
{
	const double lr_err = fabs(ax1->lr - ax0->lr) * M_2_PI;
	const double cs_err = fabs(ax1->cs - ax0->cs) * M_2_PI;
	return 1.0 + (lr_err+cs_err)*sens_err;
}

void process_events_priv(struct event_state *ev, const struct event_config *evc, const struct envs *env, const struct envs *pwr_env, double norm_accom_factor, double thresh_scale, double rear_ev_mask, struct axes *ax, struct axes *ax_ev)
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
	ev->diff_last = diff;
	const struct axes ord_d = ev->ord_buf[ev->buf_p];
	ev->ord_buf[ev->buf_p] = ord;
	#if ENABLE_LOOKBACK
		ev->diff_buf[ev->buf_p] = diff;
	#endif

	ev->adj = 1.0 - ev->ord_factor/20.0;
	ev->adj = (ev->adj > 0.5) ? ev->adj : 0.5;
	ev->ord_factor *= evc->ord_factor_c;

	const double thresh = EVENT_THRESH * thresh_scale;
	const double clip_thresh = ev->clip_thresh * thresh_scale;
	const double l_pwr_xf = pwr_env->l*(1.0-NORM_CROSSFEED) + pwr_env->r*NORM_CROSSFEED;
	const double r_pwr_xf = pwr_env->r*(1.0-NORM_CROSSFEED) + pwr_env->l*NORM_CROSSFEED;
	const double l_norm_div = ewma_run(&ev->norm[0], fabs(l_pwr_xf - ewma_run(&ev->norm[2], l_pwr_xf)*norm_accom_factor*ev->adj));
	const double r_norm_div = ewma_run(&ev->norm[1], fabs(r_pwr_xf - ewma_run(&ev->norm[3], r_pwr_xf)*norm_accom_factor*ev->adj));
	ewma_run_scale_asym(&ev->accom[4], pwr_env->l, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	ewma_run_scale_asym(&ev->accom[5], pwr_env->r, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	const double l_mask = MAXIMUM(pwr_env->l - ewma_get_last(&ev->accom[4]), 0.0);
	const double r_mask = MAXIMUM(pwr_env->r - ewma_get_last(&ev->accom[5]), 0.0);
	const double l_mask_norm = (!NEAR_POS_ZERO(l_norm_div)) ? l_mask / l_norm_div : (NEAR_POS_ZERO(l_mask)) ? 0.0 : clip_thresh;
	const double r_mask_norm = (!NEAR_POS_ZERO(r_norm_div)) ? r_mask / r_norm_div : (NEAR_POS_ZERO(r_mask)) ? 0.0 : clip_thresh;
	const double l_mask_norm_sm = ewma_run(&ev->smooth[0], MINIMUM(l_mask_norm, clip_thresh));
	const double r_mask_norm_sm = ewma_run(&ev->smooth[1], MINIMUM(r_mask_norm, clip_thresh));
	const double l_event = (l_mask_norm_sm - ewma_run(&ev->slow[0], l_mask_norm_sm)) * ev->adj;
	const double r_event = (r_mask_norm_sm - ewma_run(&ev->slow[1], r_mask_norm_sm)) * ev->adj;
	const double l_slope = l_event - ev->last[0];
	const double r_slope = r_event - ev->last[1];
	ev->last[0] = l_event;
	ev->last[1] = r_event;
	ev->slope_last[0] = l_slope;
	ev->slope_last[1] = r_slope;
	#if ENABLE_LOOKBACK
		ev->slope_buf[ev->buf_p][0] = l_slope;
		ev->slope_buf[ev->buf_p][1] = r_slope;
	#endif
	const struct axes ord_d_notched = {
		.lr = biquad(&ev->drift_notch[0], ord_d.lr),
		.cs = biquad(&ev->drift_notch[1], ord_d.cs),
	};

	if (!ev->sample && ((l_slope > 0.0 && l_event > thresh) || (r_slope > 0.0 && r_event > thresh))) {
		ev->sample = 1;
		ev->flags[1] = 0;
		ev->flags[1] |= (l_event >= r_event) ? EVENT_FLAG_L : 0;
		ev->flags[1] |= (r_event >= l_event) ? EVENT_FLAG_R : 0;
		ev->t_sample = ev->t;
		if (ev->t - ev->t_hold > 1) {
			ev->max[1] = 0.0;
			ewma_set(&ev->avg[0], ord.lr);
			ewma_set(&ev->avg[1], ord.cs);
			ewma_set(&ev->avg[2], diff.lr);
			ewma_set(&ev->avg[3], diff.cs);
			#if ENABLE_LOOKBACK
				ssize_t i = CBUF_PREV(ev->buf_p, ev->buf_len), k = ev->buf_p;
				switch (ev->flags[1] & (EVENT_FLAG_L | EVENT_FLAG_R)) {
				case EVENT_FLAG_L:
					while (ev->slope_buf[i][0] > ev->slope_buf[k][0])
					{ --ev->t_sample; k = i; i = CBUF_PREV(i, ev->buf_len); }
					break;
				case EVENT_FLAG_R:
					while (ev->slope_buf[i][1] > ev->slope_buf[k][1])
					{ --ev->t_sample; k = i; i = CBUF_PREV(i, ev->buf_len); }
					break;
				default:
					while (ev->slope_buf[i][0] + ev->slope_buf[i][1] > ev->slope_buf[k][0] + ev->slope_buf[k][1])
					{ --ev->t_sample; k = i; i = CBUF_PREV(i, ev->buf_len); }
				}
				i = k;
				while (i != ev->buf_p) {
					ewma_run(&ev->avg[0], ev->ord_buf[i].lr);
					ewma_run(&ev->avg[1], ev->ord_buf[i].cs);
					ewma_run(&ev->avg[2], ev->diff_buf[i].lr);
					ewma_run(&ev->avg[3], ev->diff_buf[i].cs);
					i = CBUF_NEXT(i, ev->buf_len);
				}
				/* LOG_FMT(LL_VERBOSE, "%s(): lookback: %zd samples", __func__, ev->t - ev->t_sample); */
			#endif
		}
		else {
			ev->t_sample -= evc->sample_frames/2;
			ev->flags[1] |= EVENT_FLAG_FUSE;
		}
	}

	if (ev->sample) {
		ewma_run(&ev->avg[0], ord.lr);
		ewma_run(&ev->avg[1], ord.cs);
		const double diff_lr_avg = ewma_run(&ev->avg[2], diff.lr);
		const double diff_cs_avg = ewma_run(&ev->avg[3], diff.cs);
		if (l_event > ev->max[1]) ev->max[1] = l_event;
		if (r_event > ev->max[1]) ev->max[1] = r_event;
		if (ev->t - ev->t_sample >= evc->sample_frames) {
			ev->sample = 0;
			if (fabs(diff_lr_avg)+fabs(diff_cs_avg) > evc->diff_lim)
				ev->flags[1] |= EVENT_FLAG_USE_ORD;
			if ((ev->flags[1] & EVENT_FLAG_FUSE) && (ev->flags[1] & EVENT_FLAG_USE_ORD) && !(ev->flags[0] & EVENT_FLAG_USE_ORD)) {
				++ev->ignore_count;
				/* LOG_FMT(LL_VERBOSE, "%s(): ignoring event: lr: %+06.2f°; cs: %+06.2f°",
					__func__, TO_DEGREES(diff_lr_avg), TO_DEGREES(diff_cs_avg)); */
			}
			else if (diff_cs_avg < -M_PI_4/12 && ((ev->flags[1] & EVENT_FLAG_L && l_event < thresh*rear_ev_mask)
						|| (ev->flags[1] & EVENT_FLAG_R && r_event < thresh*rear_ev_mask))) {
				++ev->ignore_count;
				/* LOG_FMT(LL_VERBOSE, "%s(): ignoring short-duration rear event: lr: %+06.2f°; cs: %+06.2f°",
					__func__, TO_DEGREES(diff_lr_avg), TO_DEGREES(diff_cs_avg)); */
			}
			else {
				ev->hold = 1;
				ev->t_hold = ev->t;
				ev->dir.lr = diff_lr_avg;
				ev->dir.cs = diff_cs_avg;
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
				ev->max[0] = ev->max[1];
				ev->ds_diff = 1.0 + smoothstep((ev->max[1]-thresh)/(thresh*DIFF_WEIGHT_SCALE))*DIFF_SENS_WEIGHT;
				ewma_set(&ev->drift_scale[1], ev->ds_diff*0.25);
				/* LOG_FMT(LL_VERBOSE, "%s(): event: type: %4s; max: %6.3f; lr: %+06.2f°; cs: %+06.2f°",
					__func__, (ev->flags[1] & EVENT_FLAG_USE_ORD) ? "ord" : "diff", ev->max[1],
					TO_DEGREES(ev->dir.lr), TO_DEGREES(ev->dir.cs)); */
			}
		}
	}
	if (ev->hold) {
		const double ds_diff = ewma_run_scale(&ev->drift_scale[1], ev->ds_diff, ev->ds_diff);
		#if DEBUG_PRINT_MIN_RISE_TIME
			ev->max_diff_scale = MAXIMUM(ev->max_diff_scale, ds_diff);
		#endif
		ax->lr = ax_ev->lr = ewma_run_scale(&ev->drift[2], ev->dir.lr, ds_diff);
		ax->cs = ax_ev->cs = ewma_run_scale(&ev->drift[3], ev->dir.cs, ds_diff);
		if ((ev->flags[0] & EVENT_FLAG_L && l_mask_norm_sm <= EVENT_END_THRESH)
				|| (ev->flags[0] & EVENT_FLAG_R && r_mask_norm_sm <= EVENT_END_THRESH)) {
			ev->flags[0] |= EVENT_FLAG_END;
		}
		if ((ev->t - ev->t_hold >= evc->min_hold_frames && ev->flags[0] & EVENT_FLAG_END)
				|| ev->t - ev->t_hold >= evc->max_hold_frames) {
			if (ev->t - ev->t_hold < evc->max_hold_frames) ++ev->early_count;
			ev->hold = 0;
			ewma_set(&ev->drift[0], ax->lr);
			ewma_set(&ev->drift[1], ax->cs);
			ewma_set(&ev->drift_scale[0], 1.0);
		}
	}
	else {
		const struct axes ax_last = { .lr = ewma_get_last(&ev->drift[0]), .cs = ewma_get_last(&ev->drift[1]) };
		const double ds_ord = ewma_run_set_max(&ev->drift_scale[0],
			drift_err_scale(&ax_last, &ord_d_notched, ORD_SENS_ERR) * ev->ds_ord_buf[ev->buf_p]);
		#if DEBUG_PRINT_MIN_RISE_TIME
			ev->max_ord_scale = MAXIMUM(ev->max_ord_scale, ds_ord);
		#endif
		ax->lr = ewma_run_scale(&ev->drift[0], ord_d_notched.lr, ds_ord);
		ax->cs = ewma_run_scale(&ev->drift[1], ord_d_notched.cs, ds_ord);
		ewma_set(&ev->drift[2], ax->lr);
		ewma_set(&ev->drift[3], ax->cs);
		ax_ev->lr = ax_ev->cs = 0.0;
	}
	const double ds_ord_thresh = thresh*ORD_WEIGHT_THRESH;
	if (l_mask_norm_sm > ds_ord_thresh || r_mask_norm_sm > ds_ord_thresh) {
		const double x = (MAXIMUM(l_mask_norm_sm, r_mask_norm_sm) - ds_ord_thresh) / (thresh*1.5 - ds_ord_thresh);
		ev->ds_ord_buf[ev->buf_p] = smoothstep(x)*ORD_SENS_WEIGHT + 1.0;
	}
	else ev->ds_ord_buf[ev->buf_p] = 1.0;
	++ev->t;
	ev->buf_p = CBUF_NEXT(ev->buf_p, ev->buf_len);
}

static inline double square(double x) { return x*x; }
static inline double pwr_sum(double a, double b) { return sqrt(a*a+b*b); }

/*
 * No steering of rear-encoded signals.
*/
void calc_matrix_coefs_v1(const struct axes *ax, double surr_mult, double surr_mult_rear, int is_mb, struct matrix_coefs *m, double r_shelf_mult[2])
{
	const double lr = ax->lr, cs = ax->cs;
	const double abs_lr = fabs(lr);

	const double gl = 1.0+tan(abs_lr-M_PI_4);
	const double gc_2 = (cs > 0.0) ? 0.5+0.5*tan(cs-M_PI_4) : 0.0;

	m->lsl = 1.0-gc_2;
	m->lsr = -gc_2;
	m->rsl = m->lsr;
	m->rsr = m->lsl;

	if (cs >= 0.0) {
		if (lr > 0.0) {
			m->lsl -= gl*gl;
			m->lsr -= gl;
		}
		else if (lr < 0.0) {
			m->rsl -= gl;
			m->rsr -= gl*gl;
		}
	}
	else {
		const double cs_gl = (cs > -M_PI_4/2) ? 3.0*cs : cs-M_PI_4;
		if (lr > 0.0) {
			m->lsl -= gl*gl*(1.0+sin(cs_gl));
			m->lsr -= gl*cos(cs_gl);
		}
		else if (lr < 0.0) {
			m->rsl -= gl*cos(cs_gl);
			m->rsr -= gl*gl*(1.0+sin(cs_gl));
		}
	}

	/* power correction for uncorrelated input */
	const double pu_sl = pwr_sum(m->lsl, m->lsr);
	m->lsl /= pu_sl;
	m->lsr /= pu_sl;
	const double pu_sr = pwr_sum(m->rsl, m->rsr);
	m->rsl /= pu_sr;
	m->rsr /= pu_sr;

	/* input phasors for given lr, cs */
	const double sin_lr = sin(lr+M_PI_4), cos_lr = cos(lr+M_PI_4);
	double sin_theta, cos_theta;
	if (abs_lr+fabs(cs) < M_PI_4) {
		const double alpha = sqrt(1.0-square(sin(2.0*cs)/cos(2.0*lr)));
		const double beta = sqrt(1.0+alpha), gamma = sqrt(1.0-alpha);
		sin_theta = (cs < 0.0) ? 0.5*(beta+gamma) : 0.5*(beta-gamma);
		cos_theta = (cs < 0.0) ? 0.5*(beta-gamma) : 0.5*(beta+gamma);
	}
	else {
		sin_theta = (cs < 0.0) ? 1.0 : 0.0;
		cos_theta = (cs < 0.0) ? 0.0 : 1.0;
	}
	const double l_real = sin_lr*cos_theta, l_imag = sin_lr*sin_theta;
	const double r_real = cos_lr*cos_theta, r_imag = cos_lr*-sin_theta;

	/* level for directional input */
	const double gd_sl2 = square(m->lsl*l_real + m->lsr*r_real) + square(m->lsl*l_imag + m->lsr*r_imag);
	const double gd_sr2 = square(m->rsl*l_real + m->rsr*r_real) + square(m->rsl*l_imag + m->rsr*r_imag);

	/* power for directional input */
	const double pd_s = gd_sl2 + gd_sr2;

	/* directional power correction and normalization */
	const double surr_mult2 = square(surr_mult);
	const double adj_norm_mult2 = 1.0/(1.0+surr_mult2);
	const double surr_pwr = surr_mult2*adj_norm_mult2;
	const double pdc_f = sqrt(1.0-surr_pwr*MINIMUM(pd_s, 1.0));
	const double pdc_s = sqrt(surr_pwr);

	if (r_shelf_mult) {
		const double surr_mult_hf2 = square(r_shelf_mult[0]);
		const double adj_norm_mult_hf2 = 1.0/(1.0+surr_mult_hf2);
		const double surr_pwr_hf = surr_mult_hf2*adj_norm_mult_hf2;
		r_shelf_mult[0] = sqrt(1.0-surr_pwr_hf*MINIMUM(pd_s, 1.0))/pdc_f;
		r_shelf_mult[1] = sqrt(surr_pwr_hf)/MAXIMUM(pdc_s, DBL_MIN);
	}

	m->ll = pdc_f;
	m->rr = pdc_f;
	m->rl = m->lr = 0.0;
	m->lsl *= pdc_s;
	m->lsr *= pdc_s;
	m->rsl *= pdc_s;
	m->rsr *= pdc_s;
}

/*
 * Note: A factor of 0.75 results in approximately equal power
 * error for both directional and uncorrelated inputs.
*/
#define SURR_PDC_FACTOR_SB 0.5

/*
 * Full steering of rear-encoded sounds and full left/right separation from
 * cs=0° to cs=-22.5°, but only partial steering of left-/right-surround-
 * encoded sounds (lr=±22.5° cs=-22.5°).
*/
void calc_matrix_coefs_v2(const struct axes *ax, double surr_mult, double surr_mult_rear, int is_mb, struct matrix_coefs *m, double r_shelf_mult[2])
{
	const double lr = ax->lr, cs = ax->cs;
	const double abs_lr = fabs(lr), abs_cs = fabs(cs);

	/* initial surround elements */
	m->rsr = m->lsl = 1.0;
	m->rsl = m->lsr = 0.0;
	const double gl = 1.0+tan(abs_lr-M_PI_4);
	if (lr > 0.0) {
		m->lsl -= gl*gl;
		m->lsr -= gl;
	}
	else if (lr < 0.0) {
		m->rsl -= gl;
		m->rsr -= gl*gl;
	}
	if (cs > 0.0) {
		const double gc_2 = 0.5+0.5*tan(abs_cs-M_PI_4);
		m->lsl -= gc_2;
		m->lsr -= gc_2;
		m->rsl -= gc_2;
		m->rsr -= gc_2;
	}
	else if (cs < 0.0) {
		const double cs_gc = (cs > -M_PI_4/2) ? abs_cs : M_PI_4+cs;
		const double gc_2 = 0.5+0.5*tan(cs_gc-M_PI_4);
		m->lsl -= gc_2;
		m->lsr += gc_2;
		m->rsl += gc_2;
		m->rsr -= gc_2;
	}

	/* power correction for uncorrelated input */
	const double pu_sl = pwr_sum(m->lsl, m->lsr);
	m->lsl /= pu_sl;
	m->lsr /= pu_sl;
	const double pu_sr = pwr_sum(m->rsl, m->rsr);
	m->rsl /= pu_sr;
	m->rsr /= pu_sr;

	if (cs >= 0.0) {
		m->ll = 1.0;
		m->lr = 0.0;
	}
	else {
		/* initial front elements */
		const double cf_sm2 = square(MINIMUM(surr_mult_rear, 1.0));
		const double cf = 1.0-sqrt((1.0-cf_sm2)/(1.0+cf_sm2));
		const double front_gc_2 = (0.5+0.5*tan(abs_cs-M_PI_4))*cf;
		m->ll = 1.0 - front_gc_2;
		m->lr = front_gc_2;

		/* power correction for uncorrelated input */
		const double pu_fl = pwr_sum(m->ll, m->lr);
		m->ll /= pu_fl;
		m->lr /= pu_fl;
	}

	/* input phasors for given lr, cs */
	const double sin_lr = sin(lr+M_PI_4), cos_lr = cos(lr+M_PI_4);
	double sin_theta, cos_theta;
	if (abs_lr+abs_cs < M_PI_4) {
		const double alpha = sqrt(1.0-square(sin(2.0*cs)/cos(2.0*lr)));
		const double beta = sqrt(1.0+alpha), gamma = sqrt(1.0-alpha);
		sin_theta = (cs < 0.0) ? 0.5*(beta+gamma) : 0.5*(beta-gamma);
		cos_theta = (cs < 0.0) ? 0.5*(beta-gamma) : 0.5*(beta+gamma);
	}
	else {
		sin_theta = (cs < 0.0) ? 1.0 : 0.0;
		cos_theta = (cs < 0.0) ? 0.0 : 1.0;
	}
	const double l_real = sin_lr*cos_theta, l_imag = sin_lr*sin_theta;
	const double r_real = cos_lr*cos_theta, r_imag = cos_lr*-sin_theta;

	/* level for directional input */
	const double gd_fl2 = square(m->ll*l_real + m->lr*r_real) + square(m->ll*l_imag + m->lr*r_imag);
	const double gd_fr2 = square(m->lr*l_real + m->ll*r_real) + square(m->lr*l_imag + m->ll*r_imag);
	const double gd_sl2 = square(m->lsl*l_real + m->lsr*r_real) + square(m->lsl*l_imag + m->lsr*r_imag);
	const double gd_sr2 = square(m->rsl*l_real + m->rsr*r_real) + square(m->rsl*l_imag + m->rsr*r_imag);

	/* power for directional input */
	const double pd_f = gd_fl2 + gd_fr2;
	const double pd_s = gd_sl2 + gd_sr2;

	/* weighted directional power */
	double pd_f_wf = pd_f, pd_s_wf = pd_s;
	double pd_f_ws = 1.0, pd_s_ws = 1.0;
	if (cs < 0.0) {
		if (abs_cs < abs_lr) {
			const double lr2 = square(lr), cs2 = square(cs);
			const double wf = (lr2+cs2 > DBL_MIN) ? square((lr2-cs2)/(lr2+cs2)) : 0.0;
			pd_f_wf = (pd_f-1.0)*wf+1.0;
			pd_s_wf = (pd_s-1.0)*wf+1.0;
			pd_f_ws = (pd_f-1.0)*(1.0-wf)+1.0;
			pd_s_ws = (pd_s-1.0)*(1.0-wf)+1.0;
		}
		else {
			pd_s_wf = pd_f_wf = 1.0;
			pd_f_ws = pd_f;
			pd_s_ws = pd_s;
		}
	}
	if (!is_mb) pd_s_ws = (pd_s_ws-1.0)*SURR_PDC_FACTOR_SB+1.0;

	/* directional power correction and normalization */
	const double surr_mult2 = square(surr_mult);
	const double adj_norm_mult2 = 1.0/(1.0+surr_mult2);
	const double pdc_fi2 = (1.0-surr_mult2*adj_norm_mult2*pd_s_wf)/pd_f_wf;
	const double pdc_si2 = (1.0-adj_norm_mult2*pd_f_ws)/pd_s_ws;
	const double pdc_all2 = (is_mb) ? 1.0/(pd_f*pdc_fi2 + pd_s*pdc_si2) : 1.0;
	const double pdc_f = sqrt(pdc_fi2*pdc_all2);
	const double pdc_s = sqrt(MAXIMUM(pdc_si2, 0.0)*pdc_all2);

	if (r_shelf_mult) {
		const double surr_mult_hf2 = square(r_shelf_mult[0]);
		const double adj_norm_mult_hf2 = 1.0/(1.0+surr_mult_hf2);
		const double pdc_fi_hf2 = (1.0-surr_mult_hf2*adj_norm_mult_hf2*pd_s_wf)/pd_f_wf;
		const double pdc_si_hf2 = (1.0-adj_norm_mult_hf2*pd_f_ws)/pd_s_ws;
		const double pdc_all_hf2 = (is_mb) ? 1.0/(pd_f*pdc_fi_hf2 + pd_s*pdc_si_hf2) : 1.0;
		r_shelf_mult[0] = sqrt(pdc_fi_hf2*pdc_all_hf2)/pdc_f;
		r_shelf_mult[1] = sqrt(MAXIMUM(pdc_si_hf2, 0.0)*pdc_all_hf2)/MAXIMUM(pdc_s, DBL_MIN);
	}

	m->ll *= pdc_f;
	m->lr *= pdc_f;
	m->rr = m->ll;
	m->rl = m->lr;
	m->lsl *= pdc_s;
	m->lsr *= pdc_s;
	m->rsl *= pdc_s;
	m->rsr *= pdc_s;
}

/*
 * Like v2, but with full steering of left-/right-surround-encoded sounds.
*/
void calc_matrix_coefs_v3(const struct axes *ax, double surr_mult, double surr_mult_rear, int is_mb, struct matrix_coefs *m, double r_shelf_mult[2])
{
	const double lr = ax->lr, cs = ax->cs;
	const double abs_lr = fabs(lr), abs_cs = fabs(cs);

	/* initial surround elements */
	m->rsr = m->lsl = 1.0;
	m->rsl = m->lsr = 0.0;
	const double gl = 1.0+tan(abs_lr-M_PI_4);
	if (lr > 0.0) {
		m->lsl -= gl*gl;
		m->lsr -= gl;
	}
	else if (lr < 0.0) {
		m->rsl -= gl;
		m->rsr -= gl*gl;
	}
	if (cs > 0.0) {
		const double gc_2 = 0.5+0.5*tan(abs_cs-M_PI_4);
		m->lsl -= gc_2;
		m->lsr -= gc_2;
		m->rsl -= gc_2;
		m->rsr -= gc_2;
	}
	else if (cs < 0.0) {
		const double cs_gc = (cs > -M_PI_4/2) ? abs_cs : M_PI_4+cs;
		const double gc_2 = 0.5+0.5*tan(cs_gc-M_PI_4);
		m->lsl -= gc_2;
		m->lsr += gc_2;
		m->rsl += gc_2;
		m->rsr -= gc_2;
	}

	/* power correction for uncorrelated input */
	const double pu_sl = pwr_sum(m->lsl, m->lsr);
	m->lsl /= pu_sl;
	m->lsr /= pu_sl;
	const double pu_sr = pwr_sum(m->rsl, m->rsr);
	m->rsl /= pu_sr;
	m->rsr /= pu_sr;

	if (cs >= 0.0) {
		m->rr = m->ll = 1.0;
		m->rl = m->lr = 0.0;
	}
	else {
		/* initial front elements */
		const double front_gc_2 = 0.5+0.5*tan(abs_cs-M_PI_4);
		const double front_cs = (cs > -M_PI_4/2) ? 4.0*abs_cs : M_PI_2;
		const double front_lr_mult = (abs_lr <= M_PI_4/2) ? 1.0 : 1.0+cos(4.0*abs_lr);
		m->rr = m->ll = -front_gc_2;
		m->rl = m->lr = front_gc_2;
		if (lr > 0.0) {
			m->ll -= gl*gl * sin(front_cs) * front_lr_mult;
			m->lr += gl * (1.0-cos(front_cs)) * front_lr_mult;
		}
		else if (lr < 0.0) {
			m->rl += gl * (1.0-cos(front_cs)) * front_lr_mult;
			m->rr -= gl*gl * sin(front_cs) * front_lr_mult;
		}
		const double cf_sm2 = square(MINIMUM(surr_mult_rear, 1.0));
		const double cf = 1.0-sqrt((1.0-cf_sm2)/(1.0+cf_sm2));
		m->ll = 1.0 + m->ll*cf;
		m->lr = m->lr*cf;
		m->rl = m->rl*cf;
		m->rr = 1.0 + m->rr*cf;

		/* power correction for uncorrelated input */
		const double pu_fl = pwr_sum(m->ll, m->lr);
		m->ll /= pu_fl;
		m->lr /= pu_fl;
		const double pu_fr = pwr_sum(m->rl, m->rr);
		m->rl /= pu_fr;
		m->rr /= pu_fr;
	}

	/* input phasors for given lr, cs */
	const double sin_lr = sin(lr+M_PI_4), cos_lr = cos(lr+M_PI_4);
#if 0
	/* straightforward calculation */
	const double phase = (abs_lr+abs_cs>=M_PI_4)?(cs<0.0)?-M_PI_4:M_PI_4:0.5*asin(sin(2.0*cs)/cos(2.0*lr));
	const double l_real = sin_lr*cos(M_PI_4-phase), l_imag = sin_lr*sin(M_PI_4-phase);
	const double r_real = cos_lr*cos(phase-M_PI_4), r_imag = cos_lr*sin(phase-M_PI_4);
#else
	/* faster calculation */
	double sin_theta, cos_theta;
	if (abs_lr+abs_cs < M_PI_4) {
		const double alpha = sqrt(1.0-square(sin(2.0*cs)/cos(2.0*lr)));
		const double beta = sqrt(1.0+alpha), gamma = sqrt(1.0-alpha);
		sin_theta = (cs < 0.0) ? 0.5*(beta+gamma) : 0.5*(beta-gamma);
		cos_theta = (cs < 0.0) ? 0.5*(beta-gamma) : 0.5*(beta+gamma);
	}
	else {
		sin_theta = (cs < 0.0) ? 1.0 : 0.0;
		cos_theta = (cs < 0.0) ? 0.0 : 1.0;
	}
	const double l_real = sin_lr*cos_theta, l_imag = sin_lr*sin_theta;
	const double r_real = cos_lr*cos_theta, r_imag = cos_lr*-sin_theta;
#endif

	/* level for directional input */
	const double gd_fl2 = square(m->ll*l_real + m->lr*r_real) + square(m->ll*l_imag + m->lr*r_imag);
	const double gd_fr2 = square(m->rl*l_real + m->rr*r_real) + square(m->rl*l_imag + m->rr*r_imag);
	const double gd_sl2 = square(m->lsl*l_real + m->lsr*r_real) + square(m->lsl*l_imag + m->lsr*r_imag);
	const double gd_sr2 = square(m->rsl*l_real + m->rsr*r_real) + square(m->rsl*l_imag + m->rsr*r_imag);

	/* power for directional input */
	const double pd_f = gd_fl2 + gd_fr2;
	const double pd_s = gd_sl2 + gd_sr2;

	/* weighted directional power */
	double pd_f_wf = pd_f, pd_s_wf = pd_s;
	double pd_f_ws = 1.0, pd_s_ws = 1.0;
	if (cs < 0.0) {
		if (abs_cs < abs_lr) {
			const double lr2 = square(lr), cs2 = square(cs);
			const double wf = (lr2+cs2 > DBL_MIN) ? square((lr2-cs2)/(lr2+cs2)) : 0.0;
			pd_f_wf = (pd_f-1.0)*wf+1.0;
			pd_s_wf = (pd_s-1.0)*wf+1.0;
			pd_f_ws = (pd_f-1.0)*(1.0-wf)+1.0;
			pd_s_ws = (pd_s-1.0)*(1.0-wf)+1.0;
		}
		else {
			pd_s_wf = pd_f_wf = 1.0;
			pd_f_ws = pd_f;
			pd_s_ws = pd_s;
		}
	}
	if (!is_mb) pd_s_ws = (pd_s_ws-1.0)*SURR_PDC_FACTOR_SB+1.0;

	/* directional power correction and normalization */
	const double surr_mult2 = square(surr_mult);
	const double adj_norm_mult2 = 1.0/(1.0+surr_mult2);
	const double pdc_fi2 = (1.0-surr_mult2*adj_norm_mult2*pd_s_wf)/pd_f_wf;
	const double pdc_si2 = (1.0-adj_norm_mult2*pd_f_ws)/pd_s_ws;
	const double pdc_all2 = (is_mb) ? 1.0/(pd_f*pdc_fi2 + pd_s*pdc_si2) : 1.0;
	const double pdc_f = sqrt(pdc_fi2*pdc_all2);
	const double pdc_s = sqrt(MAXIMUM(pdc_si2, 0.0)*pdc_all2);

	if (r_shelf_mult) {
		const double surr_mult_hf2 = square(r_shelf_mult[0]);
		const double adj_norm_mult_hf2 = 1.0/(1.0+surr_mult_hf2);
		const double pdc_fi_hf2 = (1.0-surr_mult_hf2*adj_norm_mult_hf2*pd_s_wf)/pd_f_wf;
		const double pdc_si_hf2 = (1.0-adj_norm_mult_hf2*pd_f_ws)/pd_s_ws;
		const double pdc_all_hf2 = (is_mb) ? 1.0/(pd_f*pdc_fi_hf2 + pd_s*pdc_si_hf2) : 1.0;
		r_shelf_mult[0] = sqrt(pdc_fi_hf2*pdc_all_hf2)/pdc_f;
		r_shelf_mult[1] = sqrt(MAXIMUM(pdc_si_hf2, 0.0)*pdc_all_hf2)/MAXIMUM(pdc_s, DBL_MIN);
	}

	m->ll *= pdc_f;
	m->lr *= pdc_f;
	m->rl *= pdc_f;
	m->rr *= pdc_f;
	m->lsl *= pdc_s;
	m->lsr *= pdc_s;
	m->rsl *= pdc_s;
	m->rsr *= pdc_s;
}

#ifndef LADSPA_FRONTEND
void draw_steering_bar(double a, int is_event, struct steering_bar *bar)
{
	memset(bar->s, ' ', 31);
	int i = lrint(a*(-15/M_PI_4))+15;
	if (i > 30) i = 30;
	else if (i < 0) i = 0;
	#if 0
		snprintf(&bar->s[32-6], 6, "%4ld%%", lrint(a/M_PI_4*100.0));
	#else
		bar->s[31] = '\0';
	#endif
	char cursor_c = '*', fill_c = '-';
	if (is_event) {
		cursor_c = '#';
		fill_c = '=';
		bar->e = i+1;
	}
	if (bar->e) bar->s[bar->e-1] = '\'';
	bar->s[i] = cursor_c;
	if (i > 15) memset(&bar->s[15], fill_c, i-15);
	else if (i < 15) memset(&bar->s[i+1], fill_c, 15-i);
}
#endif

struct matrix4_delay_state {
	sample_t *buf;
	ssize_t len, p, drain_frames;
	int n_ch;
	char buf_full, is_draining;
};

sample_t * matrix4_delay_surr_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	sample_t *ibuf_p = &ibuf[e->istream.channels-2];
	if (!state->buf_full && state->p + *frames >= state->len)
		state->buf_full = 1;
	for (ssize_t i = *frames; i > 0; --i) {
		sample_t *b = &state->buf[state->p*2];
		const sample_t s0 = ibuf_p[0], s1 = ibuf_p[1];
		ibuf_p[0] = b[0];
		ibuf_p[1] = b[1];
		b[0] = s0;
		b[1] = s1;
		ibuf_p += e->istream.channels;
		state->p = CBUF_NEXT(state->p, state->len);
	}
	return ibuf;
}

sample_t * matrix4_delay_front_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	sample_t *ibuf_p = ibuf;
	if (!state->buf_full && state->p + *frames >= state->len)
		state->buf_full = 1;
	for (ssize_t i = *frames; i > 0; --i) {
		sample_t *b = &state->buf[state->p*state->n_ch];
		for (int k = 0; k < state->n_ch; ++k) {
			const sample_t s = ibuf_p[k];
			ibuf_p[k] = b[k];
			b[k] = s;
		}
		ibuf_p += e->istream.channels;
		state->p = CBUF_NEXT(state->p, state->len);
	}
	return ibuf;
}

ssize_t matrix4_delay_front_effect_delay(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	return (state->buf_full) ? state->len : state->p;
}

void matrix4_delay_effect_reset(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	state->p = 0;
	state->buf_full = 0;
	memset(state->buf, 0, state->len * state->n_ch * sizeof(sample_t));
}

void matrix4_delay_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	if (!state->buf_full && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->len;
			state->is_draining = 1;
		}
		if (state->drain_frames > 0) {
			*frames = MINIMUM(*frames, state->drain_frames);
			state->drain_frames -= *frames;
			memset(obuf, 0, *frames * e->istream.channels * sizeof(sample_t));
			e->run(e, frames, obuf, NULL);
		}
		else *frames = -1;
	}
}

void matrix4_delay_effect_destroy(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	free(state->buf);
	free(state);
}

struct effect * matrix4_delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, ssize_t frames)
{
	if (frames == 0)
		return NULL;

	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	if (frames > 0)
		e->run = matrix4_delay_surr_effect_run;
	else {
		e->run = matrix4_delay_front_effect_run;
		e->delay = matrix4_delay_front_effect_delay;
	}
	e->reset = matrix4_delay_effect_reset;
	e->drain = matrix4_delay_effect_drain;
	e->destroy = matrix4_delay_effect_destroy;

	struct matrix4_delay_state *state = calloc(1, sizeof(struct matrix4_delay_state));
	state->len = (frames < 0) ? -frames : frames;
	state->n_ch = (frames > 0) ? 2 : e->istream.channels - 2;
	state->buf = calloc(state->len * state->n_ch, sizeof(sample_t));

	LOG_FMT(LL_VERBOSE, "%s: info: net surround delay is %gms (%zd sample%s)",
		ei->name, frames*1000.0/istream->fs, frames, (state->len == 1) ? "" : "s");

	e->data = state;
	return e;
}
