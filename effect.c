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
#include "st2ms.h"
#include "delay.h"
#include "resample.h"
#include "fir.h"
#include "fir_p.h"
#include "zita_convolver.h"
#include "noise.h"
#include "ladspa_host.h"
#include "stats.h"

static struct effect_info effects[] = {
	{ "lowpass_1",          "lowpass_1 f0[k]",                         biquad_effect_init,    BIQUAD_LOWPASS_1 },
	{ "highpass_1",         "highpass_1 f0[k]",                        biquad_effect_init,    BIQUAD_HIGHPASS_1 },
	{ "lowpass",            "lowpass f0[k] width[q|o|h|k]",            biquad_effect_init,    BIQUAD_LOWPASS },
	{ "highpass",           "highpass f0[k] width[q|o|h|k]",           biquad_effect_init,    BIQUAD_HIGHPASS },
	{ "bandpass_skirt",     "bandpass_skirt f0[k] width[q|o|h|k]",     biquad_effect_init,    BIQUAD_BANDPASS_SKIRT },
	{ "bandpass_peak",      "bandpass_peak f0[k] width[q|o|h|k]",      biquad_effect_init,    BIQUAD_BANDPASS_PEAK },
	{ "notch",              "notch f0[k] width[q|o|h|k]",              biquad_effect_init,    BIQUAD_NOTCH },
	{ "allpass",            "allpass f0[k] width[q|o|h|k]",            biquad_effect_init,    BIQUAD_ALLPASS },
	{ "eq",                 "eq f0[k] width[q|o|h|k] gain",            biquad_effect_init,    BIQUAD_PEAK },
	{ "lowshelf",           "lowshelf f0[k] width[q|s|d|o|h|k] gain",  biquad_effect_init,    BIQUAD_LOWSHELF },
	{ "highshelf",          "highshelf f0[k] width[q|s|d|o|h|k] gain", biquad_effect_init,    BIQUAD_HIGHSHELF },
	{ "linkwitz_transform", "linkwitz_transform fz[k] qz fp[k] qp",    biquad_effect_init,    BIQUAD_LINKWITZ_TRANSFORM },
	{ "deemph",             "deemph",                                  biquad_effect_init,    BIQUAD_DEEMPH },
	{ "biquad",             "biquad b0 b1 b2 a0 a1 a2",                biquad_effect_init,    BIQUAD_BIQUAD },
	{ "gain",               "gain [channel] gain",                     gain_effect_init,      GAIN_EFFECT_NUMBER_GAIN },
	{ "mult",               "mult [channel] multiplier",               gain_effect_init,      GAIN_EFFECT_NUMBER_MULT },
	{ "add",                "add [channel] value",                     gain_effect_init,      GAIN_EFFECT_NUMBER_ADD },
	{ "crossfeed",          "crossfeed f0[k] separation",              crossfeed_effect_init, 0 },
	{ "remix",              "remix channel_selector|. ...",            remix_effect_init,     0 },
	{ "st2ms",              "st2ms",                                   st2ms_effect_init,     ST2MS_EFFECT_NUMBER_ST2MS },
	{ "ms2st",              "ms2st",                                   st2ms_effect_init,     ST2MS_EFFECT_NUMBER_MS2ST },
	{ "delay",              "delay delay[s|m|S]",                      delay_effect_init,     0 },
#ifdef HAVE_FFTW3
#ifndef SYMMETRIC_IO
	{ "resample",           "resample [bandwidth] fs[k]",              resample_effect_init,  0 },
#endif
	{ "fir",                "fir [~/]impulse_path",                    fir_effect_init,       0 },
	{ "fir_p",              "fir_p [min_part_len [max_part_len]] [~/]impulse_path", fir_p_effect_init, 0 },
#endif
#ifdef HAVE_ZITA_CONVOLVER
	{ "zita_convolver",     "zita_convolver [min_part_len [max_part_len]] [~/]impulse_path", zita_convolver_effect_init, 0 },
#endif
	{ "noise",              "noise level",                             noise_effect_init,     0 },
#ifdef ENABLE_LADSPA_HOST
	{ "ladspa_host",        "ladspa_host module_path plugin_label [control ...]", ladspa_host_effect_init, 0 },
#endif
	{ "stats",              "stats [ref_level]",                       stats_effect_init,     0 },
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
	int i = 1, k = 0, j, last_selector_index = -1, old_stream_channels, allow_fail = 0, channels_changed = 0;
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
			if (channels_changed) {
				free(channel_selector);
				channel_selector = NEW_SELECTOR(stream->channels);
				channels_changed = 0;
			}
			if (parse_selector(&argv[k][1], channel_selector, stream->channels))
				goto fail;
			last_selector_index = k++;
			i = k + 1;
			continue;
		}
		if (channels_changed) {  /* re-parse the channel selector if the last effect changed the number of channels */
			tmp_channel_selector = NEW_SELECTOR(stream->channels);
			if (last_selector_index == -1)
				SET_SELECTOR(tmp_channel_selector, stream->channels);
			else if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, stream->channels)) {
				LOG_S(LL_VERBOSE, "note: the last effect changed the number of channels");
				free(tmp_channel_selector);
				goto fail;
			}
			free(channel_selector);
			channel_selector = tmp_channel_selector;
			channels_changed = 0;
		}
		if (argv[k][0] == '@') {
			old_stream_channels = stream->channels;
			if (build_effects_chain_from_file(chain, stream, channel_selector, dir, &argv[k][1]))
				goto fail;
			if (stream->channels != old_stream_channels) channels_changed = 1;
			i = ++k + 1;
			continue;
		}
		ei = get_effect_info(argv[k]);
		/* Find end of argument list */
		for (; i < argc && get_effect_info(argv[i]) == NULL && argv[i][0] != ':' && argv[i][0] != '@' && !(argv[i][0] == '!' && argv[i][1] == '\0'); ++i);
		if (ei == NULL) {
			if (allow_fail)
				LOG_FMT(LL_VERBOSE, "warning: no such effect: %s", argv[k]);
			else {
				LOG_FMT(LL_ERROR, "error: no such effect: %s", argv[k]);
				goto fail;
			}
		}
		else {
			if (LOGLEVEL(LL_VERBOSE)) {
				fprintf(stderr, "%s: effect:", dsp_globals.prog_name);
				for (j = 0; j < i - k; ++j)
					fprintf(stderr, " %s", argv[k + j]);
				fprintf(stderr, "; channels=%d [", stream->channels);
				print_selector(channel_selector, stream->channels);
				fprintf(stderr, "] fs=%d\n", stream->fs);
			}
			e = ei->init(ei, stream, channel_selector, dir, i - k, &argv[k]);
			if (e == NULL) {
				if (allow_fail)
					LOG_FMT(LL_VERBOSE, "warning: failed to initialize non-essential effect: %s", argv[k]);
				else {
					LOG_FMT(LL_ERROR, "error: failed to initialize effect: %s", argv[k]);
					goto fail;
				}
			}
			else if (e->run == NULL) {
				LOG_FMT(LL_VERBOSE, "info: not using effect: %s", argv[k]);
				destroy_effect(e);
			}
			else {
				append_effect(chain, e);
				if (e->ostream.channels != stream->channels) channels_changed = 1;
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
		LOG_FMT(LL_ERROR, "error: failed to load effects file: %s: %s", p, strerror(errno));
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
	LOG_FMT(LL_VERBOSE, "info: begin effects file: %s", p);
	if (build_effects_chain(argc, argv, chain, stream, channel_selector, d))
		goto fail;
	LOG_FMT(LL_VERBOSE, "info: end effects file: %s", p);
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

ssize_t get_effects_chain_buffer_len(struct effects_chain *chain, ssize_t in_frames, int in_channels)
{
	int gcd;
	ssize_t frames = in_frames, len, max_len = in_frames * in_channels;
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->ostream.fs != e->istream.fs) {
			gcd = find_gcd(e->ostream.fs, e->istream.fs);
			frames = ratio_mult_ceil(frames, e->ostream.fs / gcd, e->istream.fs / gcd);
		}
		len = frames * e->ostream.channels;
		if (len  > max_len) max_len = len;
		e = e->next;
	}
	return max_len;
}

