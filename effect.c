/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <errno.h>
#include "effect.h"
#include "util.h"

#include "biquad.h"
#include "gain.h"
#include "crossfeed.h"
#include "matrix4.h"
#include "matrix4_mb.h"
#include "remix.h"
#include "st2ms.h"
#include "delay.h"
#include "resample.h"
#include "fir.h"
#include "fir_p.h"
#include "zita_convolver.h"
#include "hilbert.h"
#include "decorrelate.h"
#include "noise.h"
#include "ladspa_host.h"
#include "stats.h"

static const struct effect_info effects[] = {
	{ "lowpass_1",          "lowpass_1 f0[k]",                         biquad_effect_init,    BIQUAD_LOWPASS_1 },
	{ "highpass_1",         "highpass_1 f0[k]",                        biquad_effect_init,    BIQUAD_HIGHPASS_1 },
	{ "allpass_1",          "allpass_1 f0[k]",                         biquad_effect_init,    BIQUAD_ALLPASS_1 },
	{ "lowshelf_1",         "lowshelf_1 f0[k] gain",                   biquad_effect_init,    BIQUAD_LOWSHELF_1 },
	{ "highshelf_1",        "highshelf_1 f0[k] gain",                  biquad_effect_init,    BIQUAD_HIGHSHELF_1 },
	{ "lowpass_1p",         "lowpass_1p f0[k]",                        biquad_effect_init,    BIQUAD_LOWPASS_1P },
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
	{ "matrix4",            "matrix4 [options] [surround_level]",      matrix4_effect_init,   0 },
	{ "matrix4_mb",         "matrix4_mb [options] [surround_level]",   matrix4_mb_effect_init, 0 },
	{ "remix",              "remix channel_selector|. ...",            remix_effect_init,     0 },
	{ "st2ms",              "st2ms",                                   st2ms_effect_init,     ST2MS_EFFECT_NUMBER_ST2MS },
	{ "ms2st",              "ms2st",                                   st2ms_effect_init,     ST2MS_EFFECT_NUMBER_MS2ST },
	{ "delay",              "delay delay[s|m|S]",                      delay_effect_init,     0 },
#ifdef HAVE_FFTW3
#ifndef SYMMETRIC_IO
	{ "resample",           "resample [bandwidth] fs[k]",              resample_effect_init,  0 },
#endif
	{ "fir",                "fir [file:][~/]filter_path|coefs:list[/list...]",                  fir_effect_init,   0 },
	{ "fir_p",              "fir_p [max_part_len] [file:][~/]filter_path|coefs:list[/list...]", fir_p_effect_init, 0 },
#endif
#ifdef HAVE_ZITA_CONVOLVER
	{ "zita_convolver",     "zita_convolver [min_part_len [max_part_len]] [~/]filter_path", zita_convolver_effect_init, 0 },
#endif
#ifdef HAVE_FFTW3
	{ "hilbert",            "hilbert [-p] taps",                       hilbert_effect_init,   0 },
#endif
	{ "decorrelate",        "decorrelate [-m] [stages]",               decorrelate_effect_init, 0 },
	{ "noise",              "noise level",                             noise_effect_init,     0 },
#ifdef ENABLE_LADSPA_HOST
	{ "ladspa_host",        "ladspa_host module_path plugin_label [control ...]", ladspa_host_effect_init, 0 },
#endif
	{ "stats",              "stats [ref_level]",                       stats_effect_init,     0 },
};

const struct effect_info * get_effect_info(const char *name)
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

int build_effects_chain(int argc, const char *const *argv, struct effects_chain *chain, struct stream_info *stream, const char *initial_channel_selector, const char *dir)
{
	int i = 1, k = 0, j, last_selector_index = -1, old_stream_channels, allow_fail = 0, channels_changed = 0;
	char *channel_selector, *tmp_channel_selector;
	const struct effect_info *ei = NULL;
	struct effect *e = NULL;

	if (initial_channel_selector == NULL) {
		channel_selector = NEW_SELECTOR(stream->channels);
		SET_SELECTOR(channel_selector, stream->channels);
	}
	else {
		channel_selector = NEW_SELECTOR(stream->channels);
		COPY_SELECTOR(channel_selector, initial_channel_selector, stream->channels);
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
			while (e != NULL) {
				struct effect *e_n = e->next;
				if (e->run == NULL) {
					LOG_FMT(LL_VERBOSE, "info: not using effect: %s", argv[k]);
					destroy_effect(e);
				}
				else {
					append_effect(chain, e);
					if (e->ostream.channels != stream->channels) channels_changed = 1;
					*stream = e->ostream;
				}
				e = e_n;
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

int build_effects_chain_from_file(struct effects_chain *chain, struct stream_info *stream, const char *channel_selector, const char *dir, const char *path)
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
	if (build_effects_chain(argc, (const char *const *) argv, chain, stream, channel_selector, d))
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

ssize_t get_effects_chain_max_out_frames(struct effects_chain *chain, ssize_t in_frames)
{
	ssize_t frames = in_frames;
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->ostream.fs != e->istream.fs) {
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			frames = ratio_mult_ceil(frames, e->ostream.fs / gcd, e->istream.fs / gcd);
		}
		e = e->next;
	}
	return frames;
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

void signal_effects_chain(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->signal != NULL) e->signal(e);
		e = e->next;
	}
}

