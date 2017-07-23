#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "effect.h"
#include "util.h"

#include "biquad.h"
#include "gain.h"
#include "crossfeed.h"
#include "remix.h"
#include "delay.h"
#include "resample.h"
#include "fir.h"
#include "zita_convolver.h"
#include "noise.h"
#include "compress.h"
#include "reverb.h"
#include "g2reverb.h"
#include "stats.h"

static struct effect_info effects[] = {
	{ "lowpass_1",          "lowpass_1 f0[k]",                       biquad_effect_init },
	{ "highpass_1",         "highpass_1 f0[k]",                      biquad_effect_init },
	{ "lowpass",            "lowpass f0[k] width[q|o|h|k]",          biquad_effect_init },
	{ "highpass",           "highpass f0[k] width[q|o|h|k]",         biquad_effect_init },
	{ "bandpass_skirt",     "bandpass_skirt f0[k] width[q|o|h|k]",   biquad_effect_init },
	{ "bandpass_peak",      "bandpass_peak f0[k] width[q|o|h|k]",    biquad_effect_init },
	{ "notch",              "notch f0[k] width[q|o|h|k]",            biquad_effect_init },
	{ "allpass",            "allpass f0[k] width[q|o|h|k]",          biquad_effect_init },
	{ "eq",                 "eq f0[k] width[q|o|h|k] gain",          biquad_effect_init },
	{ "lowshelf",           "lowshelf f0[k] width[q|s|o|h|k] gain",  biquad_effect_init },
	{ "highshelf",          "highshelf f0[k] width[q|s|o|h|k] gain", biquad_effect_init },
	{ "linkwitz_transform", "linkwitz_transform fz[k] qz fp[k] qp",  biquad_effect_init },
	{ "deemph",             "deemph",                                biquad_effect_init },
	{ "biquad",             "biquad b0 b1 b2 a0 a1 a2",              biquad_effect_init },
	{ "gain",               "gain [channel] gain",                   gain_effect_init },
	{ "mult",               "mult [channel] multiplier",             gain_effect_init },
	{ "crossfeed",          "crossfeed f0[k] separation",            crossfeed_effect_init },
	{ "remix",              "remix channel_selector|. ...",          remix_effect_init },
	{ "delay",              "delay seconds",                         delay_effect_init },
#ifdef __HAVE_FFTW3__
#ifndef __SYMMETRIC_IO__
	{ "resample",           "resample [bandwidth] fs[k]",            resample_effect_init },
#endif
	{ "fir",                "fir [~/]impulse_path",                  fir_effect_init },
#endif
#ifdef __HAVE_ZITA_CONVOLVER__
	{ "zita_convolver",     "zita_convolver [min_part_len [max_part_len]] [~/]impulse_path", zita_convolver_effect_init },
#endif
	{ "noise",              "noise level",                           noise_effect_init },
	{ "compress",           "compress thresh ratio attack release",  compress_effect_init },
#ifdef __ENABLE_GPL_CODE__
	{ "reverb",             "reverb [-w] [reverberance [hf_damping [room_scale [stereo_depth [pre_delay [wet_gain]]]]]]", reverb_effect_init },
	{ "g2reverb",           "g2reverb [-w] [room_size [reverb_time [input_bandwidth [damping [dry_level [reflection_level [tail_level]]]]]]]", g2reverb_effect_init },
#endif
	{ "stats",              "stats [ref_level]",                     stats_effect_init },
};

struct effect_info * get_effect_info(const char *name)
{
	int i;
	for (i = 0; i < LENGTH(effects); ++i)
		if (strcmp(name, effects[i].name) == 0)
			return &effects[i];
	return NULL;
}

