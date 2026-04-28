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
#include <float.h>
#include <math.h>
#include <pthread.h>
#include "delay.h"
#include "allpass.h"
#include "util.h"

struct delay_channel_state {
	void (*run)(struct delay_channel_state *, ssize_t, sample_t *, int);
	union {
		struct ap1_state first;
		struct ap2_state second;
		struct thiran_ap_state *nth;
	} fd_ap;
	double samples_frac;
	ssize_t samples_int;
	int fd_ap_n;
};

struct delay_state {
	struct delay_channel_state *cs;
};

#define DELAY_MIN_FRAC 0.1
#define DELAY_FD_AP_N_DEFAULT 2
#define MOD_INTERP_FIR 1

#define DELAY_RUN_CHANNEL_DEFINE_FN(X, C) \
	static void delay_run_channel ## X (struct delay_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride) \
	{ for (ssize_t i = frames; i > 0; --i) { C ibuf_p += stride; } }

DELAY_RUN_CHANNEL_DEFINE_FN(_frac_1, *ibuf_p = ap1_run(&cs->fd_ap.first, *ibuf_p);    )
DELAY_RUN_CHANNEL_DEFINE_FN(_frac_2, *ibuf_p = ap2_run(&cs->fd_ap.second, *ibuf_p);   )
DELAY_RUN_CHANNEL_DEFINE_FN(_frac_n, *ibuf_p = thiran_ap_run(cs->fd_ap.nth, *ibuf_p); )

static sample_t * delay_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->run) cs->run(cs, *frames, &ibuf[k], e->istream.channels);
	}
	return ibuf;
}

static sample_t * delay_effect_run_noop(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	return ibuf;
}

static void delay_effect_reset(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->fd_ap_n == 1)
			ap1_reset(&cs->fd_ap.first);
		else if (cs->fd_ap_n == 2)
			ap2_reset(&cs->fd_ap.second);
		else if (cs->fd_ap_n > 2)
			thiran_ap_reset(cs->fd_ap.nth);
	}
}

static void delay_effect_plot(struct effect *e, int i)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		printf("H%d_%d(w)=exp(-j*w*%zd)", k, i, cs->samples_int);
		if (cs->fd_ap_n == 1) {
			printf("*((abs(w)<=pi)?(%.15e+1.0*exp(-j*w))/(1.0+%.15e*exp(-j*w)):0/0)",
				cs->fd_ap.first.c0, cs->fd_ap.first.c0);
		}
		else if (cs->fd_ap_n == 2) {
			printf("*((abs(w)<=pi)?(%.15e+%.15e*exp(-j*w)+exp(-2*j*w))/(1.0+%.15e*exp(-j*w)+%.15e*exp(-2*j*w)):0/0)",
				cs->fd_ap.second.c1, cs->fd_ap.second.c0, cs->fd_ap.second.c0, cs->fd_ap.second.c1);
		}
		else if (cs->fd_ap_n > 2) {
			putchar('*');
			thiran_ap_plot(cs->fd_ap.nth);
		}
		putchar('\n');
	}
}

static void delay_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		drain_samples[k] += state->cs[k].fd_ap_n;
}

static void delay_effect_destroy(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	if (state->cs) {
		for (int k = 0; k < e->istream.channels; ++k) {
			struct delay_channel_state *cs = &state->cs[k];
			if (cs->fd_ap_n > 2) free(cs->fd_ap.nth);
		}
		free(state->cs);
	}
	free(state);
}

static int delay_effect_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge == src->merge) {
		struct delay_state *dest_state = (struct delay_state *) dest->data;
		struct delay_state *src_state = (struct delay_state *) src->data;
		for (int k = 0; k < dest->istream.channels; ++k) {
			struct delay_channel_state *dest_cs = &dest_state->cs[k], *src_cs = &src_state->cs[k];
			dest_cs->samples_int += src_cs->samples_int;
			dest_cs->samples_frac += src_cs->samples_frac;
			dest_cs->fd_ap_n = MAXIMUM(dest_cs->fd_ap_n, src_cs->fd_ap_n);
		}
		return 1;
	}
	return 0;
}

static void delay_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct delay_state *state = (struct delay_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		req_delay[k] += state->cs[k].samples_int;
}

static int delay_effect_prepare(struct effect *e)
{
	struct delay_state *state = (struct delay_state *) e->data;
	int is_noop = 1;
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->fd_ap_n < 1)
			cs->fd_ap_n = DELAY_FD_AP_N_DEFAULT;
		/* ignore extremely small fractional delay */
		if (fabs(cs->samples_frac - rint(cs->samples_frac)) >= DBL_EPSILON) {
			const ssize_t adj = (cs->fd_ap_n-1) - ((ssize_t) floor(cs->samples_frac-DELAY_MIN_FRAC));
			cs->samples_int -= adj;
			cs->samples_frac += adj;
		}
		else {
			cs->samples_int += lrint(cs->samples_frac);
			cs->samples_frac = 0.0;
			cs->fd_ap_n = 0;
		}
	}
	for (int k = 0; k < e->istream.channels; ++k) {
		struct delay_channel_state *cs = &state->cs[k];
		if (cs->fd_ap_n > 0) {
			const double delta = fabs(cs->samples_frac);
			if (cs->fd_ap_n == 1) {
				cs->run = delay_run_channel_frac_1;
				cs->fd_ap.first.c0 = (1.0-delta)/(1.0+delta);
			}
			else if (cs->fd_ap_n == 2) {
				cs->run = delay_run_channel_frac_2;
				cs->fd_ap.second.c0 = (4.0-2.0*delta)/(1.0+delta);
				cs->fd_ap.second.c1 = ((delta-2.0)*(delta-1.0))/((delta+1.0)*(delta+2.0));
			}
			else if (cs->fd_ap_n > 2) {
				cs->run = delay_run_channel_frac_n;
				if ((cs->fd_ap.nth = thiran_ap_new(cs->fd_ap_n, delta)) == NULL) {
					LOG_FMT(LL_ERROR, "%s: error: thiran_ap_new() failed", e->name);
					return 1;
				}
			}
			is_noop = 0;
			/* LOG_FMT(LL_VERBOSE, "%s: info: channel %d: total delay is %gs (%zd%+g samples)",
				e->name, k, (cs->samples_int+cs->samples_frac) / e->istream.fs,
				cs->samples_int, cs->samples_frac); */
		}
		else {
			cs->run = NULL;  /* handled via effect.channel_offsets() */
			/* LOG_FMT(LL_VERBOSE, "%s: info: channel %d: total delay is %gs (%zd sample%s)",
				e->name, k, (double) cs->samples_int / e->istream.fs, cs->samples_int,
				(cs->samples_int == 1) ? "" : "s"); */
		}
	}
	if (is_noop)
		e->run = delay_effect_run_noop;  /* nothing to do */

	return 0;
}

