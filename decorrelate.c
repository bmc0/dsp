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
#include <math.h>
#include <pthread.h>
#include "decorrelate.h"
#include "util.h"

/* This is an implementation of the allpass decorrelator described in
 * "Frequency-Dependent Schroeder Allpass Filters" by Sebastian J. Schlecht
 * (doi:10.3390/app10010187) https://www.mdpi.com/2076-3417/10/1/187
*/

#define FILTER_FC 1100.0
#define RT60_LF   0.1
#define RT60_HF   0.008

struct sch_ap_state {
	int len, p;
	sample_t *mx, *my;
	sample_t b0, b1, a0, a1;
};

struct decorrelate_state {
	int n_stages;
	struct sch_ap_state **ap;
};

static void sch_ap_init(struct sch_ap_state *ap, int fs, double delay)
{
	const int delay_samples = lround(delay*fs);
	ap->len = delay_samples+1;
	ap->p = 0;
	ap->mx = calloc(ap->len, sizeof(sample_t));
	ap->my = calloc(ap->len, sizeof(sample_t));

	const double gain_lf = -60.0/(RT60_LF * fs) * delay_samples;
	const double gain_hf = -60.0/(RT60_HF * fs) * delay_samples;
	const double w0 = 2.0 * M_PI * FILTER_FC / fs;
	const double t = tan(w0/2.0);
	const double g_hf = pow(10.0, gain_hf/20.0);
	const double gd = pow(10.0, (gain_lf-gain_hf)/20.0);
	const double sgd = sqrt(gd);
	ap->a0 = t + sgd;
	ap->a1 = (t - sgd) / ap->a0;
	ap->b0 = (gd*t - sgd) / ap->a0 * g_hf;
	ap->b1 = (gd*t + sgd) / ap->a0 * g_hf;
	ap->a0 = 1.0;
}

static sample_t sch_ap_run(struct sch_ap_state *ap, sample_t x)
{
	const int i0 = ((ap->p < 1) ? ap->len : ap->p)-1, i_n1 = ap->p, i_n2 = (ap->p+1 >= ap->len) ? 0 : ap->p+1;
	const sample_t r = ap->b1*x + ap->b0*ap->mx[i0] + ap->a1*ap->mx[i_n2] + ap->a0*ap->mx[i_n1]
		- ap->a1*ap->my[i0] - ap->b0*ap->my[i_n2] - ap->b1*ap->my[i_n1];
	ap->mx[ap->p] = x;
	ap->my[ap->p] = r;
	ap->p = (ap->p+1 >= ap->len) ? 0 : ap->p+1;
	return r;
}

static void sch_ap_reset(struct sch_ap_state *ap)
{
	ap->p = 0;
	memset(ap->mx, 0, ap->len * sizeof(sample_t));
	memset(ap->my, 0, ap->len * sizeof(sample_t));
}

static void sch_ap_destroy(struct sch_ap_state *ap)
{
	free(ap->mx);
	free(ap->my);
}

sample_t * decorrelate_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, samples = *frames * e->ostream.channels;
	int k, j;
	struct decorrelate_state *state = (struct decorrelate_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels)
		for (k = 0; k < e->ostream.channels; ++k)
			if (state->ap[k])
				for (j = 0; j < state->n_stages; ++j)
					ibuf[i + k] = sch_ap_run(&state->ap[k][j], ibuf[i + k]);
	return ibuf;
}

void decorrelate_effect_reset(struct effect *e)
{
	int k, j;
	struct decorrelate_state *state = (struct decorrelate_state *) e->data;
	for (k = 0; k < e->ostream.channels; ++k)
		if (state->ap[k])
			for (j = 0; j < state->n_stages; ++j)
				sch_ap_reset(&state->ap[k][j]);
}