void destroy_effect(struct effect *e)
{
	if (e->destroy != NULL) e->destroy(e);
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

int build_effects_chain(int argc, char **argv, struct effects_chain *chain, struct stream_info *stream, char *channel_selector, const char *dir)
{
	int i = 1, k = 0, j, last_selector_index = -1, old_stream_channels, allow_fail = 0;
	char *tmp_channel_selector;
	struct effect_info *ei = NULL;
	struct effect *e = NULL;

	if (channel_selector == NULL) {
		channel_selector = NEW_SELECTOR(stream->channels);
		SET_SELECTOR(channel_selector, stream->channels);
	}
	else {
		tmp_channel_selector = NEW_SELECTOR(stream->channels);
		COPY_SELECTOR(tmp_channel_selector, channel_selector, stream->channels);
		channel_selector = tmp_channel_selector;
	}

	while (k < argc) {
		if (argv[k][0] == '!' && argv[k][1] == '\0') {
			allow_fail = 1;
			i = ++k + 1;
			continue;
		}
		if (argv[k][0] == ':') {
			if (parse_selector(&argv[k][1], channel_selector, stream->channels))
				goto fail;
			last_selector_index = k++;
			i = k + 1;
			continue;
		}
		if (argv[k][0] == '@') {
			old_stream_channels = stream->channels;
			if (build_effects_chain_from_file(chain, stream, channel_selector, dir, &argv[k][1]))
				goto fail;
			if (stream->channels != old_stream_channels) {
				tmp_channel_selector = NEW_SELECTOR(stream->channels);
				if (last_selector_index == -1)
					SET_SELECTOR(tmp_channel_selector, stream->channels);
				else if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, stream->channels)) {
					free(tmp_channel_selector);
					goto fail;
				}
				free(channel_selector);
				channel_selector = tmp_channel_selector;
			}
			i = ++k + 1;
			continue;
		}
		ei = get_effect_info(argv[k]);
		/* Find end of argument list */
		for (; i < argc && get_effect_info(argv[i]) == NULL && argv[i][0] != ':' && argv[i][0] != '@' && !(argv[i][0] == '!' && argv[i][1] == '\0'); ++i);
		if (ei == NULL) {
			if (allow_fail)
				LOG(LL_VERBOSE, "dsp: warning: no such effect: %s\n", argv[k]);
			else {
				LOG(LL_ERROR, "dsp: error: no such effect: %s\n", argv[k]);
				goto fail;
			}
		}
		else {
			if (LOGLEVEL(LL_VERBOSE)) {
				fprintf(stderr, "dsp: effect:");
				for (j = 0; j < i - k; ++j)
					fprintf(stderr, " %s", argv[k + j]);
				fprintf(stderr, "; channels=%d [", stream->channels);
				print_selector(channel_selector, stream->channels);
				fprintf(stderr, "] fs=%d\n", stream->fs);
			}
			e = ei->init(ei, stream, channel_selector, dir, i - k, &argv[k]);
			if (e == NULL) {
				if (allow_fail)
					LOG(LL_VERBOSE, "dsp: warning: failed to initialize non-essential effect: %s\n", argv[k]);
				else {
					LOG(LL_ERROR, "dsp: error: failed to initialize effect: %s\n", argv[k]);
					goto fail;
				}
			}
			else {
				append_effect(chain, e);
				if (e->ostream.channels != stream->channels) {
					tmp_channel_selector = NEW_SELECTOR(e->ostream.channels);
					if (last_selector_index == -1)
						SET_SELECTOR(tmp_channel_selector, e->ostream.channels);
					else if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, e->ostream.channels)) {
						free(tmp_channel_selector);
						goto fail;
					}
					free(channel_selector);
					channel_selector = tmp_channel_selector;
				}
				*stream = e->ostream;
			}
		}
		allow_fail = 0;
		k = i;
		i = k + 1;
	}
	free(channel_selector);
	return 0;

	fail:
	free(channel_selector);
	return 1;
}