static struct effect * delay_effect_init_common(const char *name, const struct stream_info *istream, const char *channel_selector, ssize_t samples_int, double samples_frac, int fd_ap_n)
{
	struct effect *e = NULL;
	struct delay_state *state = NULL;

	e = calloc(1, sizeof(struct effect));
	if (check_alloc(name, e)) goto fail;
	e->name = name;
	if (samples_int == 0 && samples_frac == 0.0)
		return e;  /* nothing to do */
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->prepare = delay_effect_prepare;
	e->run = delay_effect_run;
	e->reset = delay_effect_reset;
	e->plot = delay_effect_plot;
	e->drain_samples = delay_effect_drain_samples;
	e->destroy = delay_effect_destroy;
	e->merge = delay_effect_merge;
	e->channel_offsets = delay_effect_channel_offsets;

	e->data = state = calloc(1, sizeof(struct delay_state));
	if (check_alloc(name, state)) goto fail;
	state->cs = calloc(e->istream.channels, sizeof(struct delay_channel_state));
	if (check_alloc(name, state->cs)) goto fail;
	for (int k = 0; k < e->istream.channels; ++k) {
		if (GET_BIT(channel_selector, k)) {
			state->cs[k].samples_int = samples_int;
			state->cs[k].samples_frac = samples_frac;
			state->cs[k].fd_ap_n = fd_ap_n;
		}
	}
	return e;

	fail:
	if (state) delay_effect_destroy(e);
	free(e);
	return NULL;
}

struct effect * delay_effect_init_int(const char *name, const struct stream_info *istream, const char *channel_selector, ssize_t samples_int)
{
	return delay_effect_init_common(name, istream, channel_selector, samples_int, 0.0, 0);
}

struct effect * delay_effect_init_frac(const char *name, const struct stream_info *istream, const char *channel_selector, double samples_frac, int fd_ap_n)
{
	return delay_effect_init_common(name, istream, channel_selector, 0, samples_frac, fd_ap_n);
}

/* Windowed sinc (Dolph-Chebyshev [fc=0.91 stop=76dB]); 6 phase; 16 taps/phase */
static const sample_t mod_flt_q1[6][16] = {
	/* phase 0 */
	{ -1.23278026367039893e-04, +8.19705936926010516e-04, -3.85162660047126855e-03, +1.19920594020032564e-02,
	  -2.84459487116192830e-02, +5.53698626084192097e-02, -9.44064738589076718e-02, +1.63822770142293989e-01,
	  +9.00984219449725288e-01, +1.21296149120120149e-02, -3.71275071349935135e-02, +3.16862443494606572e-02,
	  -1.97154254307645334e-02, +9.43450574448295200e-03, -3.33820855559853816e-03, +7.64758427782342900e-04, },
	/* phase 1 */
	{ -3.22231083954224864e-05, +5.78478355244285052e-04, -3.34512885441808636e-03, +1.19529454585944293e-02,
	  -3.19473840616454757e-02, +7.07010403858260167e-02, -1.43169863970712213e-01, +3.43997355394093540e-01,
	  +8.31142854536729314e-01, -9.64853597736237689e-02, +1.51647805275226498e-02, +6.50055148215965793e-03,
	  -8.89422479292227043e-03, +5.55790219355951562e-03, -2.25573961443770871e-03, +5.47827446051560134e-04, },
	/* phase 2 */
	{ +8.92401095872863816e-05, -4.93340795382755823e-05, -1.52047437082791868e-03, +8.39244404534354033e-03,
	  -2.76533624663673067e-02, +7.12531932152865449e-02, -1.68261631125487060e-01, +5.31470622577906449e-01,
	  +7.01745718823633347e-01, -1.55834940559989588e-01, +5.28389390370281042e-02, -1.44710312950771091e-02,
	  +1.14582060567898275e-03, +1.60093881639606066e-03, -1.04960742710502510e-03, +2.94379835522039354e-04, },
	/* phase 3 */
	{ +2.94379835522039354e-04, -1.04960742710502510e-03, +1.60093881639606066e-03, +1.14582060567898275e-03,
	  -1.44710312950771091e-02, +5.28389390370281042e-02, -1.55834940559989588e-01, +7.01745718823633347e-01,
	  +5.31470622577906449e-01, -1.68261631125487060e-01, +7.12531932152865449e-02, -2.76533624663673067e-02,
	  +8.39244404534354033e-03, -1.52047437082791868e-03, -4.93340795382755823e-05, +8.92401095872863816e-05, },
	/* phase 4 */
	{ +5.47827446051560134e-04, -2.25573961443770871e-03, +5.55790219355951562e-03, -8.89422479292227043e-03,
	  +6.50055148215965793e-03, +1.51647805275226498e-02, -9.64853597736237689e-02, +8.31142854536729314e-01,
	  +3.43997355394093540e-01, -1.43169863970712213e-01, +7.07010403858260167e-02, -3.19473840616454757e-02,
	  +1.19529454585944293e-02, -3.34512885441808636e-03, +5.78478355244285052e-04, -3.22231083954224864e-05, },
	/* phase 5 */
	{ +7.64758427782342900e-04, -3.33820855559853816e-03, +9.43450574448295200e-03, -1.97154254307645334e-02,
	  +3.16862443494606572e-02, -3.71275071349935135e-02, +1.21296149120120149e-02, +9.00984219449725288e-01,
	  +1.63822770142293989e-01, -9.44064738589076718e-02, +5.53698626084192097e-02, -2.84459487116192830e-02,
	  +1.19920594020032564e-02, -3.85162660047126855e-03, +8.19705936926010516e-04, -1.23278026367039893e-04, },
};

