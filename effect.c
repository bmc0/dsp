#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "effect.h"
#include "util.h"

#include "effects/biquad.h"
#include "effects/gain.h"
#include "effects/crossfeed.h"
#include "effects/crossfeed_hrtf.h"
#include "effects/mono.h"

static struct effect_info effects[] = {
	{ "lowpass_1",          "lowpass_1 f0[k]",                      biquad_effect_init },
	{ "highpass_1",         "highpass_1 f0[k]",                     biquad_effect_init },
	{ "lowpass",            "lowpass f0[k] q",                      biquad_effect_init },
	{ "highpass",           "highpass f0[k] q",                     biquad_effect_init },
	{ "bandpass_skirt",     "bandpass_skirt f0[k] q",               biquad_effect_init },
	{ "bandpass_peak",      "bandpass_peak f0[k] q",                biquad_effect_init },
	{ "notch",              "notch f0[k] q",                        biquad_effect_init },
	{ "allpass",            "allpass f0[k] q",                      biquad_effect_init },
	{ "eq",                 "eq f0[k] q gain",                      biquad_effect_init },
	{ "lowshelf",           "lowshelf f0[k] q gain",                biquad_effect_init },
	{ "highshelf",          "highshelf f0[k] q gain",               biquad_effect_init },
	{ "linkwitz_transform", "linkwitz_transform fz[k] qz fp[k] qp", biquad_effect_init },
	{ "biquad",             "biquad b0 b1 b2 a0 a1 a2",             biquad_effect_init },
	{ "gain",               "gain [channel] gain",                  gain_effect_init },
	{ "crossfeed",          "crossfeed f0[k] separation",           crossfeed_effect_init },
	{ "crossfeed_hrtf",     "crossfeed_hrtf left_fir right_fir",    crossfeed_hrtf_effect_init },
	{ "mono",               "mono",                                 mono_effect_init },
};

struct effect_info * get_effect_info(const char *name)
{
	int i;
	for (i = 0; i < LENGTH(effects); ++i)
		if (strcmp(name, effects[i].name) == 0)
			return &effects[i];
	return NULL;
}

struct effect * init_effect(struct effect_info *ei, struct stream_info *istream, int argc, char **argv)
{
	return ei->init(ei, istream, argc, argv);
}

void destroy_effect(struct effect *e)
{
	e->destroy(e);
	free(e);
}

void append_effect(struct effects_chain *chain, struct effect *e)
{
	if (chain->tail == NULL)
		chain->head = e;
	else
		chain->tail->next = e;
	chain->tail = e;
	e->next = NULL;
}

double get_effects_chain_max_ratio(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	double ratio = 1.0, max_ratio = 1.0;
	while (e != NULL) {
		ratio *= e->ratio;
		max_ratio = (ratio > max_ratio) ? ratio : max_ratio;
		e = e->next;
	}
	return max_ratio;
}

sample_t * run_effects_chain(struct effects_chain *chain, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	sample_t *ibuf = buf1, *obuf = buf2, *tmp;
	struct effect *e = chain->head;
	while (e != NULL && *frames > 0) {
		e->run(e, frames, ibuf, obuf);
		tmp = ibuf;
		ibuf = obuf;
		obuf = tmp;
		e = e->next;
	}
	return ibuf;
}

void reset_effects_chain(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		e->reset(e);
		e = e->next;
	}
}

void plot_effects_chain(struct effects_chain *chain)
{
	int i = 0, k;
	struct effect *e = chain->head;
	if (e == NULL) {
		LOG(LL_ERROR, "dsp: error: effects chain empty\n");
		return;
	}
	printf(
		"set xlabel 'frequency (Hz)'\n"
		"set ylabel 'amplitude (dB)'\n"
		"set logscale x\n"
		"set samples 500\n"
		"set grid xtics ytics\n"
		"set key off\n"
		"Fs=%d\n"
		"o=2*pi/Fs\n"
		, e->istream.fs
	);
	while (e != NULL) {
		e->plot(e, i++);
		e = e->next;
	}
	printf("Hsum(f)=");
	for (k = 0; k < i; ++k)
		printf("H%d(f) + ", k);
	printf("0\nplot [f=10:Fs/2] [-30:20] Hsum(f)\n");
}

sample_t * drain_effects_chain(struct effects_chain *chain, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	ssize_t dframes = 0;
	sample_t *ibuf = buf1, *obuf = buf2, *tmp;
	struct effect *e = chain->head;
	while (e != NULL && dframes == 0) {
		dframes = *frames;
		e->drain(e, &dframes, ibuf);
		e = e->next;
	}
	*frames = dframes;
	while (e != NULL && *frames > 0) {
		e->run(e, frames, ibuf, obuf);
		tmp = ibuf;
		ibuf = obuf;
		obuf = tmp;
		e = e->next;
	}
	return ibuf;
}

void destroy_effects_chain(struct effects_chain *chain)
{
	struct effect *e;
	while (chain->head != NULL) {
		e = chain->head;
		chain->head = e->next;
		destroy_effect(e);
	}
}

void print_all_effects(void)
{
	int i;
	fprintf(stderr, "effects:\n");
	for (i = 0; i < LENGTH(effects); ++i)
		fprintf(stderr, "  %s\n", effects[i].usage);
}
