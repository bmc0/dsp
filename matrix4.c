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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "matrix4.h"
#include "biquad.h"
#include "util.h"

#include "matrix4_common.h"

struct matrix4_state {
	int c0, c1;
	char has_output, is_draining, disable, show_status, do_dir_boost;
	sample_t **bufs;
	struct biquad_state in_hp[2], in_lp[2];
	struct smooth_state sm;
	struct event_state ev;
	struct event_config evc;
	struct axes ax, ax_ev;
	sample_t norm_mult, surr_mult;
	struct smf_state dir_boost_smooth;
	ssize_t len, p, drain_frames, fade_frames, fade_p;
};

sample_t * matrix4_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, oframes = 0;
	double dir_boost = 0.0;
	struct matrix4_state *state = (struct matrix4_state *) e->data;

	for (i = 0; i < *frames; ++i) {
		double norm_mult = state->norm_mult, surr_mult = state->surr_mult;
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

		const sample_t s0_bp = biquad(&state->in_lp[0], biquad(&state->in_hp[0], s0));
		const sample_t s1_bp = biquad(&state->in_lp[1], biquad(&state->in_hp[1], s1));

		struct envs env, pwr_env;
		calc_input_envs(&state->sm, s0_bp, s1_bp, &env, &pwr_env);

		process_events(&state->ev, &state->evc, &env, &pwr_env, &state->ax, &state->ax_ev);
		norm_axes(&state->ax);

		struct matrix_coefs m = {0};
		calc_matrix_coefs(&state->ax, state->do_dir_boost, norm_mult, surr_mult, &m);
		dir_boost = smf_asym_run(&state->dir_boost_smooth, m.dir_boost);

		const double ll_m = norm_mult + dir_boost;
		const double rr_m = norm_mult + dir_boost;
		const double lr_m = 0.0, rl_m = 0.0;

		const sample_t out_l = s0_d*ll_m + s1_d*lr_m;
		const sample_t out_r = s0_d*rl_m + s1_d*rr_m;
		const sample_t out_ls = s0_d*m.lsl + s1_d*m.lsr;
		const sample_t out_rs = s0_d*m.rsl + s1_d*m.rsr;

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
			dsp_log_printf("\n%s%s: lr: %+06.2f (%+06.2f); cs: %+06.2f (%+06.2f); dir_boost: %05.3f; adj: %05.3f; ord: %zd; diff: %zd; early: %zd\033[K\r\033[A",
				e->name, (state->disable) ? " [off]" : "", TO_DEGREES(state->ax.lr), TO_DEGREES(state->ax_ev.lr), TO_DEGREES(state->ax.cs), TO_DEGREES(state->ax_ev.cs),
				dir_boost, state->ev.adj, state->ev.ord_count, state->ev.diff_count, state->ev.early_count);
			dsp_log_release();
		}
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
	state->fade_p = state->fade_frames - state->fade_p;
	if (!state->show_status)
		LOG_FMT(LL_NORMAL, "%s: %s", e->name, (state->disable) ? "disabled" : "enabled");
}

sample_t * matrix4_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct matrix4_state *state = (struct matrix4_state *) e->data;
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
			rbuf = matrix4_effect_run(e, frames, buf1, buf2);
		}
		else
			*frames = -1;
	}
	return rbuf;
}

void matrix4_effect_destroy(struct effect *e)
{
	int i;
	struct matrix4_state *state = (struct matrix4_state *) e->data;
	for (i = 0; i < e->istream.channels; ++i)
		free(state->bufs[i]);
	free(state->bufs);
	event_state_cleanup(&state->ev);
	#ifndef LADSPA_FRONTEND
		if (state->show_status) {
			dsp_log_acquire();
			dsp_log_printf("\033[K\n\033[K\r\033[A");
			dsp_log_release();
		}
	#endif
	free(state);
}

struct effect * matrix4_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effect *e;
	struct matrix4_state *state;
	struct matrix4_config config = {0};

	if (get_args_and_channels(ei, istream, channel_selector, argc, argv, &config))
		return NULL;
	if (parse_effect_opts(argv, istream, &config))
		return NULL;

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = istream->channels + 2;
	e->run = matrix4_effect_run;
	e->delay = matrix4_effect_delay;
	e->reset = matrix4_effect_reset;
	e->drain2 = matrix4_effect_drain2;
	e->destroy = matrix4_effect_destroy;

	state = calloc(1, sizeof(struct matrix4_state));
	state->c0 = config.c0;
	state->c1 = config.c1;
	state->show_status = config.show_status;
	state->do_dir_boost = config.do_dir_boost;
	e->signal = (config.enable_signal) ? matrix4_effect_signal : NULL;

	for (int i = 0; i < 2; ++i) {
		biquad_init_using_type(&state->in_hp[i], BIQUAD_HIGHPASS, istream->fs,  500.0, 0.5, 0, 0, BIQUAD_WIDTH_Q);
		biquad_init_using_type(&state->in_lp[i], BIQUAD_LOWPASS,  istream->fs, 5000.0, 0.5, 0, 0, BIQUAD_WIDTH_Q);
	}
	smooth_state_init(&state->sm, istream);
	event_state_init(&state->ev, istream, 1.0);
	smf_asym_init(&state->dir_boost_smooth, istream->fs,
		SMF_RISE_TIME(DIR_BOOST_RT0), DIR_BOOST_SENS_RISE, DIR_BOOST_SENS_FALL);

	state->len = TIME_TO_FRAMES(DELAY_TIME, istream->fs);
	state->bufs = calloc(istream->channels, sizeof(sample_t *));
	for (int i = 0; i < istream->channels; ++i)
		state->bufs[i] = calloc(state->len, sizeof(sample_t));
	state->surr_mult = config.surr_mult;
	state->norm_mult = CALC_NORM_MULT(config.surr_mult);
	state->fade_frames = TIME_TO_FRAMES(FADE_TIME, istream->fs);
	event_config_init(&state->evc, istream);

	struct effect *e_delay = matrix4_delay_effect_init(ei, &e->ostream, config.surr_delay_frames);

	e->data = state;
	e->next = e_delay;
	return e;
}