/* Windowed sinc (Dolph-Chebyshev [fc=0.936 stop=120dB]); 16 phase; 32 taps/phase */
static const sample_t mod_flt_q2[16][32] = {
	/* phase 0 */
	{ +3.52203560958273242e-07, +2.59335444498408197e-07, -8.63543507831690459e-06, +5.41782023281094688e-05,
	  -2.14968397675198821e-04, +6.54587405761760223e-04, -1.65534458315876975e-03, +3.62599902413910909e-03,
	  -7.05998369580099119e-03, +1.24322245327915110e-02, -2.00498015340008634e-02, +2.99079391918543984e-02,
	  -4.16412715167827421e-02, +5.47242835760801336e-02, -6.94106300051381270e-02, +9.26367266878741630e-02,
	  +9.34659835569005670e-01, +3.27275923093596349e-02, -4.30355762190085395e-02, +4.05510644669190984e-02,
	  -3.38644710320998160e-02, +2.58370541957508131e-02, -1.81191346073100341e-02, +1.16629100398877219e-02,
	  -6.85214165383429569e-03, +3.64027125212970475e-03, -1.72433823851031128e-03, +7.13059339589778435e-04,
	  -2.49042503366221038e-04, +6.94369144800050984e-05, -1.38345652090794398e-05, +1.47657338138237595e-06, },
	/* phase 1 */
	{ +3.00672929586365958e-07, -1.28533481673772682e-06, -2.06258286374106165e-06, +3.43263311803131298e-05,
	  -1.67942970870100242e-04, +5.64345510648012649e-04, -1.51715391617144300e-03, +3.47596801216177231e-03,
	  -7.02544660546436210e-03, +1.28050659955456381e-02, -2.13844716984531481e-02, +3.31579739740803306e-02,
	  -4.84012813739312445e-02, +6.78538709069775592e-02, -9.53971928586008489e-02, +1.57827184507585677e-01,
	  +9.23981829283398781e-01, -2.10402952286613404e-02, -1.71333823111622686e-02, +2.58625947872180226e-02,
	  -2.53876726254017027e-02, +2.11290673254453110e-02, -1.56935838023534585e-02, +1.05489379470937329e-02,
	  -6.42615675512466910e-03, +3.52886913493476520e-03, -1.72772559816036634e-03, +7.40782114113007289e-04,
	  -2.70363211971824397e-04, +8.01171751606922442e-05, -1.76614805540458895e-05, +2.37371672136199670e-06, },
	/* phase 2 */
	{ +5.38958652132745187e-07, -3.14759256398561512e-06, +5.81902961819810933e-06, +1.00679820256261042e-05,
	  -1.08285429792171793e-04, +4.42479799425374385e-04, -1.30853613395380108e-03, +3.18442461158771053e-03,
	  -6.73154362299371607e-03, +1.27406949462928621e-02, -2.20373396659370689e-02, +3.54219436008549604e-02,
	  -5.38451015428494523e-02, +7.94146554042998998e-02, -1.20081845680832119e-01, +2.27281638248904905e-01,
	  +9.02850420266162468e-01, -6.79807035242710744e-02, +7.50782819677348230e-03, +1.11734043163622034e-02,
	  -1.65335796397600050e-02, +1.59789948473352747e-02, -1.28851935703063938e-02, +9.15069668284228775e-03,
	  -5.81216129946867700e-03, +3.30551875235304132e-03, -1.67112829106305649e-03, +7.39795239924908839e-04,
	  -2.79582721645488003e-04, +8.64098456164380838e-05, -2.01746120233491894e-05, +2.97199787604816103e-06, },
	/* phase 3 */
	{ +8.33252056086746835e-07, -5.29748633904858601e-06, +1.48702407527813701e-05, -1.81893688771127625e-05,
	  -3.68799832657953401e-05, +2.90418542206939473e-04, -1.03088692719296667e-03, +2.75052232785120889e-03,
	  -6.16971333509590443e-03, +1.22117024890106493e-02, -2.19419883099599838e-02, +3.65601811513444619e-02,
	  -5.77022356828701954e-02, +8.89025706043831826e-02, -1.42522749243161412e-01, +2.99846244197338241e-01,
	  +8.71709067807070803e-01, -1.07592421510800021e-01, +3.01913049175578201e-02, -3.03156140885641997e-03,
	  -7.61972906580422535e-03, +1.05858663948725489e-02, -9.81243829782979526e-03, +7.53450026617405975e-03,
	  -5.04471300665879866e-03, +2.98681921176491803e-03, -1.56182218154665622e-03, +7.12984293786424065e-04,
	  -2.77733663280535048e-04, +8.86525878861153761e-05, -2.14786125909730179e-05, +3.30310859560316001e-06, },
	/* phase 4 */
	{ +1.17851462731566614e-06, -7.68262952400937484e-06, +2.48724349129127293e-05, -4.98055229805003790e-05,
	  +4.47950528681123001e-05, +1.10946145840556758e-04, -6.88412012941306346e-04, +2.17875350273631077e-03,
	  -5.34075991186366578e-03, +1.12059155418507451e-02, -2.10550063806513379e-02, +3.64650646027351172e-02,
	  -5.97422253289935923e-02, +9.58528367022432742e-02, -1.61775163611390022e-01, +3.74255635601258518e-01,
	  +8.31208754835524588e-01, -1.39565426518450636e-01, +5.03263865285565812e-02, -1.63109125072144721e-02,
	  +1.05183130350539641e-03, +5.14572916673770517e-03, -6.59575917250223957e-03, +5.76989915707471369e-03,
	  -4.16126807645906052e-03, +2.59142859500931071e-03, -1.40833160432191767e-03, +6.63892694637513870e-04,
	  -2.66148313107565009e-04, +8.72980833236483182e-05, -2.17131263042590746e-05, +3.40580959335791974e-06, },
	/* phase 5 */
	{ +1.56440358100355984e-06, -1.02267035714685449e-05, +3.55253959354717488e-05, -8.39046207448126632e-05,
	  +1.34660284648839454e-04, -9.17711554582200674e-05, -2.88215862613358054e-04, +1.47920813393584405e-03,
	  -4.25550487584624038e-03, +9.72785835265431963e-03, -1.93589818524855170e-02, +3.50667333386926947e-02,
	  -5.97850041167495155e-02, +9.98578518150650962e-02, -1.76918143783013848e-01, +4.49161397819485553e-01,
	  +7.82191393633337562e-01, -1.63781833826151052e-01, +6.74394869112291268e-02, -2.82786667951536105e-02,
	  +9.20380547017636899e-03, -1.54863054737171756e-04, -3.35329323331430909e-03, +3.92704730919887779e-03,
	  -3.20065667746006295e-03, +2.13923762706155502e-03, -1.22001204544578486e-03, +5.96526526342920746e-04,
	  -2.46374768486010284e-04, +8.28814225072550614e-05, -2.10415978999316562e-05, +3.32274734830421166e-06, },
	/* phase 6 */
	{ +1.97469347268934035e-06, -1.28290212323754281e-05, +4.64488329537415573e-05, -1.19382468665746758e-04,
	  +2.30053164651573545e-04, -3.12213741412338249e-04, +1.59716992352903098e-04, +6.67636089047172403e-04,
	  -2.93510499987552787e-03, +7.79968680985324882e-03, -1.68647238674164840e-02, +3.23377756652374754e-02,
	  -5.77100708783231178e-02, +1.00584198629252178e-01, -1.87081658938854506e-01, +5.23163291799515706e-01,
	  +7.25668440163987793e-01, -1.80311725987448157e-01, +8.11811742123405428e-02, -3.86137076975041696e-02,
	  +1.65915958406056274e-02, -5.14467851362195461e-03, -1.96932875233982516e-04, +2.07421535451181478e-03,
	  -2.20161559464639211e-03, +1.65056222977243193e-03, -1.00663780450764768e-03, +5.15160520535456611e-04,
	  -2.20093720898226566e-04, +7.59881186957002462e-05, -1.96406176072679822e-05, +3.09760610877796876e-06, },
	/* phase 7 */
	{ +2.38697230688656725e-06, -1.53653559865875178e-05, +5.71880392403290667e-05, -1.54925091017102910e-04,
	  +3.27772782023702948e-04, -5.43595272341600134e-04, +6.42656648518477439e-04, -2.34705027997320889e-04,
	  -1.41100067980963039e-03, +5.46152671817030227e-03, -1.36125860839719469e-02, +2.82966624135735922e-02,
	  -5.34641024308652110e-02, +9.77881762051997366e-02, -1.91473347114188425e-01, +5.94842402437157247e-01,
	  +6.62795322203526771e-01, -1.89404105106531689e-01, +9.13292898154076521e-02, -4.70666619865635835e-02,
	  +2.30097347384734946e-02, -9.67279008212974627e-03, +2.77116539032389421e-03, +2.75529073119829958e-04,
	  -1.20142698860237879e-03, +1.14538310594639449e-03, -7.78009623753663057e-04, +4.24152525478563292e-04,
	  -1.89039064563435955e-04, +6.72240036750521708e-05, -1.76902070519078608e-05, +2.77268721675408203e-06, },
	/* phase 8 */
	{ +2.77268721675408203e-06, -1.76902070519078608e-05, +6.72240036750521708e-05, -1.89039064563435955e-04,
	  +4.24152525478563292e-04, -7.78009623753663057e-04, +1.14538310594639449e-03, -1.20142698860237879e-03,
	  +2.75529073119829958e-04, +2.77116539032389421e-03, -9.67279008212974627e-03, +2.30097347384734946e-02,
	  -4.70666619865635835e-02, +9.13292898154076521e-02, -1.89404105106531689e-01, +6.62795322203526771e-01,
	  +5.94842402437157247e-01, -1.91473347114188425e-01, +9.77881762051997366e-02, -5.34641024308652110e-02,
	  +2.82966624135735922e-02, -1.36125860839719469e-02, +5.46152671817030227e-03, -1.41100067980963039e-03,
	  -2.34705027997320889e-04, +6.42656648518477439e-04, -5.43595272341600134e-04, +3.27772782023702948e-04,
	  -1.54925091017102910e-04, +5.71880392403290667e-05, -1.53653559865875178e-05, +2.38697230688656725e-06, },
	/* phase 9 */
	{ +3.09760610877796876e-06, -1.96406176072679822e-05, +7.59881186957002462e-05, -2.20093720898226566e-04,
	  +5.15160520535456611e-04, -1.00663780450764768e-03, +1.65056222977243193e-03, -2.20161559464639211e-03,
	  +2.07421535451181478e-03, -1.96932875233982516e-04, -5.14467851362195461e-03, +1.65915958406056274e-02,
	  -3.86137076975041696e-02, +8.11811742123405428e-02, -1.80311725987448157e-01, +7.25668440163987793e-01,
	  +5.23163291799515706e-01, -1.87081658938854506e-01, +1.00584198629252178e-01, -5.77100708783231178e-02,
	  +3.23377756652374754e-02, -1.68647238674164840e-02, +7.79968680985324882e-03, -2.93510499987552787e-03,
	  +6.67636089047172403e-04, +1.59716992352903098e-04, -3.12213741412338249e-04, +2.30053164651573545e-04,
	  -1.19382468665746758e-04, +4.64488329537415573e-05, -1.28290212323754281e-05, +1.97469347268934035e-06, },
	/* phase 10 */
	{ +3.32274734830421166e-06, -2.10415978999316562e-05, +8.28814225072550614e-05, -2.46374768486010284e-04,
	  +5.96526526342920746e-04, -1.22001204544578486e-03, +2.13923762706155502e-03, -3.20065667746006295e-03,
	  +3.92704730919887779e-03, -3.35329323331430909e-03, -1.54863054737171756e-04, +9.20380547017636899e-03,
	  -2.82786667951536105e-02, +6.74394869112291268e-02, -1.63781833826151052e-01, +7.82191393633337562e-01,
	  +4.49161397819485553e-01, -1.76918143783013848e-01, +9.98578518150650962e-02, -5.97850041167495155e-02,
	  +3.50667333386926947e-02, -1.93589818524855170e-02, +9.72785835265431963e-03, -4.25550487584624038e-03,
	  +1.47920813393584405e-03, -2.88215862613358054e-04, -9.17711554582200674e-05, +1.34660284648839454e-04,
	  -8.39046207448126632e-05, +3.55253959354717488e-05, -1.02267035714685449e-05, +1.56440358100355984e-06, },
	/* phase 11 */
	{ +3.40580959335791974e-06, -2.17131263042590746e-05, +8.72980833236483182e-05, -2.66148313107565009e-04,
	  +6.63892694637513870e-04, -1.40833160432191767e-03, +2.59142859500931071e-03, -4.16126807645906052e-03,
	  +5.76989915707471369e-03, -6.59575917250223957e-03, +5.14572916673770517e-03, +1.05183130350539641e-03,
	  -1.63109125072144721e-02, +5.03263865285565812e-02, -1.39565426518450636e-01, +8.31208754835524588e-01,
	  +3.74255635601258518e-01, -1.61775163611390022e-01, +9.58528367022432742e-02, -5.97422253289935923e-02,
	  +3.64650646027351172e-02, -2.10550063806513379e-02, +1.12059155418507451e-02, -5.34075991186366578e-03,
	  +2.17875350273631077e-03, -6.88412012941306346e-04, +1.10946145840556758e-04, +4.47950528681123001e-05,
	  -4.98055229805003790e-05, +2.48724349129127293e-05, -7.68262952400937484e-06, +1.17851462731566614e-06, },
	/* phase 12 */
	{ +3.30310859560316001e-06, -2.14786125909730179e-05, +8.86525878861153761e-05, -2.77733663280535048e-04,
	  +7.12984293786424065e-04, -1.56182218154665622e-03, +2.98681921176491803e-03, -5.04471300665879866e-03,
	  +7.53450026617405975e-03, -9.81243829782979526e-03, +1.05858663948725489e-02, -7.61972906580422535e-03,
	  -3.03156140885641997e-03, +3.01913049175578201e-02, -1.07592421510800021e-01, +8.71709067807070803e-01,
	  +2.99846244197338241e-01, -1.42522749243161412e-01, +8.89025706043831826e-02, -5.77022356828701954e-02,
	  +3.65601811513444619e-02, -2.19419883099599838e-02, +1.22117024890106493e-02, -6.16971333509590443e-03,
	  +2.75052232785120889e-03, -1.03088692719296667e-03, +2.90418542206939473e-04, -3.68799832657953401e-05,
	  -1.81893688771127625e-05, +1.48702407527813701e-05, -5.29748633904858601e-06, +8.33252056086746835e-07, },
	/* phase 13 */
	{ +2.97199787604816103e-06, -2.01746120233491894e-05, +8.64098456164380838e-05, -2.79582721645488003e-04,
	  +7.39795239924908839e-04, -1.67112829106305649e-03, +3.30551875235304132e-03, -5.81216129946867700e-03,
	  +9.15069668284228775e-03, -1.28851935703063938e-02, +1.59789948473352747e-02, -1.65335796397600050e-02,
	  +1.11734043163622034e-02, +7.50782819677348230e-03, -6.79807035242710744e-02, +9.02850420266162468e-01,
	  +2.27281638248904905e-01, -1.20081845680832119e-01, +7.94146554042998998e-02, -5.38451015428494523e-02,
	  +3.54219436008549604e-02, -2.20373396659370689e-02, +1.27406949462928621e-02, -6.73154362299371607e-03,
	  +3.18442461158771053e-03, -1.30853613395380108e-03, +4.42479799425374385e-04, -1.08285429792171793e-04,
	  +1.00679820256261042e-05, +5.81902961819810933e-06, -3.14759256398561512e-06, +5.38958652132745187e-07, },
	/* phase 14 */
	{ +2.37371672136199670e-06, -1.76614805540458895e-05, +8.01171751606922442e-05, -2.70363211971824397e-04,
	  +7.40782114113007289e-04, -1.72772559816036634e-03, +3.52886913493476520e-03, -6.42615675512466910e-03,
	  +1.05489379470937329e-02, -1.56935838023534585e-02, +2.11290673254453110e-02, -2.53876726254017027e-02,
	  +2.58625947872180226e-02, -1.71333823111622686e-02, -2.10402952286613404e-02, +9.23981829283398781e-01,
	  +1.57827184507585677e-01, -9.53971928586008489e-02, +6.78538709069775592e-02, -4.84012813739312445e-02,
	  +3.31579739740803306e-02, -2.13844716984531481e-02, +1.28050659955456381e-02, -7.02544660546436210e-03,
	  +3.47596801216177231e-03, -1.51715391617144300e-03, +5.64345510648012649e-04, -1.67942970870100242e-04,
	  +3.43263311803131298e-05, -2.06258286374106165e-06, -1.28533481673772682e-06, +3.00672929586365958e-07, },
	/* phase 15 */
	{ +1.47657338138237595e-06, -1.38345652090794398e-05, +6.94369144800050984e-05, -2.49042503366221038e-04,
	  +7.13059339589778435e-04, -1.72433823851031128e-03, +3.64027125212970475e-03, -6.85214165383429569e-03,
	  +1.16629100398877219e-02, -1.81191346073100341e-02, +2.58370541957508131e-02, -3.38644710320998160e-02,
	  +4.05510644669190984e-02, -4.30355762190085395e-02, +3.27275923093596349e-02, +9.34659835569005670e-01,
	  +9.26367266878741630e-02, -6.94106300051381270e-02, +5.47242835760801336e-02, -4.16412715167827421e-02,
	  +2.99079391918543984e-02, -2.00498015340008634e-02, +1.24322245327915110e-02, -7.05998369580099119e-03,
	  +3.62599902413910909e-03, -1.65534458315876975e-03, +6.54587405761760223e-04, -2.14968397675198821e-04,
	  +5.41782023281094688e-05, -8.63543507831690459e-06, +2.59335444498408197e-07, +3.52203560958273242e-07, },
};

