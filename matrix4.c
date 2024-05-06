#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "matrix4.h"
#include "biquad.h"
#include "util.h"

#define ADAPT_TIME          300.0
#define RISE_TIME_FAST       33.0
#define RISE_TIME_SLOW      100.0
#define NORM_TIME           160.0
#define EVENT_THRESH          1.5
#define EVENT_END_THRESH      0.2
#define NORM_MAX              3.2
#define NORM_CROSSFEED        0.5
#define EVENT_SAMPLE_TIME    16.5
#define EVENT_MAX_HOLD_TIME 200.0
#define EVENT_MIN_HOLD_TIME  50.0

struct ewma_state {
	double c0, c1, m0;
};

enum event_state {
	EVENT_STATE_NONE = 0,
	EVENT_STATE_SAMPLE,
	EVENT_STATE_HOLD,
};

enum {
	EVENT_FLAG_L = 1<<0,
	EVENT_FLAG_R = 1<<1,
	EVENT_FLAG_USE_ORD = 1<<2,
	EVENT_FLAG_END = 1<<4,
};

struct matrix4_state {
	int c0, c1, has_output, is_draining, disable, show_status, do_dir_boost;
	enum event_state ev;
	int ev_flags;
	sample_t norm_mult, surr_mult;
	struct biquad_state in_hp[4], in_lp[4];
	struct ewma_state r_fast[4], r_slow[2], adapt[4], norm[4], smooth[2], avg[2], drift[4];
	sample_t **bufs;
	ssize_t len, p, drain_frames;
	ssize_t t, ev_sample_frames, ev_max_hold_frames, ev_min_hold_frames;
	ssize_t ord_count, diff_count, early_count;
};

static void ewma_init(struct ewma_state *state, double fs, double f0)
{
	const double w0 = 2.0*M_PI*f0/fs;
	const double y = 1.0-cos(w0);
	const double a = -y+sqrt(y*y+2.0*y);
	state->c0 = a;
	state->c1 = 1.0-a;
	state->m0 = 0.0;
}

static double ewma_run(struct ewma_state *state, double s)
{
	const double r = state->c0*s + state->c1*state->m0;
	state->m0 = r;
	return r;
}

static double ewma_run_set_max(struct ewma_state *state, double s)
{
	if (s >= state->m0) s = ewma_run(state, s);
	else state->m0 = s;
	return s;
}

static double ewma_set(struct ewma_state *state, double s)
{
	state->m0 = s;
	return s;
}

static double ewma_get_last(struct ewma_state *state)
{
	return state->m0;
}

#define TO_DEGREES(x) ((x)*M_1_PI*180.0)
#define TIME_TO_FREQ(x) (350.0/(x))  /* f0 ~= 0.35/tr */
#define TIME_TO_FRAMES(x, fs) ((x)/1000.0 * (fs))
#define ANGLE(n, d, expr) ((!isnormal(n) && !isnormal(d)) ? M_PI_4 : (!isnormal(d)) ? M_PI_2 : atan(expr))
#define CALC_LR(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)
#define CALC_CS(n, d, expr) (ANGLE(n, d, expr) - M_PI_4)

