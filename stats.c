/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "stats.h"
#include "util.h"

#define STATS_DEFAULT_WIDTH 80

struct stats_interp_state {
	double m[64], y[6], tmin, tmax, z[9];
	int p, zp, n;
};

struct stats_ch_state {
	double sum, sum_sq, min, max, peak;
	ssize_t peak_count, peak_frame;
	struct stats_interp_state interp;
	int ch;
};

struct stats_state {
	struct stats_ch_state *cs;
	double ref;
	ssize_t samples;
	int width, n_cs;
};

sample_t * stats_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples = *frames * e->ostream.channels;
	struct stats_state *state = (struct stats_state *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels) {
		for (int k = 0; k < state->n_cs; ++k) {
			struct stats_ch_state *cs = &state->cs[k];
			const double s = ibuf[i+cs->ch];
			cs->sum += s;
			cs->sum_sq += s*s;
			int pk = 0;
			if      (s <= cs->min) { cs->min = s; pk = 1; }
			else if (s >= cs->max) { cs->max = s; pk = 1; }
			if (pk) {
				const double abs_s = fabs(s);
				if (abs_s > 0.0 && abs_s == cs->peak)
					++cs->peak_count;
				else if (abs_s > cs->peak) {
					cs->peak = abs_s;
					cs->peak_frame = state->samples;
					cs->peak_count = 1;
				}
			}
		}
		++state->samples;
	}
	return ibuf;
}

#define STATS_INTERP_DELAY 18  /* actually 7.75+1+9 samples (fir+quadratic+lookahead) */
static void stats_interp_insert(struct stats_interp_state *state, double x)
{
	const double r[24] = {  /* 4x half filter with every 4th coefficient omitted */
		-9.353493881474939e-04*x, -2.811275711123766e-03*x, -3.165361696477658e-03*x,
		+5.929994218827107e-03*x, +1.065865725083938e-02*x, +9.308373173634579e-03*x,
		-1.340062089976642e-02*x, -2.227979776029874e-02*x, -1.833945608477310e-02*x,
		+2.430932418366197e-02*x, +3.925899279385184e-02*x, +3.157919724264597e-02*x,
		-4.056172445833198e-02*x, -6.489751870004079e-02*x, -5.192701793078084e-02*x,
		+6.684049697012354e-02*x, +1.078342211598459e-01*x, +8.751763525896815e-02*x,
		-1.187292496637064e-01*x, -2.001458972657618e-01*x, -1.729186314209981e-01*x,
		+2.957854651930789e-01*x, +6.325370350028462e-01*x, +8.988707620097378e-01*x,
	};
	int p = state->p;
	double *m = state->m, *y = state->y;
	y[0]=y[4]; y[1]=y[5];  /* for quadratic peak estimation */
	y[2]=m[p++]+r[0]; y[3]=m[p++]+r[1]; y[4]=m[p++]+r[2]; y[5]=m[p++]; p&=0x3f;
	memset(&m[state->p], 0, sizeof(double)*4);
	state->p = p;
	m[p++]+=r[3];  m[p++]+=r[4];  m[p++]+=r[5];  p++; p&=0x3f;
	m[p++]+=r[6];  m[p++]+=r[7];  m[p++]+=r[8];  p++; p&=0x3f;
	m[p++]+=r[9];  m[p++]+=r[10]; m[p++]+=r[11]; p++; p&=0x3f;
	m[p++]+=r[12]; m[p++]+=r[13]; m[p++]+=r[14]; p++; p&=0x3f;
	m[p++]+=r[15]; m[p++]+=r[16]; m[p++]+=r[17]; p++; p&=0x3f;
	m[p++]+=r[18]; m[p++]+=r[19]; m[p++]+=r[20]; p++; p&=0x3f;
	m[p++]+=r[21]; m[p++]+=r[22]; m[p++]+=r[23]; m[p++]+=x; p&=0x3f;
	m[p++]+=r[23]; m[p++]+=r[22]; m[p++]+=r[21]; p++; p&=0x3f;
	m[p++]+=r[20]; m[p++]+=r[19]; m[p++]+=r[18]; p++; p&=0x3f;
	m[p++]+=r[17]; m[p++]+=r[16]; m[p++]+=r[15]; p++; p&=0x3f;
	m[p++]+=r[14]; m[p++]+=r[13]; m[p++]+=r[12]; p++; p&=0x3f;
	m[p++]+=r[11]; m[p++]+=r[10]; m[p++]+=r[9];  p++; p&=0x3f;
	m[p++]+=r[8];  m[p++]+=r[7];  m[p++]+=r[6];  p++; p&=0x3f;
	m[p++]+=r[5];  m[p++]+=r[4];  m[p++]+=r[3];  p++; p&=0x3f;
	m[p++]+=r[2];  m[p++]+=r[1];  m[p++]+=r[0];
}