enum mod_interp_q { MOD_INTERP_Q0 = 0, MOD_INTERP_Q1, MOD_INTERP_Q2 };
static const int mod_interp_n[] = {
	[MOD_INTERP_Q0] = 3,
	[MOD_INTERP_Q1] = LENGTH(mod_flt_q1[0]),
	[MOD_INTERP_Q2] = LENGTH(mod_flt_q2[0]),
};
#define MOD_QUALITY_DEFAULT MOD_INTERP_Q1
#define MOD_BW_DEFAULT      1.0

static inline sample_t mod_interp(const sample_t *y, sample_t t, enum mod_interp_q q)
{
	sample_t c[4] = {0}, z[4], t_os, a;
	switch (q) {
	case MOD_INTERP_Q0:  /* cubic Hermite */
		c[0] = y[-1];
		c[1] = (1.0/2.0)*(y[-2]-y[0]);
		c[2] = y[0] - (5.0/2.0)*y[-1] + 2.0*y[-2] - (1.0/2.0)*y[-3];
		c[3] = (1.0/2.0)*(y[-3]-y[0]) + (3.0/2.0)*(y[-1]-y[-2]);
		break;
	case MOD_INTERP_Q1:  /* polyphase FIR + cubic B-spline */
		t_os = t*LENGTH(mod_flt_q1);
		for (int i = 0, ph = (int) t_os; i < LENGTH(z); ++i) {
			const sample_t *flt = mod_flt_q1[ph]+15;
			const sample_t x[4] = {
				y[  0]*flt[  0] + y[ -1]*flt[ -1] + y[ -2]*flt[ -2] + y[ -3]*flt[ -3],
				y[ -4]*flt[ -4] + y[ -5]*flt[ -5] + y[ -6]*flt[ -6] + y[ -7]*flt[ -7],
				y[ -8]*flt[ -8] + y[ -9]*flt[ -9] + y[-10]*flt[-10] + y[-11]*flt[-11],
				y[-12]*flt[-12] + y[-13]*flt[-13] + y[-14]*flt[-14] + y[-15]*flt[-15],
			};
			z[i] = x[0]+x[1]+x[2]+x[3];
			if (ph < LENGTH(mod_flt_q1)-1) ++ph;
			else { ph = 0; --y; }
		}
		goto cubic_b_spline;
	case MOD_INTERP_Q2:  /* polyphase FIR + cubic B-spline */
		t_os = t*LENGTH(mod_flt_q2);
		for (int i = 0, ph = (int) t_os; i < LENGTH(z); ++i) {
			const sample_t *flt = mod_flt_q2[ph]+31;
			const sample_t x[8] = {
				y[  0]*flt[  0] + y[ -1]*flt[ -1] + y[ -2]*flt[ -2] + y[ -3]*flt[ -3],
				y[ -4]*flt[ -4] + y[ -5]*flt[ -5] + y[ -6]*flt[ -6] + y[ -7]*flt[ -7],
				y[ -8]*flt[ -8] + y[ -9]*flt[ -9] + y[-10]*flt[-10] + y[-11]*flt[-11],
				y[-12]*flt[-12] + y[-13]*flt[-13] + y[-14]*flt[-14] + y[-15]*flt[-15],
				y[-16]*flt[-16] + y[-17]*flt[-17] + y[-18]*flt[-18] + y[-19]*flt[-19],
				y[-20]*flt[-20] + y[-21]*flt[-21] + y[-22]*flt[-22] + y[-23]*flt[-23],
				y[-24]*flt[-24] + y[-25]*flt[-25] + y[-26]*flt[-26] + y[-27]*flt[-27],
				y[-28]*flt[-28] + y[-29]*flt[-29] + y[-30]*flt[-30] + y[-31]*flt[-31],
			};
			z[i] = x[0]+x[1]+x[2]+x[3]+x[4]+x[5]+x[6]+x[7];
			if (ph < LENGTH(mod_flt_q2)-1) ++ph;
			else { ph = 0; --y; }
		}
		cubic_b_spline:
		t = t_os-((int) t_os);
		a = z[0]+z[2];
		c[0] = (1.0/6.0)*a + (2.0/3.0)*z[1];
		c[1] = (1.0/2.0)*(z[2]-z[0]);
		c[2] = (1.0/2.0)*a - z[1];
		c[3] = (1.0/2.0)*(z[1]-z[2]) + (1.0/6.0)*(z[3]-z[0]);
		break;
	}
	return ((c[3]*t+c[2])*t+c[1])*t+c[0];
}