sample_t * matrix4_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, oframes = 0;
	double lr = 0, cs = 0, lr_adapt = 0, cs_adapt = 0, fl_boost = 0, fr_boost = 0;
	struct matrix4_state *state = (struct matrix4_state *) e->data;

	for (i = 0; i < *frames; ++i) {
		const double s0 = (ibuf) ? ibuf[i*e->istream.channels + state->c0] : 0.0;
		const double s1 = (ibuf) ? ibuf[i*e->istream.channels + state->c1] : 0.0;

		const double s0_d = state->bufs[state->c0][state->p];
		const double s1_d = state->bufs[state->c1][state->p];

		const double s0_bp = biquad(&state->in_lp[0], biquad(&state->in_hp[0], s0));
		const double s1_bp = biquad(&state->in_lp[1], biquad(&state->in_hp[1], s1));
		const double s0_d_bp = biquad(&state->in_lp[2], biquad(&state->in_hp[2], s0_d));
		const double s1_d_bp = biquad(&state->in_lp[3], biquad(&state->in_hp[3], s1_d));

		const double lr_ord = CALC_LR(s0_d_bp, s1_d_bp, fabs(s0_d_bp)/fabs(s1_d_bp));
		const double cs_ord = CALC_CS(s0_d_bp+s1_d_bp, s0_d_bp-s1_d_bp, fabs(s0_d_bp+s1_d_bp)/fabs(s0_d_bp-s1_d_bp));

		const double l_pwr = s0_bp*s0_bp;
		const double r_pwr = s1_bp*s1_bp;
		const double sum_pwr = (s0_bp+s1_bp)*(s0_bp+s1_bp);
		const double diff_pwr = (s0_bp-s1_bp)*(s0_bp-s1_bp);

		const double l_pwr_env = ewma_run(&state->r_fast[0], l_pwr);
		const double r_pwr_env = ewma_run(&state->r_fast[1], r_pwr);
		const double sum_pwr_env = ewma_run(&state->r_fast[2], sum_pwr);
		const double diff_pwr_env = ewma_run(&state->r_fast[3], diff_pwr);

		const double l_adapt = l_pwr_env - ewma_run_set_max(&state->adapt[0], l_pwr_env);
		const double r_adapt = r_pwr_env - ewma_run_set_max(&state->adapt[1], r_pwr_env);
		const double sum_adapt = sum_pwr_env - ewma_run_set_max(&state->adapt[2], sum_pwr_env);
		const double diff_adapt = diff_pwr_env - ewma_run_set_max(&state->adapt[3], diff_pwr_env);

		lr_adapt = CALC_LR(l_adapt, r_adapt, sqrt(l_adapt/r_adapt));
		cs_adapt = CALC_CS(sum_adapt, diff_adapt, sqrt(sum_adapt/diff_adapt));

		ewma_run(&state->norm[0], l_adapt);
		ewma_run(&state->norm[1], r_adapt);
		ewma_run(&state->norm[2], l_pwr_env*(1.0/(NORM_MAX)));
		ewma_run(&state->norm[3], r_pwr_env*(1.0/(NORM_MAX)));
		const double l_norm_div_adapt = sqrt(pow(ewma_get_last(&state->norm[0]), 2.0) + pow(ewma_get_last(&state->norm[1])*(NORM_CROSSFEED), 2.0));
		const double r_norm_div_adapt = sqrt(pow(ewma_get_last(&state->norm[1]), 2.0) + pow(ewma_get_last(&state->norm[0])*(NORM_CROSSFEED), 2.0));
		const double l_norm_div_env = sqrt(pow(ewma_get_last(&state->norm[2]), 2.0) + pow(ewma_get_last(&state->norm[3])*(NORM_CROSSFEED), 2.0));
		const double r_norm_div_env = sqrt(pow(ewma_get_last(&state->norm[3]), 2.0) + pow(ewma_get_last(&state->norm[2])*(NORM_CROSSFEED), 2.0));
		const double l_norm_div = MAXIMUM(l_norm_div_adapt, l_norm_div_env);
		const double r_norm_div = MAXIMUM(r_norm_div_adapt, r_norm_div_env);
		const double l_adapt_norm = (l_norm_div > 0.0) ? l_adapt / l_norm_div : (l_adapt <= 0.0) ? 0.0 : 100.0;
		const double r_adapt_norm = (r_norm_div > 0.0) ? r_adapt / r_norm_div : (r_adapt <= 0.0) ? 0.0 : 100.0;
		const double l_adapt_norm_sm = ewma_run(&state->smooth[0], l_adapt_norm);
		const double r_adapt_norm_sm = ewma_run(&state->smooth[1], r_adapt_norm);

		const double l_event = l_adapt_norm_sm - ewma_run(&state->r_slow[0], l_adapt_norm_sm);
		const double r_event = r_adapt_norm_sm - ewma_run(&state->r_slow[1], r_adapt_norm_sm);

		if (state->ev == EVENT_STATE_NONE
				&& (l_event > EVENT_THRESH || r_event > EVENT_THRESH)) {
			state->ev = EVENT_STATE_SAMPLE;
			state->ev_flags = 0;
			state->ev_flags |= (l_event >= r_event) ? EVENT_FLAG_L : 0;
			state->ev_flags |= (r_event >= l_event) ? EVENT_FLAG_R : 0;
			state->t = 0;
			ewma_set(&state->avg[0], lr_adapt);
			ewma_set(&state->avg[1], cs_adapt);
		}

		switch (state->ev) {
		case EVENT_STATE_SAMPLE:
			ewma_run(&state->avg[0], lr_adapt);
			ewma_run(&state->avg[1], cs_adapt);
			lr = ewma_run(&state->drift[0], lr_ord);
			cs = ewma_run(&state->drift[1], cs_ord);
			if (state->t >= state->ev_sample_frames) {
				state->ev = EVENT_STATE_HOLD;
				state->t = 0;
				ewma_set(&state->drift[2], lr);
				ewma_set(&state->drift[3], cs);
				const double abs_lr_ev = fabs(ewma_get_last(&state->avg[0]));
				const double abs_cs_ev = fabs(ewma_get_last(&state->avg[1]));
				if (abs_lr_ev+abs_cs_ev > M_PI_4*1.01) state->ev_flags |= EVENT_FLAG_USE_ORD;
				if (state->ev_flags & EVENT_FLAG_USE_ORD) ++state->ord_count;
				else ++state->diff_count;
				/* LOG_FMT(LL_VERBOSE, "%s: event: type: %4s; lr: %+06.2f°; cs: %+06.2f°",
						e->name, (state->ev_flags & EVENT_FLAG_USE_ORD) ? "ord" : "diff",
						TO_DEGREES(ewma_get_last(&state->avg[0])), TO_DEGREES(ewma_get_last(&state->avg[1]))); */
			}
			else ++state->t;
			break;
		case EVENT_STATE_HOLD:
			if (state->ev_flags & EVENT_FLAG_USE_ORD) {
				lr_adapt = lr = ewma_run(&state->drift[0], lr_ord);
				cs_adapt = cs = ewma_run(&state->drift[1], cs_ord);
			}
			else {
				lr_adapt = lr = ewma_set(&state->drift[0], ewma_run(&state->drift[2], ewma_get_last(&state->avg[0])));
				cs_adapt = cs = ewma_set(&state->drift[1], ewma_run(&state->drift[3], ewma_get_last(&state->avg[1])));
			}
			if ((state->ev_flags & EVENT_FLAG_L && l_adapt_norm_sm <= EVENT_END_THRESH)
					|| (state->ev_flags & EVENT_FLAG_R && r_adapt_norm_sm <= EVENT_END_THRESH)) {
				state->ev_flags |= EVENT_FLAG_END;
			}
			if ((state->t >= state->ev_min_hold_frames && state->ev_flags & EVENT_FLAG_END)
					|| state->t >= state->ev_max_hold_frames) {
				if (state->t < state->ev_max_hold_frames) ++state->early_count;
				state->ev = EVENT_STATE_NONE;
				state->t = 0;
			}
			else ++state->t;
			break;
		default:
			lr = ewma_run(&state->drift[0], lr_ord);
			cs = ewma_run(&state->drift[1], cs_ord);
			lr_adapt = cs_adapt = 0;
			++state->t;
		}

		double abs_lr = fabs(lr);
		double abs_cs = fabs(cs);
		if (abs_lr+abs_cs > M_PI_4) {
			const double norm = M_PI_4 / (abs_lr+abs_cs);
			lr *= norm;
			cs *= norm;
			abs_lr *= norm;
			abs_cs *= norm;
		}

		const double norm_mult = (state->disable) ? 1.0 : state->norm_mult;
		const double surr_mult = (state->disable) ? 0.0 : state->surr_mult;
		sample_t ll_m, lr_m, rl_m, rr_m;
		sample_t lsl_m, lsr_m, rsl_m, rsr_m;

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
				lsl_m = 1.0-gsl-0.5*gc;
				lsr_m = -0.5*gc-gl;
				rsl_m = -0.5*gc;
				rsr_m = 1.0-0.5*gc;
			}
			else {
				lsl_m = 1.0-0.5*gc;
				lsr_m = -0.5*gc;
				rsl_m = -0.5*gc-gl;
				rsr_m = 1.0-gsl-0.5*gc;
			}
		}
		else {
			if (lr >= 0.0) {
				if (cs > -M_PI_4/2) {
					lsl_m = 1.0-gsl*(1.0+sin(4.0*cs));
					lsr_m = -gl*cos(4.0*cs);
				}
				else {
					lsl_m = 1.0;
					lsr_m = 0.0;
				}
				rsl_m = 0.0;
				rsr_m = 1.0;
			}
			else {
				lsl_m = 1.0;
				lsr_m = 0.0;
				if (cs > -M_PI_4/2) {
					rsl_m = -gl*cos(4.0*cs);
					rsr_m = 1.0-gsl*(1.0+sin(4.0*cs));
				}
				else {
					rsl_m = 0.0;
					rsr_m = 1.0;
				}
			}
		}

		/* Power correction and scaling */
		const sample_t ls_m_scale = norm_mult*surr_mult/sqrt(lsl_m*lsl_m + lsr_m*lsr_m);
		const sample_t rs_m_scale = norm_mult*surr_mult/sqrt(rsl_m*rsl_m + rsr_m*rsr_m);
		lsl_m *= ls_m_scale;
		lsr_m *= ls_m_scale;
		rsl_m *= rs_m_scale;
		rsr_m *= rs_m_scale;

		if (state->do_dir_boost) {
			if (cs >= 0.0) {
				const double b_gl = 1.0+tan(abs_lr+cs-M_PI_4);
				fl_boost = (1.0-norm_mult)*b_gl*b_gl;
				fr_boost = fl_boost;
			}
			else if (cs >= -M_PI_4/2) {
				fl_boost = (1.0-norm_mult)*gsl*cos(4.0*cs);
				fr_boost = fl_boost;
			}
			else {
				fl_boost = 0.0;
				fr_boost = 0.0;
			}
		}
		else {
			fl_boost = 0.0;
			fr_boost = 0.0;
		}
		ll_m = norm_mult + fl_boost;
		lr_m = 0.0;
		rl_m = 0.0;
		rr_m = norm_mult + fr_boost;

		const sample_t out_l = s0_d*ll_m + s1_d*lr_m;
		const sample_t out_r = s0_d*rl_m + s1_d*rr_m;
		const sample_t out_ls = s0_d*lsl_m + s1_d*lsr_m;
		const sample_t out_rs = s0_d*rsl_m + s1_d*rsr_m;

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
		if (state->show_status)
			fprintf(stderr, "\n%s%s: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); dir_boost: l:%05.3f r:%05.3f; ord: %zd; diff: %zd; early: %zd\033[K\r\033[A",
				e->name, (state->disable) ? " [off]" : "", TO_DEGREES(lr), TO_DEGREES(lr_adapt), TO_DEGREES(cs), TO_DEGREES(cs_adapt),
				fl_boost, fr_boost, state->ord_count, state->diff_count, state->early_count);
	#endif

	*frames = oframes;
	return obuf;
}