int build_effects_chain_from_file(struct effects_chain *chain, struct stream_info *stream, char *channel_selector, const char *dir, const char *path)
{
	char **argv = NULL, *tmp, *d = NULL, *p, *c;
	int i, ret = 0, argc = 0;

	p = construct_full_path(dir, path);
	if (!(c = get_file_contents(p))) {
		LOG(LL_ERROR, "dsp: info: failed to load effects file: %s: %s\n", p, strerror(errno));
		goto fail;
	}
	if (gen_argv_from_string(c, &argc, &argv))
		goto fail;
	d = strdup(p);
	tmp = strrchr(d, '/');
	if (tmp == NULL) {
		free(d);
		d = strdup(".");
	}
	else
		*tmp = '\0';
	LOG(LL_VERBOSE, "dsp: info: begin effects file: %s\n", p);
	if (build_effects_chain(argc, argv, chain, stream, channel_selector, d))
		goto fail;
	LOG(LL_VERBOSE, "dsp: info: end effects file: %s\n", p);
	done:
	free(c);
	free(p);
	free(d);
	for (i = 0; i < argc; ++i)
		free(argv[i]);
	free(argv);
	return ret;

	fail:
	ret = 1;
	goto done;
}

double get_effects_chain_max_ratio(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	double ratio = 1.0, max_ratio = 1.0;
	while (e != NULL) {
		ratio *= e->worst_case_ratio;
		max_ratio = (ratio > max_ratio) ? ratio : max_ratio;
		e = e->next;
	}
	return max_ratio;
}

double get_effects_chain_total_ratio(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	double ratio = 1.0;
	while (e != NULL) {
		ratio *= e->ratio;
		e = e->next;
	}
	return ratio;
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

double get_effects_chain_delay(struct effects_chain *chain)
{
	double delay = 0.0;
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->delay != NULL) delay += (double) e->delay(e) / e->ostream.fs;
		e = e->next;
	}
	return delay;
}

void reset_effects_chain(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->reset != NULL) e->reset(e);
		e = e->next;
	}
}

void plot_effects_chain(struct effects_chain *chain, int input_fs)
{
	int i = 0, k, j, max_fs = -1, channels = -1;
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->plot == NULL) {
			LOG(LL_ERROR, "dsp: plot: error: effect '%s' does not support plotting\n", e->name);
			return;
		}
		if (channels == -1)
			channels = e->ostream.channels;
		else if (channels != e->ostream.channels) {
			LOG(LL_ERROR, "dsp: plot: error: number of channels cannot change: %s\n", e->name);
			return;
		}
		e = e->next;
	}
	printf(
		"set xlabel 'frequency (Hz)'\n"
		"set ylabel 'amplitude (dB)'\n"
		"set logscale x\n"
		"set samples 500\n"
		"set grid xtics ytics\n"
		"set key on\n"
	);
	e = chain->head;
	while (e != NULL) {
		e->plot(e, i++);
		max_fs = (e->ostream.fs > max_fs) ? e->ostream.fs : max_fs;
		e = e->next;
	}
	if (channels > 0) {
		for (k = 0; k < channels; ++k) {
			printf("Hsum%d(f)=H%d_%d(f)", k, k, 0);
			for (j = 1; j < i; ++j)
				printf("+H%d_%d(f)", k, j);
			putchar('\n');
		}
		printf("plot [10:%d/2] [-30:20] Hsum%d(x) title 'Channel %d'", (max_fs == -1) ? input_fs : max_fs, 0, 0);
		for (k = 1; k < channels; ++k)
			printf(", Hsum%d(x) title 'Channel %d'", k, k);
		puts("\npause mouse close");
	}
}

sample_t * drain_effects_chain(struct effects_chain *chain, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	ssize_t dframes = -1;
	sample_t *ibuf = buf1, *obuf = buf2, *tmp;
	double fratio = 1.0;
	struct effect *e = chain->head;
	while (e != NULL && dframes == -1) {
		dframes = *frames * fratio;
		if (e->drain != NULL) e->drain(e, &dframes, ibuf);
		else dframes = -1;
		fratio *= e->ratio * e->istream.channels / e->ostream.channels;
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
	chain->tail = NULL;
}

void print_all_effects(void)
{
	int i;
	fprintf(stdout, "Effects:\n");
	for (i = 0; i < LENGTH(effects); ++i)
		fprintf(stdout, "  %s\n", effects[i].usage);
}