struct mod_noise_state {
	double c[4], y[4];
	double t, step;
	uint32_t *s0, *s1;
};

#define MOD_NOISE_N 6
#define MOD_NOISE_SCALE (0.77/MOD_NOISE_N/PM_RAND_MAX)
static void mod_noise_next(struct mod_noise_state *s)
{
	double *c = s->c, *y = s->y;
	memmove(y, y+1, sizeof(double)*3);
	y[3] = 0.0;
	for (int i = 0; i < MOD_NOISE_N; ++i) {
		int32_t n1 = pm_rand1_r(s->s0);
		int32_t n2 = pm_rand2_r(s->s1);
		y[3] += (n1 - n2) * MOD_NOISE_SCALE;
	}
	/* cubic B-spline with +0.5 offset */
	const double a = y[0]+y[2];
	c[0] = (1.0/6.0)*a + (2.0/3.0)*y[1] + 0.5;
	c[1] = (1.0/2.0)*(y[2]-y[0]);
	c[2] = (1.0/2.0)*a - y[1];
	c[3] = (1.0/2.0)*(y[1]-y[2]) + (1.0/6.0)*(y[3]-y[0]);
}

static inline double mod_noise(struct mod_noise_state *s)
{
	const double *c = s->c, t = s->t;
	const double z = ((c[3]*t+c[2])*t+c[1])*t+c[0];
	s->t += s->step;
	if (s->t >= 1.0) {
		s->t -= 1.0;
		mod_noise_next(s);
	}
	if (z > 1.0) return 1.0;
	if (z < 0.0) return 0.0;
	return z;
}

