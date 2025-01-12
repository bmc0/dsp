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
#include "util.h"
#include "codec.h"

#include "null.h"
#include "sgen.h"
#include "sndfile.h"
#include "ffmpeg.h"
#include "alsa.h"
#include "ao.h"
#include "mp3.h"
#include "pcm.h"
#include "pulse.h"

struct codec_info {
	const char *type;
	const char **ext;  /* null terminated array of extension strings */
	int modes;
	struct codec * (*init)(const struct codec_params *);
	void (*print_encodings)(const char *);
};

#ifdef HAVE_SNDFILE
static const char *wav_ext[]   = { ".wav", NULL };
static const char *aiff_ext[]  = { ".aiff", ".aif", ".aifc", NULL };
static const char *au_ext[]    = { ".au", NULL };
static const char *raw_ext[]   = { ".raw", NULL };
static const char *paf_ext[]   = { ".paf", NULL };
static const char *svx_ext[]   = { ".8svx", ".iff", NULL };
static const char *nist_ext[]  = { ".wav", ".nist", NULL };
static const char *voc_ext[]   = { ".voc", NULL };
static const char *ircam_ext[] = { ".sf", NULL };
static const char *w64_ext[]   = { ".w64", NULL };
static const char *mat4_ext[]  = { ".mat", NULL };
static const char *mat5_ext[]  = { ".mat", NULL };
static const char *pvf_ext[]   = { ".pvf", NULL };
static const char *xi_ext[]    = { ".xi", NULL };
static const char *htk_ext[]   = { ".htk", NULL };
static const char *sds_ext[]   = { ".sds", NULL };
static const char *avr_ext[]   = { ".avr", NULL };
static const char *wavex_ext[] = { ".wav", ".wavex", NULL };
static const char *sd2_ext[]   = { ".sd2", NULL };
static const char *flac_ext[]  = { ".flac", NULL };
static const char *caf_ext[]   = { ".caf", ".m4a", NULL };
static const char *wve_ext[]   = { ".wve", NULL };
static const char *ogg_ext[]   = { ".ogg", ".oga", ".ogv", ".opus", NULL };
static const char *mpc2k_ext[] = { ".mpc", ".mpc2k", NULL };
static const char *rf64_ext[]  = { ".wav", ".rf64", NULL };
static const char *sf_mpeg_ext[] = { ".mp1", ".mp2", ".mp3", NULL };
#endif
#ifdef HAVE_MAD
static const char *mp3_ext[]   = { ".mp3", NULL };
#endif

struct codec_info codecs[] = {
#ifndef LADSPA_FRONTEND
	{ "null",    NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, null_codec_init,    null_codec_print_encodings },
	{ "sgen",    NULL,      CODEC_MODE_READ,                  sgen_codec_init,    sgen_codec_print_encodings },
#endif
#ifdef HAVE_SNDFILE
	{ "sndfile", NULL,      CODEC_MODE_READ,                  sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wav",     wav_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "aiff",    aiff_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "au",      au_ext,    CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "raw",     raw_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "paf",     paf_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "svx",     svx_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "nist",    nist_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "voc",     voc_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "ircam",   ircam_ext, CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "w64",     w64_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mat4",    mat4_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mat5",    mat5_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "pvf",     pvf_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "xi",      xi_ext,    CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "htk",     htk_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "sds",     sds_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "avr",     avr_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wavex",   wavex_ext, CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "sd2",     sd2_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "flac",    flac_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "caf",     caf_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wve",     wve_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "ogg",     ogg_ext,   CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mpc2k",   mpc2k_ext, CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "rf64",    rf64_ext,  CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "sf/mpeg", sf_mpeg_ext, CODEC_MODE_READ|CODEC_MODE_WRITE, sndfile_codec_init, sndfile_codec_print_encodings },
#endif
#ifdef HAVE_FFMPEG
	{ "ffmpeg",  NULL,      CODEC_MODE_READ,                  ffmpeg_codec_init,  ffmpeg_codec_print_encodings },
#endif
#ifdef HAVE_ALSA
	{ "alsa",    NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, alsa_codec_init,    alsa_codec_print_encodings },
#endif
#ifdef HAVE_AO
	{ "ao",      NULL,                      CODEC_MODE_WRITE, ao_codec_init,      ao_codec_print_encodings },
#endif
#ifdef HAVE_MAD
	{ "mp3",     mp3_ext,   CODEC_MODE_READ,                  mp3_codec_init,     mp3_codec_print_encodings },
#endif
#ifndef LADSPA_FRONTEND
	{ "pcm",     NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, pcm_codec_init,     pcm_codec_print_encodings },
#endif
#ifdef HAVE_PULSE
	{ "pulse",   NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, pulse_codec_init,   pulse_codec_print_encodings },
#endif
};