static void stats_interp_peak(struct stats_state *state, struct stats_ch_state *cs)
{
	double *y = cs->interp.y;
	int r = 0;
	for (int i = 1; i < LENGTH(cs->interp.y)-1; ++i) {
		const double d0 = y[i]-y[i-1], d1=y[i]-y[i+1];
		if ((d0 > 0.0 && d1 < 0.0) || (d0 < 0.0 && d1 > 0.0) || (d0 == 0.0 && d1 == 0.0))
			continue;  /* no extrema */
		const double dy = y[i-1]-y[i+1];
		/* quadratic peak estimation */
		const double p_4 = dy/(8.0*(y[i-1]-2.0*y[i]+y[i+1]));
		const double yq = y[i] - dy*p_4;
		int pk = 0;
		if      (yq <= cs->min) { cs->min = yq; cs->interp.tmin = 0.5*yq; pk = 1; }
		else if (yq >= cs->max) { cs->max = yq; cs->interp.tmax = 0.5*yq; pk = 1; }
		if (pk) {
			const double ayq = fabs(yq);
			if (ayq > 0.0 && ayq == cs->peak) r = 1;
			else if (ayq > cs->peak) { cs->peak = ayq; r = 2; }
		}
	}
	if (r == 2) {
		cs->peak_frame = state->samples - (STATS_INTERP_DELAY-1);
		cs->peak_count = 1;
	}
	else if (r == 1)
		++cs->peak_count;
}

sample_t * stats_effect_run_interp(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples = *frames * e->ostream.channels;
	struct stats_state *state = (struct stats_state *) e->data;
	for (ssize_t i = 0; i < samples; i += e->ostream.channels) {
		for (int k = 0; k < state->n_cs; ++k) {
			struct stats_ch_state *cs = &state->cs[k];
			struct stats_interp_state *cis = &cs->interp;
			const double s = ibuf[i+cs->ch];
			cs->sum += s;
			cs->sum_sq += s*s;
			if (s < cis->tmin || s > cis->tmax)
				cis->n = STATS_INTERP_DELAY;
			if (cis->n > 0) {
				stats_interp_insert(cis, cis->z[cis->zp]);
				stats_interp_peak(state, cs);
				--cis->n;
			}
			cis->z[cis->zp++] = s;
			if (cis->zp >= LENGTH(cis->z)) cis->zp = 0;
		}
		++state->samples;
	}
	return ibuf;
}

static void stats_print_channels(struct effect *e, int start, int end)
{
	struct stats_state *state = (struct stats_state *) e->data;
	dsp_log_printf("\n%-18s", "Channel");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12d", state->cs[i].ch);
	dsp_log_printf("\n%-18s", "DC offset");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state->cs[i].sum/state->samples);
	dsp_log_printf("\n%-18s", "Minimum");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state->cs[i].min);
	dsp_log_printf("\n%-18s", "Maximum");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.8f", state->cs[i].max);
	dsp_log_printf("\n%-18s", "Peak level (dBFS)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(state->cs[i].peak));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "Peak level (dBr)");
		for (int i = start; i < end; ++i)
			dsp_log_printf(" %12.4f", state->ref + 20.0*log10(state->cs[i].peak));
	}
	dsp_log_printf("\n%-18s", "RMS level (dBFS)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(sqrt(state->cs[i].sum_sq/state->samples)));
	if (state->ref != -HUGE_VAL) {
		dsp_log_printf("\n%-18s", "RMS level (dBr)");
		for (int i = start; i < end; ++i)
			dsp_log_printf(" %12.4f", state->ref + 20.0*log10(sqrt(state->cs[i].sum_sq/state->samples)));
	}
	dsp_log_printf("\n%-18s", "Crest factor (dB)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.4f", 20.0*log10(state->cs[i].peak/sqrt(state->cs[i].sum_sq/state->samples)));
	dsp_log_printf("\n%-18s", "Peak count");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state->cs[i].peak_count);
	dsp_log_printf("\n%-18s", "Peak sample");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state->cs[i].peak_frame);
	dsp_log_printf("\n%-18s", "Samples");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12zd", state->samples);
	dsp_log_printf("\n%-18s", "Length (s)");
	for (int i = start; i < end; ++i)
		dsp_log_printf(" %12.2f", (double) state->samples / e->ostream.fs);
	dsp_log_printf("\n");
}