static void mod_noise_state_init(struct mod_noise_state *s, double fs, double fc, uint32_t seeds[2])
{
	s->s0 = &seeds[0];
	s->s1 = &seeds[1];
	s->c[0] = 0.5;  /* start at midpoint */
	s->step = 2.0*fc/fs;
}

struct mod_channel_state {
	sample_t *buf;
	struct mod_noise_state ns;
	ssize_t len, n, p;
	double depth;
	uint32_t seeds[2];
	enum mod_interp_q q;
};

struct mod_state {
	struct mod_channel_state *cs;
	uint32_t seeds[2];
};

static inline void mod_channel_run(struct mod_channel_state *cs, ssize_t frames, sample_t *ibuf_p, int stride)
{
	while (frames-- > 0) {
		const sample_t mod = mod_noise(&cs->ns) * cs->depth;
		ssize_t d_int = (ssize_t) mod;
		const sample_t d_frac = mod - d_int;

		const sample_t s = *ibuf_p;
		const ssize_t op = cs->p+cs->n, dup_p = op-cs->len;
		ssize_t yp = op-d_int;
		if (yp < cs->n) yp += cs->len;
		cs->buf[op] = s;
		*ibuf_p = mod_interp(&cs->buf[yp], d_frac, cs->q);
		if (dup_p >= 0) cs->buf[dup_p] = s;

		ibuf_p += stride;
		cs->p = (cs->p+1 >= cs->len) ? 0 : cs->p+1;
	}
}