ssize_t matrix4_effect_delay(struct effect *e)
{
	struct matrix4_state *state = (struct matrix4_state *) e->data;
	return (state->has_output) ? state->len : state->p;
}

void matrix4_effect_reset(struct effect *e)
{
	int i;
	struct matrix4_state *state = (struct matrix4_state *) e->data;
	state->p = 0;
	state->has_output = 0;
	for (i = 0; i < e->istream.channels; ++i)
		memset(state->bufs[i], 0, state->len * sizeof(sample_t));
}

void matrix4_effect_signal(struct effect *e)
{
	struct matrix4_state *state = (struct matrix4_state *) e->data;
	state->disable = !state->disable;
	LOG_FMT(LL_NORMAL, "%s: %s", e->name, (state->disable) ? "disabled" : "enabled");
}

void matrix4_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct matrix4_state *state = (struct matrix4_state *) e->data;
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

void matrix4_effect_destroy(struct effect *e)
{
	int i;
	struct matrix4_state *state = (struct matrix4_state *) e->data;
	for (i = 0; i < e->istream.channels; ++i)
		free(state->bufs[i]);
	free(state->bufs);
	#ifndef LADSPA_FRONTEND
		if (state->show_status) fprintf(stderr, "\033[K\n\033[K\r\033[A");
	#endif
	free(state);
}