void decorrelate_effect_plot(struct effect *e, int i)
{
	struct decorrelate_state *state = (struct decorrelate_state *) e->data;
	for (int k = 0; k < e->ostream.channels; ++k) {
		if (state->ap[k]) {
			printf("H%d_%d(w)=(abs(w)<=pi)?1.0", k, i);
			for (int j = 0; j < state->n_stages; ++j) {
				struct sch_ap_state *ap = &state->ap[k][j];
				printf("*((%.15e+%.15e*exp(-j*w)+%.15e*exp(-j*w*%d)+%.15e*exp(-j*w*%d))/(1.0+%.15e*exp(-j*w)+%.15e*exp(-j*w*%d)+%.15e*exp(-j*w*%d)))",
					ap->b1, ap->b0, ap->a1, ap->len-1, ap->a0, ap->len, ap->a1, ap->b0, ap->len-1, ap->b1, ap->len);
			}
			puts(":0/0");
		}
		else
			printf("H%d_%d(w)=1.0\n", k, i);
	}
}

void decorrelate_effect_destroy(struct effect *e)
{
	int k, j;
	struct decorrelate_state *state = (struct decorrelate_state *) e->data;
	for (k = 0; k < e->ostream.channels; ++k) {
		if (state->ap[k]) {
			for (j = 0; j < state->n_stages; ++j)
				sch_ap_destroy(&state->ap[k][j]);
			free(state->ap[k]);
		}
	}
	free(state->ap);
	free(state);
}

#define RANDOM_FILTER_DELAY ((double)pm_rand1_r((opt_seed>0)?&opt_seed:&seed)/PM_RAND_MAX * 2.2917e-3 + 0.83333e-3)

struct effect * decorrelate_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	static pthread_mutex_t rand_lock = PTHREAD_MUTEX_INITIALIZER;
	static uint32_t seed = 1;

	struct decorrelate_state *state;
	struct effect *e;
	char *endptr;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int mono = 0, n_stages = 5, opt;
	uint32_t opt_seed = 0;

	while ((opt = dsp_getopt(&g, argc, argv, "ms:")) != -1) {
		long int v;
		switch (opt) {
		case 'm': mono = 1; break;
		case 's':
			v = strtol(g.arg, &endptr, 10);
			CHECK_ENDPTR(g.arg, endptr, "seed", return NULL);
			CHECK_RANGE(v > 0 && v <= PM_RAND_MAX, "seed", return NULL);
			seed = v;
			break;
		case ':':
			LOG_FMT(LL_ERROR, "%s: error: expected argument to option '%c'", argv[0], g.opt);
			return NULL;
		default: goto print_usage;
		}
	}
	if (g.ind < argc-1) {
		print_usage:
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	else if (g.ind == argc-1) {
		n_stages = strtol(argv[g.ind], &endptr, 10);
		CHECK_ENDPTR(argv[g.ind], endptr, "stages", return NULL);
		CHECK_RANGE(n_stages > 0 && n_stages <= 100, "stages", return NULL);
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->run = decorrelate_effect_run;
	e->reset = decorrelate_effect_reset;
	e->plot = decorrelate_effect_plot;
	e->destroy = decorrelate_effect_destroy;
	state = calloc(1, sizeof(struct decorrelate_state));
	state->n_stages = n_stages;
	state->ap = calloc(istream->channels, sizeof(struct sch_ap_state *));
	for (int k = 0; k < istream->channels; ++k) {
		if (GET_BIT(channel_selector, k))
			state->ap[k] = calloc(n_stages, sizeof(struct sch_ap_state));
	}
	pthread_mutex_lock(&rand_lock);
	for (int j = 0; j < n_stages; ++j) {
		const double d = (mono) ? RANDOM_FILTER_DELAY : 0.0;
		for (int k = 0; k < istream->channels; ++k) {
			if (GET_BIT(channel_selector, k))
				sch_ap_init(&state->ap[k][j], istream->fs, (mono) ? d : RANDOM_FILTER_DELAY);
		}
	}
	pthread_mutex_unlock(&rand_lock);
	e->data = state;
	return e;
}