static sample_t * mod_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct mod_state *state = (struct mod_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->cs[k].buf) mod_channel_run(&state->cs[k], *frames, &ibuf[k], e->istream.channels);
	return ibuf;
}

static void mod_effect_reset(struct effect *e)
{
	struct mod_state *state = (struct mod_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		if (state->cs[k].buf) {
			memset(state->cs[k].buf, 0, (state->cs[k].len+state->cs[k].n)*sizeof(sample_t));
			state->cs[k].p = 0;
		}
	}
}

static void mod_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct mod_state *state = (struct mod_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->cs[k].buf) drain_samples[k] += state->cs[k].len;
}

static void mod_effect_destroy(struct effect *e)
{
	struct mod_state *state = (struct mod_state *) e->data;
	if (state->cs) {
		for (int k = 0; k < e->istream.channels; ++k)
			free(state->cs[k].buf);
		free(state->cs);
	}
	free(state);
}

static void mod_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct mod_state *state = (struct mod_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state->cs[k].buf) latency[k] += state->cs[k].len/2;
}

static struct effect * mod_effect_init(const char *name, const struct stream_info *istream, const char *channel_selector, double samples, double fc, int is_mono, int qual)
{
	static pthread_mutex_t rand_lock = PTHREAD_MUTEX_INITIALIZER;
	static uint32_t seed = 1;
	struct effect *e = NULL;
	struct mod_state *state = NULL;