sample_t * run_effects_chain(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	sample_t *ibuf = buf1, *obuf = buf2, *tmp;
	while (e != NULL && *frames > 0) {
		tmp = e->run(e, frames, ibuf, obuf);
		if (tmp == obuf) {
			obuf = ibuf;
			ibuf = tmp;
		}
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
			LOG_FMT(LL_ERROR, "plot: error: effect '%s' does not support plotting", e->name);
			return;
		}
		if (channels == -1)
			channels = e->ostream.channels;
		else if (channels != e->ostream.channels) {
			LOG_FMT(LL_ERROR, "plot: error: effect '%s' changed the number of channels", e->name);
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
	int gcd;
	ssize_t ftmp = *frames, dframes = -1;
	struct effect *e = chain->head;
	while (e != NULL && dframes == -1) {
		dframes = ftmp;
		if (e->drain != NULL) e->drain(e, &dframes, buf1);
		else dframes = -1;
		if (e->ostream.fs != e->istream.fs) {
			gcd = find_gcd(e->ostream.fs, e->istream.fs);
			ftmp = ratio_mult_ceil(ftmp, e->ostream.fs / gcd, e->istream.fs / gcd);
		}
		e = e->next;
	}
	*frames = dframes;
	return run_effects_chain(e, frames, buf1, buf2);
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