static const char gnuplot_header[] =
	"set xlabel 'Frequency (Hz)'\n"
	"set ylabel 'Magnitude (dB)'\n"
	"set logscale x\n"
	"set format x '10^{%L}'\n"
	"set samples 500\n"
	"set mxtics\n"
	"set mytics\n"
	"set grid xtics ytics mxtics mytics lw 0.8, lw 0.3\n"
	"set key on\n"
	"j={0,1}\n"
	"\n"
	"set yrange [-30:20]\n";

static const char gnuplot_header_phase[] =
	"set ytics nomirror\n"
	"set y2tics -180,90,180 format '%g°'\n"
	"set y2range [-180:720]\n";

void plot_effects_chain(struct effects_chain *chain, int plot_phase)
{
	if (chain->head == NULL) {
		LOG_S(LL_ERROR, "plot: error: effects chain empty");
		return;
	}
	struct effect *e = chain->head;
	int fs = e->istream.fs;
	while (e != NULL) {
		if (e->plot == NULL) {
			LOG_FMT(LL_ERROR, "plot: error: effect '%s' does not support plotting", e->name);
			return;
		}
		if (e->istream.channels != e->ostream.channels && !(e->plot_info & PLOT_INFO_MIX)) {
			LOG_FMT(LL_ERROR, "plot: BUG: effect '%s' changed the number of channels but does not have PLOT_INFO_MIX set!", e->name);
			return;
		}
		fs = e->ostream.fs;
		e = e->next;
	}
	printf("%sset xrange [10:%d/2]\n%s\n",
		gnuplot_header, fs, (plot_phase)?gnuplot_header_phase:"");
	struct effect *start_e = chain->head;
	e = start_e;
	int channels = e->istream.channels, start_idx = 0;
	for (int i = 0; e != NULL; ++i) {
		if (e->plot_info & PLOT_INFO_MIX) {
			for (int k = 0; k < e->istream.channels; ++k) {
				printf("Ht%d_%d(f)=1.0", k, i);
				struct effect *e2 = start_e;
				for (int j = start_idx; e2 != NULL && e2 != e; ++j) {
					printf("*H%d_%d(2.0*pi*f/%d)", k, j, e2->ostream.fs);
					e2 = e2->next;
				}
				putchar('\n');
			}
			start_idx = i;
			start_e = e;
			channels = e->ostream.channels;
		}
		e->plot(e, i);
		e = e->next;
	}
	for (int k = 0; k < channels; ++k) {
		printf("Ht%d(f)=1.0", k);
		e = start_e;
		for (int i = start_idx; e != NULL; ++i) {
			printf("*H%d_%d(2.0*pi*f/%d)", k, i, e->ostream.fs);
			e = e->next;
		}
		putchar('\n');
		printf("Ht%d_mag(f)=abs(Ht%d(f))\n", k, k);
		printf("Ht%d_mag_dB(f)=20*log10(Ht%d_mag(f))\n", k, k);
		printf("Ht%d_phase(f)=arg(Ht%d(f))\n", k, k);
		printf("Ht%d_phase_deg(f)=Ht%d_phase(f)*180/pi\n", k, k);
		printf("Hsum%d(f)=Ht%d_mag_dB(f)\n", k, k);
	}
	printf("\nplot ");
	for (int k = 0; k < channels; ++k) {
		printf("%sHt%d_mag_dB(x) lt %d lw 2 title 'Channel %d'", (k==0)?"":", ", k, k+1, k);
		if (plot_phase)
			printf(", Ht%d_phase_deg(x) axes x1y2 lt %d lw 1 dt '-' notitle", k, k+1);
	}
	puts("\npause mouse close");
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