	if (qual < 0 || qual > LENGTH(mod_interp_n)-1 || !mod_interp_n[qual]) {
		LOG_FMT(LL_ERROR, "%s: error: invalid quality: %d", name, qual);
		goto fail;
	}

	e = calloc(1, sizeof(struct effect));
	if (check_alloc(name, e)) goto fail;
	e->name = name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->run = mod_effect_run;
	e->reset = mod_effect_reset;
	e->plot = effect_plot_noop;
	e->drain_samples = mod_effect_drain_samples;
	e->destroy = mod_effect_destroy;
	e->channel_offsets = mod_effect_channel_offsets;

	e->data = state = calloc(1, sizeof(struct mod_state));
	if (check_alloc(name, state)) goto fail;
	state->cs = calloc(e->istream.channels, sizeof(struct mod_channel_state));
	if (check_alloc(name, state->cs)) goto fail;
	pthread_mutex_lock(&rand_lock);
	state->seeds[0] = pm_rand2_r(&seed);
	state->seeds[1] = pm_rand1_r(&seed);
	pthread_mutex_unlock(&rand_lock);
	for (int k = 0; k < e->istream.channels; ++k) {
		if (GET_BIT(channel_selector, k)) {
			struct mod_channel_state *cs = &state->cs[k];
			cs->q = qual;
			cs->n = mod_interp_n[cs->q];
			cs->len = lrint(ceil(samples))*2+cs->n;
			cs->buf = calloc(state->cs[k].len+cs->n, sizeof(sample_t));
			if (check_alloc(name, cs->buf)) goto fail;
			if (is_mono) memcpy(cs->seeds, state->seeds, sizeof(cs->seeds));
			mod_noise_state_init(&cs->ns, istream->fs, fc, (is_mono) ? cs->seeds : state->seeds);
			cs->depth = samples*2.0;
		}
	}
	return e;

	fail:
	if (state) mod_effect_destroy(e);
	free(e);
	return NULL;
}

struct effect * delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	char *endptr;
	const char *mod_arg = NULL;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int opt, do_frac = 0, fd_ap_n = 0;
	int mod_mono = 0, mod_qual = MOD_QUALITY_DEFAULT;
	double mod_bw = MOD_BW_DEFAULT;

	while ((opt = dsp_getopt(&g, argc-1, argv, "f::m:M:b:q:")) != -1) {
		switch (opt) {
		case 'f':
			do_frac = 1;
			if (g.arg) {
				fd_ap_n = strtol(g.arg, &endptr, 10);
				CHECK_ENDPTR(g.arg, endptr, "order", return NULL);
				CHECK_RANGE(fd_ap_n > 0 && fd_ap_n <= 50, "order", return NULL);
			}
			break;
		case 'm':
		case 'M':
			mod_arg = g.arg;
			mod_mono = (opt=='M');
			break;
		case 'b':
			mod_bw = parse_freq(g.arg, &endptr);
			CHECK_ENDPTR(g.arg, endptr, "modulation bandwidth", return NULL);
			CHECK_RANGE(mod_bw > 0.0 && mod_bw < istream->fs/2.0, "modulation bandwidth", return NULL);
			break;
		case 'q':
			mod_qual = strtol(g.arg, &endptr, 10);
			CHECK_ENDPTR(g.arg, endptr, "quality", return NULL);
			break;
		default:
			dsp_getopt_print_error(&g, opt, argv[0]);
			goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}
	double samples = parse_len_frac(argv[g.ind], istream->fs, &endptr);
	CHECK_ENDPTR(argv[g.ind], endptr, "delay", return NULL);

	double mod_samples = 0.0;
	if (mod_arg) {
		double v = strtod(mod_arg, &endptr);
		if (*endptr == '%') {
			mod_samples = samples*(v/100.0);
			++endptr;
		}
		else mod_samples = parse_len_frac(mod_arg, istream->fs, &endptr);
		CHECK_ENDPTR(mod_arg, endptr, "modulation depth", return NULL);
	}

	struct effect *e = NULL;
	if (do_frac) e = delay_effect_init_frac(ei->name, istream, channel_selector, samples, fd_ap_n);
	else {
		ssize_t samples_int = lrint(samples);
		if (fabs(samples-samples_int) >= DBL_EPSILON) {
			LOG_FMT(LL_VERBOSE, "%s: info: delay rounded to %gs (%ld sample%s)", ei->name,
				(double) samples_int / istream->fs, samples_int, (labs(samples_int)==1)?"":"s");
		}
		e = delay_effect_init_int(ei->name, istream, channel_selector, samples_int);
	}
	if (mod_samples > 0.0) {
		if (!e) return NULL;
		struct effect *e_mod = mod_effect_init(ei->name, istream, channel_selector, mod_samples, mod_bw, mod_mono, mod_qual);
		if (!e_mod) {
			destroy_effect(e);
			return NULL;
		}
		effect_list_append(e, e_mod);
	}
	return e;
}