struct effect * matrix4_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct matrix4_state *state;
	int i, n_channels = 0;
	double surr_mult = 0.5;
	char *opt = NULL, *next_opt, *endptr;

	if (argc > 3) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (argc > 2)
		opt = argv[1];
	if (argc > 1) {
		surr_mult = pow(10.0, strtod(argv[argc-1], &endptr) / 20.0);
		CHECK_ENDPTR(argv[argc-1], endptr, "surround_level", return NULL);
		if (surr_mult > 1.0)
			LOG_FMT(LL_ERROR, "%s: warning: surround_level probably shouldn't be greater than 0", argv[0]);
	}

	if (istream->fs < 32000) {
		LOG_FMT(LL_ERROR, "%s: error: sample rate out of range", argv[0]);
		return NULL;
	}
	for (i = 0; i < istream->channels; ++i)
		if (GET_BIT(channel_selector, i))
			++n_channels;
	if (n_channels != 2) {
		LOG_FMT(LL_ERROR, "%s: error: number of input channels must be 2", argv[0]);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + 2;
	e->run = matrix4_effect_run;
	e->delay = matrix4_effect_delay;
	e->reset = matrix4_effect_reset;
	e->drain = matrix4_effect_drain;
	e->destroy = matrix4_effect_destroy;
	state = calloc(1, sizeof(struct matrix4_state));

	state->c0 = state->c1 = -1;
	for (i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (state->c0 == -1)
				state->c0 = i;
			else
				state->c1 = i;
		}
	}
	state->do_dir_boost = 1;
	if (opt) {
		while (*opt != '\0') {
			next_opt = isolate(opt, ',');
			if (*opt == '\0') /* do nothing */;
			else if (strcmp(opt, "show_status")  == 0) state->show_status = 1;
			else if (strcmp(opt, "no_dir_boost") == 0) state->do_dir_boost = 0;
			else if (strcmp(opt, "signal")       == 0) e->signal = matrix4_effect_signal;
			else {
				LOG_FMT(LL_ERROR, "%s: error: unrecognized option: %s", argv[0], opt);
				goto opt_fail;
			}
			opt = next_opt;
		}
	}
	for (i = 0; i < 4; ++i) {
		biquad_init_using_type(&state->in_hp[i], BIQUAD_HIGHPASS, istream->fs,  500.0, 0.5, 0, 0, BIQUAD_WIDTH_Q);
		biquad_init_using_type(&state->in_lp[i], BIQUAD_LOWPASS,  istream->fs, 5000.0, 0.5, 0, 0, BIQUAD_WIDTH_Q);
	}
	for (i = 0; i < 4; ++i) ewma_init(&state->r_fast[i], istream->fs, TIME_TO_FREQ(RISE_TIME_FAST));
	for (i = 0; i < 2; ++i) ewma_init(&state->r_slow[i], istream->fs, TIME_TO_FREQ(RISE_TIME_SLOW));
	for (i = 0; i < 4; ++i) ewma_init(&state->adapt[i], istream->fs, TIME_TO_FREQ(ADAPT_TIME));
	for (i = 0; i < 4; ++i) ewma_init(&state->norm[i], istream->fs, TIME_TO_FREQ(NORM_TIME));
	for (i = 0; i < 2; ++i) ewma_init(&state->smooth[i], istream->fs, TIME_TO_FREQ(RISE_TIME_FAST));
	for (i = 0; i < 2; ++i) ewma_init(&state->avg[i], istream->fs, TIME_TO_FREQ(EVENT_SAMPLE_TIME));
	for (i = 0; i < 2; ++i) ewma_init(&state->drift[i], istream->fs, TIME_TO_FREQ(ADAPT_TIME));
	for (i = 2; i < 4; ++i) ewma_init(&state->drift[i], istream->fs, TIME_TO_FREQ(RISE_TIME_FAST));

	state->len = lround(TIME_TO_FRAMES(RISE_TIME_FAST + EVENT_SAMPLE_TIME, istream->fs));
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (i = 0; i < istream->channels; ++i)
		state->bufs[i] = calloc(state->len, sizeof(sample_t));
	state->surr_mult = surr_mult;
	state->norm_mult = 1.0 / sqrt(1.0 + surr_mult*surr_mult);
	state->ev_sample_frames = TIME_TO_FRAMES(EVENT_SAMPLE_TIME, istream->fs);
	state->ev_max_hold_frames = TIME_TO_FRAMES(EVENT_MAX_HOLD_TIME, istream->fs);
	state->ev_min_hold_frames = TIME_TO_FRAMES(EVENT_MIN_HOLD_TIME, istream->fs);
	e->data = state;
	return e;

	opt_fail:
	free(state);
	free(e);
	return NULL;
}