static const char *fallback_input_codecs[] = {
#ifdef HAVE_SNDFILE
	"sndfile",
#endif
#ifdef HAVE_FFMPEG
	"ffmpeg",
#endif
};

static const char *fallback_output_codecs[] = {
#ifdef HAVE_PULSE
	"pulse",
#endif
#ifdef HAVE_ALSA
	"alsa",
#endif
#ifdef HAVE_AO
	"ao",
#endif
};

static struct codec_info * get_codec_info_by_type(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(codecs); ++i)
		if (strcmp(type, codecs[i].type) == 0)
			return &codecs[i];
	return NULL;
}

static struct codec_info * get_codec_info_by_ext(const char *ext)
{
	int i, k;
	for (i = 0; i < LENGTH(codecs); ++i) {
		if (codecs[i].ext == NULL)
			continue;
		for (k = 0; codecs[i].ext[k] != NULL; ++k)
			if (strcasecmp(ext, codecs[i].ext[k]) == 0)
				return &codecs[i];
	}
	return NULL;
}

struct codec * init_codec(const struct codec_params *p_in)
{
	int i, old_loglevel;
	struct codec_info *info;
	struct codec *c = NULL;
	const char *ext;
	if (p_in->mode != CODEC_MODE_READ && p_in->mode != CODEC_MODE_WRITE) {
		LOG_FMT(LL_ERROR, "%s: BUG: bad mode", (p_in->path) ? p_in->path : "[NULL path]");
		return NULL;
	}
	struct codec_params p = *p_in;
	ext = strrchr(p.path, '.');
	if (p.type != NULL) {
		info = get_codec_info_by_type(p.type);
		if (info == NULL) {
			LOG_FMT(LL_ERROR, "error: bad type: %s", p.type);
			return NULL;
		}
		p.type = info->type;
		if (info->modes & p.mode)
			return info->init(&p);
		LOG_FMT(LL_ERROR, "%s: error: mode '%c' not supported", info->type, (p.mode == CODEC_MODE_READ) ? 'r' : 'w');
		return NULL;
	}
	if (ext != NULL && (info = get_codec_info_by_ext(ext)) != NULL) {
		p.type = info->type;
		if (info->modes & p.mode)
			return info->init(&p);
		LOG_FMT(LL_ERROR, "%s: error: mode '%c' not supported", info->type, (p.mode == CODEC_MODE_READ) ? 'r' : 'w');
		return NULL;
	}
	c = NULL;
	if ((old_loglevel = dsp_globals.loglevel) == LL_NORMAL)
		dsp_globals.loglevel = LL_ERROR;
	if (p.mode == CODEC_MODE_WRITE) {
		if (LENGTH(fallback_output_codecs) == 0)
			LOG_S(LL_ERROR, "error: no fallback output(s) available and no output given");
		for (i = 0; i < LENGTH(fallback_output_codecs); ++i) {
			info = get_codec_info_by_type(fallback_output_codecs[i]);
			if (info != NULL && (info->modes & p.mode)) {
				p.type = info->type;
				if ((c = info->init(&p)) != NULL)
					break;
			}
		}
	}
	else {
		for (i = 0; i < LENGTH(fallback_input_codecs); ++i) {
			info = get_codec_info_by_type(fallback_input_codecs[i]);
			if (info != NULL && (info->modes & p.mode)) {
				p.type = info->type;
				if ((c = info->init(&p)) != NULL)
					break;
			}
		}
	}
	dsp_globals.loglevel = old_loglevel;
	return c;
}

void destroy_codec(struct codec *c)
{
	if (c == NULL)
		return;
	c->destroy(c);
	free(c);
}

void append_codec(struct codec_list *l, struct codec *c)
{
	if (l->tail == NULL)
		l->head = c;
	else
		l->tail->next = c;
	l->tail = c;
	c->next = NULL;
}

void destroy_codec_list_head(struct codec_list *l)
{
	struct codec *h = l->head;
	if (h == NULL)
		return;
	l->head = h->next;
	destroy_codec(h);
}

void destroy_codec_list(struct codec_list *l)
{
	while (l->head != NULL)
		destroy_codec_list_head(l);
}

void print_all_codecs(void)
{
	int i;
	fprintf(stdout, "Types:\n  Type:    Modes: Encodings:\n");
	for (i = 0; i < LENGTH(codecs); ++i) {
		fprintf(stdout, "  %-8s %c%c    ", codecs[i].type,
			(codecs[i].modes & CODEC_MODE_READ) ? 'r' : ' ', (codecs[i].modes & CODEC_MODE_WRITE) ? 'w' : ' ');
		codecs[i].print_encodings(codecs[i].type);
		fputc('\n', stdout);
	}
}
