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
#include <string.h>
#include <math.h>
#define DSP_MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS
#include "matrix4_common.h"
#include "dsp.h"

int get_args_and_channels(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, int argc, const char *const *argv, struct matrix4_config *config)
{
	double surr_level = -6.0206;
	char *endptr;
	if (argc > 3) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return 1;
	}
	config->opt_str_idx = -1;
	if (argc == 2) {
		const double val = strtod(argv[1], &endptr);
		if (endptr == argv[1] || *endptr != '\0')
			config->opt_str_idx = 1;
		else
			surr_level = val;
	}
	else if (argc == 3) {
		config->opt_str_idx = 1;
		surr_level = strtod(argv[2], &endptr);
		CHECK_ENDPTR(argv[2], endptr, "surround_level", return 1);
	}
	config->surr_mult = pow(10.0, surr_level / 20.0);
	if (config->surr_mult > 1.0)
		LOG_FMT(LL_ERROR, "%s: warning: surround_level probably shouldn't be greater than 0dB", argv[0]);

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

int parse_effect_opts(const char *const *argv, const struct stream_info *istream, struct matrix4_config *config)
{
	char *opt_str = NULL;
	config->fb_type = FILTER_BANK_TYPE_DEFAULT;
	config->db_type = DIR_BOOST_TYPE_DEFAULT;
	config->db_band_weight[0] = DIR_BOOST_MIN_BAND_WEIGHT_DEFAULT;
	config->db_band_weight[1] = DIR_BOOST_MAX_BAND_WEIGHT_DEFAULT;
	set_fb_stop_default(config);
	if (config->opt_str_idx > 0) {
		opt_str = strdup(argv[config->opt_str_idx]);
		char *opt = opt_str, *next_opt, *endptr;
		while (*opt != '\0') {
			next_opt = isolate(opt, ',');
			if (*opt == '\0') /* do nothing */;
			else if (is_opt(opt, "show_status")) config->show_status = 1;
			else if (is_opt(opt, "dir_boost=")) {
				char *opt_arg = isolate(opt, '=');
				char *opt_subarg = isolate(opt_arg, ':');
				if (*opt_arg == '\0' || strcmp(opt_arg, "simple") == 0)
					config->db_type = DIR_BOOST_TYPE_SIMPLE;
				else if (strcmp(opt_arg, "band") == 0) {
					config->db_type = DIR_BOOST_TYPE_BAND;
					config->do_phase_lin = 1;
				}
				else if (strcmp(opt_arg, "combined") == 0) {
					config->db_type = DIR_BOOST_TYPE_COMBINED;
					config->do_phase_lin = 1;
					if (*opt_subarg != '\0') {
						char *opt_subarg1 = isolate(opt_subarg, ':');
						config->db_band_weight[0] = strtod(opt_subarg, &endptr);
						CHECK_ENDPTR(opt_subarg, endptr, "min_band_weight", goto fail);
						CHECK_RANGE(config->db_band_weight[0] >= 0.0 && config->db_band_weight[0] <= 1.0,
							"min_band_weight", goto fail);
						if (*opt_subarg1 != '\0') {
							config->db_band_weight[1] = strtod(opt_subarg1, &endptr);
							CHECK_ENDPTR(opt_subarg1, endptr, "max_band_weight", goto fail);
							CHECK_RANGE(config->db_band_weight[1] >= 0.0 && config->db_band_weight[1] <= 1.0,
								"max_band_weight", goto fail);
							if (config->db_band_weight[0] > config->db_band_weight[1])
								LOG_FMT(LL_ERROR, "%s: warning: min_band_weight probably shouldn't be greater than max_band_weight", argv[0]);
						}
						else config->db_band_weight[1] = MAXIMUM(config->db_band_weight[0], DIR_BOOST_MAX_BAND_WEIGHT_DEFAULT);
					}
					else {
						config->db_band_weight[0] = DIR_BOOST_MIN_BAND_WEIGHT_DEFAULT;
						config->db_band_weight[1] = DIR_BOOST_MAX_BAND_WEIGHT_DEFAULT;
					}
				}
				else if (strcmp(opt_arg, "none") == 0)
					config->db_type = DIR_BOOST_TYPE_NONE;
				else {
					LOG_FMT(LL_ERROR, "%s: error: unrecognized directional boost type: %s", argv[0], opt_arg);
					goto fail;
				}
				if (*opt_subarg != '\0' && config->db_type != DIR_BOOST_TYPE_COMBINED)
					LOG_FMT(LL_ERROR, "%s: warning: %s: ignoring argument: %s", argv[0], opt_arg, opt_subarg);
			}
			else if (is_opt(opt, "no_dir_boost")) config->db_type = DIR_BOOST_TYPE_NONE;
			else if (is_opt(opt, "signal"))       config->enable_signal = 1;
			else if (is_opt(opt, "linear_phase")) config->do_phase_lin  = 1;
			else if (is_opt(opt, "surround_delay=")) {
				char *opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') goto needs_arg;
				config->surr_delay_frames = parse_len(opt_arg, istream->fs, &endptr);
				CHECK_ENDPTR(opt_arg, endptr, "surround_delay", goto fail);
			}
			else if (is_opt(opt, "filter_type=")) {
				char *opt_arg = isolate(opt, '=');
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

void event_state_init_priv(struct event_state *ev, double fs, double thresh_scale)
{
	for (int i = 0; i < 6; ++i) ewma_init(&ev->accom[i], fs, EWMA_RISE_TIME(ACCOM_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->norm[i], fs, EWMA_RISE_TIME(NORM_TIME));
	for (int i = 2; i < 4; ++i) ewma_init(&ev->norm[i], fs, EWMA_RISE_TIME(NORM_TIME*0.625));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->slow[i], fs, EWMA_RISE_TIME(RISE_TIME_SLOW));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->smooth[i], fs, EWMA_RISE_TIME(EVENT_SMOOTH_TIME));
	for (int i = 0; i < 4; ++i) ewma_init(&ev->avg[i], fs, EWMA_RISE_TIME(EVENT_SAMPLE_TIME));
	for (int i = 0; i < 2; ++i) ewma_init(&ev->drift[i], fs, EWMA_RISE_TIME(ACCOM_TIME*2.0));
	for (int i = 2; i < 4; ++i) ewma_init(&ev->drift[i], fs, EWMA_RISE_TIME(RISE_TIME_FAST*2.0));
	ev->t_hold = -2;
	ev->buf_len = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, fs);
	ev->ord_buf = calloc(ev->buf_len, sizeof(struct axes));
	#if ENABLE_LOOKBACK
		ev->diff_buf = calloc(ev->buf_len, sizeof(struct axes));
		ev->slope_buf = calloc(ev->buf_len, sizeof(double [2]));
	#endif
	ev->env_buf = calloc(ev->buf_len, sizeof(struct envs));
	ev->adapt_buf = calloc(ev->buf_len, sizeof(struct envs));
	ev->thresh = EVENT_THRESH * thresh_scale;
	ev->end_thresh = EVENT_END_THRESH * thresh_scale;
	ev->clip_thresh = EVENT_THRESH * 10.0 * thresh_scale;
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

void event_config_init_priv(struct event_config *evc, double fs)
{
	evc->sample_frames = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, fs);
	evc->max_hold_frames = TIME_TO_FRAMES(EVENT_MAX_HOLD_TIME, fs);
	evc->min_hold_frames = TIME_TO_FRAMES(EVENT_MIN_HOLD_TIME, fs);
	evc->ord_factor_c = exp(-1.0/(fs*ORD_FACTOR_DECAY));
}

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

void process_events_priv(struct event_state *ev, const struct event_config *evc, const struct envs *env, const struct envs *pwr_env, double norm_accom_factor, struct axes *ax, struct axes *ax_ev)
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
	#if ENABLE_LOOKBACK
		ev->diff_buf[ev->buf_p] = diff;
	#endif
	const struct envs env_d = ev->env_buf[ev->buf_p];
	ev->env_buf[ev->buf_p] = *env;
	const struct envs adapt_d = ev->adapt_buf[ev->buf_p];
	ev->adapt_buf[ev->buf_p] = adapt;

	ev->adj = 1.0 - ev->ord_factor/20.0;
	ev->adj = (ev->adj > 0.5) ? ev->adj : 0.5;
	ev->ord_factor *= evc->ord_factor_c;

	const double l_pwr_xf = pwr_env->l*(1.0-NORM_CROSSFEED) + pwr_env->r*NORM_CROSSFEED;
	const double r_pwr_xf = pwr_env->r*(1.0-NORM_CROSSFEED) + pwr_env->l*NORM_CROSSFEED;
	const double l_norm_div = ewma_run(&ev->norm[0], fabs(l_pwr_xf - ewma_run(&ev->norm[2], l_pwr_xf)*norm_accom_factor*ev->adj));
	const double r_norm_div = ewma_run(&ev->norm[1], fabs(r_pwr_xf - ewma_run(&ev->norm[3], r_pwr_xf)*norm_accom_factor*ev->adj));
	ewma_run_scale_asym(&ev->accom[4], pwr_env->l, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	ewma_run_scale_asym(&ev->accom[5], pwr_env->r, 1.0, ACCOM_TIME/EVENT_MASK_TIME);
	const double l_mask = MAXIMUM(pwr_env->l - ewma_get_last(&ev->accom[4]), 0.0);
	const double r_mask = MAXIMUM(pwr_env->r - ewma_get_last(&ev->accom[5]), 0.0);
	const double l_mask_norm = (!NEAR_POS_ZERO(l_norm_div)) ? l_mask / l_norm_div : (NEAR_POS_ZERO(l_mask)) ? 0.0 : ev->clip_thresh;
	const double r_mask_norm = (!NEAR_POS_ZERO(r_norm_div)) ? r_mask / r_norm_div : (NEAR_POS_ZERO(r_mask)) ? 0.0 : ev->clip_thresh;
	const double l_mask_norm_sm = ewma_run(&ev->smooth[0], MINIMUM(l_mask_norm, ev->clip_thresh));
	const double r_mask_norm_sm = ewma_run(&ev->smooth[1], MINIMUM(r_mask_norm, ev->clip_thresh));
	const double l_event = (l_mask_norm_sm - ewma_run(&ev->slow[0], l_mask_norm_sm)) * ev->adj;
	const double r_event = (r_mask_norm_sm - ewma_run(&ev->slow[1], r_mask_norm_sm)) * ev->adj;
	const double l_slope = l_event - ev->last[0];
	const double r_slope = r_event - ev->last[1];
	ev->last[0] = l_event;
	ev->last[1] = r_event;
	#if ENABLE_LOOKBACK
		ev->slope_buf[ev->buf_p][0] = l_slope;
		ev->slope_buf[ev->buf_p][1] = r_slope;
	#endif

	if (!ev->sample && ((l_slope > 0.0 && l_event > ev->thresh) || (r_slope > 0.0 && r_event > ev->thresh))) {
		ev->sample = 1;
		ev->flags[1] = 0;
		ev->flags[1] |= (l_event >= r_event) ? EVENT_FLAG_L : 0;
		ev->flags[1] |= (r_event >= l_event) ? EVENT_FLAG_R : 0;
		ev->t_sample = ev->t;
		if (ev->t - ev->t_hold > 1) {
			ev->max = 0.0;
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
				i = CBUF_NEXT(i, ev->buf_len);
				ewma_set(&ev->avg[0], ev->ord_buf[i].lr);
				ewma_set(&ev->avg[1], ev->ord_buf[i].cs);
				ewma_set(&ev->avg[2], ev->ord_buf[i].lr);
				ewma_set(&ev->avg[3], ev->ord_buf[i].cs);
				while (i != ev->buf_p) {
					ewma_run(&ev->avg[0], ev->ord_buf[i].lr);
					ewma_run(&ev->avg[1], ev->ord_buf[i].cs);
					ewma_run(&ev->avg[2], ev->diff_buf[i].lr);
					ewma_run(&ev->avg[3], ev->diff_buf[i].cs);
					i = CBUF_NEXT(i, ev->buf_len);
				}
				/* LOG_FMT(LL_VERBOSE, "%s(): lookback: %zd samples", __func__, ev->t - ev->t_sample); */
			#else
				ewma_set(&ev->avg[0], ord.lr);
				ewma_set(&ev->avg[1], ord.cs);
				ewma_set(&ev->avg[2], ord.lr);
				ewma_set(&ev->avg[3], ord.cs);
			#endif
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
				/* LOG_FMT(LL_VERBOSE, "%s(): event: type: %4s; max: %6.3f; lr: %+06.2f°; cs: %+06.2f°",
					__func__, (ev->flags[1] & EVENT_FLAG_USE_ORD) ? "ord" : "diff", ev->max,
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
	ev->buf_p = CBUF_NEXT(ev->buf_p, ev->buf_len);
}

void calc_matrix_coefs(const struct axes *ax, int do_dir_boost, double norm_mult, double surr_mult, struct matrix_coefs *m)
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
