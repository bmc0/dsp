#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "codec.h"

#include "null.h"
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
	/* args: path, type, encoding, fs, channels, endian, mode */
	struct codec * (*init)(const char *, const char *, const char *, int, int, int, int);
	void (*print_encodings)(const char *);
};

#ifdef __HAVE_SNDFILE__
static const char *wav_ext[]   = { ".wav", NULL };
static const char *aiff_ext[]  = { ".aif", ".aiff", ".aifc", NULL };
static const char *au_ext[]    = { ".au", NULL };
static const char *raw_ext[]   = { ".raw", NULL };
static const char *paf_ext[]   = { ".paf", NULL };
static const char *svx_ext[]   = { ".8svx", ".iff", NULL };
static const char *nist_ext[]  = { ".nist", NULL };
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
static const char *caf_ext[]   = { ".caf", NULL };
static const char *wve_ext[]   = { ".wve", NULL };
static const char *ogg_ext[]   = { ".ogg", ".oga", ".ogv", NULL };
static const char *mpc2k_ext[] = { ".mpc2k", NULL };
static const char *rf64_ext[]  = { ".wav", ".rf64", NULL };
#endif
#ifdef __HAVE_MAD__
static const char *mp3_ext[]   = { ".mp3", NULL };
#endif

struct codec_info codecs[] = {
#ifndef __LADSPA_FRONTEND__
	{ "null",    NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, null_codec_init,    null_codec_print_encodings },
#endif
#ifdef __HAVE_SNDFILE__
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
#endif
#ifdef __HAVE_FFMPEG__
	{ "ffmpeg",  NULL,      CODEC_MODE_READ,                  ffmpeg_codec_init,  ffmpeg_codec_print_encodings },
#endif
#ifdef __HAVE_ALSA__
	{ "alsa",    NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, alsa_codec_init,    alsa_codec_print_encodings },
#endif
#ifdef __HAVE_AO__
	{ "ao",      NULL,      CODEC_MODE_WRITE,                 ao_codec_init,      ao_codec_print_encodings },
#endif
#ifdef __HAVE_MAD__
	{ "mp3",     mp3_ext,   CODEC_MODE_READ,                  mp3_codec_init,     mp3_codec_print_encodings },
#endif
#ifndef __LADSPA_FRONTEND__
	{ "pcm",     NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, pcm_codec_init,     pcm_codec_print_encodings },
#endif
#ifdef __HAVE_PULSE__
	{ "pulse",   NULL,      CODEC_MODE_READ|CODEC_MODE_WRITE, pulse_codec_init,   pulse_codec_print_encodings },
#endif
};

static const char *fallback_input_codecs[] = {
#ifdef __HAVE_SNDFILE__
	"sndfile",
#endif
#ifdef __HAVE_FFMPEG__
	"ffmpeg",
#endif
};

static const char *fallback_output_codecs[] = {
#ifdef __HAVE_PULSE__
	"pulse",
#endif
#ifdef __HAVE_ALSA__
	"alsa",
#endif
#ifdef __HAVE_AO__
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

struct codec * init_codec(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	int i, old_loglevel;
	struct codec_info *info;
	struct codec *c = NULL;
	const char *ext;
	ext = strrchr(path, '.');
	if (type != NULL) {
		info = get_codec_info_by_type(type);
		if (info == NULL) {
			LOG(LL_ERROR, "dsp: error: bad type: %s\n", type);
			return NULL;
		}
		if (info->modes & mode)
			return info->init(path, info->type, enc, fs, channels, endian, mode);
		LOG(LL_ERROR, "dsp: %s: error: mode '%c' not supported\n", info->type, (mode == CODEC_MODE_READ) ? 'r' : 'w');
		return NULL;
	}
	if (ext != NULL && (info = get_codec_info_by_ext(ext)) != NULL) {
		if (info->modes & mode)
			return info->init(path, info->type, enc, fs, channels, endian, mode);
		LOG(LL_ERROR, "dsp: %s: error: mode '%c' not supported\n", info->type, (mode == CODEC_MODE_READ) ? 'r' : 'w');
		return NULL;
	}
	c = NULL;
	if ((old_loglevel = dsp_globals.loglevel) == LL_NORMAL)
		dsp_globals.loglevel = LL_ERROR;
	if (mode == CODEC_MODE_WRITE) {
		if (LENGTH(fallback_output_codecs) == 0)
			LOG(LL_ERROR, "dsp: error: no fallback output(s) available and no output given\n");
		for (i = 0; i < LENGTH(fallback_output_codecs); ++i) {
			info = get_codec_info_by_type(fallback_output_codecs[i]);
			if (info != NULL && info->modes & mode && (c = info->init(path, info->type, enc, fs, channels, endian, mode)) != NULL)
				break;
		}
	}
	else {
		for (i = 0; i < LENGTH(fallback_input_codecs); ++i) {
			info = get_codec_info_by_type(fallback_input_codecs[i]);
			if (info != NULL && info->modes & mode && (c = info->init(path, info->type, enc, fs, channels, endian, mode)) != NULL)
				break;
		}
	}
	dsp_globals.loglevel = old_loglevel;
	return c;
}

void destroy_codec(struct codec *c)
{
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