void stats_effect_destroy(struct effect *e)
{
	struct stats_state *state = (struct stats_state *) e->data;
	if (e->run == stats_effect_run_interp) {
		for (ssize_t i = 0; i < STATS_INTERP_DELAY; ++i) {
			for (int k = 0; k < state->n_cs; ++k) {
				struct stats_ch_state *cs = &state->cs[k];
				struct stats_interp_state *cis = &cs->interp;
				if (cis->n > 0) {
					stats_interp_insert(cis, cis->z[cis->zp]);
					stats_interp_peak(state, cs);
					--cis->n;
				}
				cis->z[cis->zp++] = 0.0;
				if (cis->zp >= LENGTH(cis->z)) cis->zp = 0;
			}
			++state->samples;
		}
		state->samples -= STATS_INTERP_DELAY;
	}
	int cols = state->n_cs;
	dsp_log_acquire();
#ifdef DSP_STATUSLINES
	if (state->width < 0) {
		dsp_get_term_size(NULL, &state->width);
		if (state->width <= 0) state->width = STATS_DEFAULT_WIDTH;
	}
#endif
	if (state->width > 0)
		cols = MAXIMUM((state->width-18)/13, 1);
	for (int i = 0; i < state->n_cs; i+=cols)
		stats_print_channels(e, i, MINIMUM(i+cols, state->n_cs));
	dsp_log_release();
	free(state->cs);
	free(state);
}

struct effect * stats_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	int width = STATS_DEFAULT_WIDTH, do_interp = 0, opt;
	double ref = -HUGE_VAL;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	char *endptr;

	while ((opt = dsp_getopt(&g, argc, argv, "w:i")) != -1) {
		switch (opt) {
		case 'w':
			if (strcmp(g.arg, "auto") == 0)
				width = -1;
			else {
				width = strtol(g.arg, &endptr, 10);
				CHECK_ENDPTR(g.arg, endptr, "width", return NULL);
				if (width < 0) {
					LOG_FMT(LL_ERROR, "%s: error: width must be positive or zero", argv[0]);
					return NULL;
				}
			}
			break;
		case 'i':
			do_interp = 1;
			break;
		case ':':
			LOG_FMT(LL_ERROR, "%s: error: expected argument to option '%c'", argv[0], g.opt);
			return NULL;
		default: goto print_usage;
		}
	}

	if (g.ind == argc-1) {
		ref = strtod(argv[1], &endptr);
		CHECK_ENDPTR(argv[1], endptr, "ref_level", return NULL);
	}
	else if (g.ind != argc) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}

	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_NO_DITHER;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->flags |= EFFECT_FLAG_ALIGN_BARRIER;
	e->run = (do_interp) ? stats_effect_run_interp : stats_effect_run;
	e->plot = effect_plot_noop;
	e->destroy = stats_effect_destroy;
	struct stats_state *state = calloc(1, sizeof(struct stats_state));
	state->ref = ref;
	state->width = width;
	state->n_cs = num_bits_set(channel_selector, istream->channels);
	state->cs = calloc(state->n_cs, sizeof(struct stats_ch_state));
	for (int i = 0, k = 0; k < istream->channels; ++k) {
		if (GET_BIT(channel_selector, k))
			state->cs[i++].ch = k;
	}
	e->data = state;
	return e;
}
