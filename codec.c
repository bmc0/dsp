#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "codec.h"

#include "codecs/sndfile.h"
#include "codecs/alsa.h"
#include "codecs/mp3.h"
#include "codecs/null.h"

struct codec_info {
	const char *type;
	const char **ext;  /* null terminated array of extension strings */
	int modes;
	int interactive;
	struct codec * (*init)(const char *, int, const char *, const char *, int, int, int);
	void (*print_encodings)(const char *);
};

static const char *mp3_ext[] = { ".mp3", NULL };

struct codec_info codecs[] = {
	{ "sndfile", NULL,    CODEC_MODE_READ,                  0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wav",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "aiff",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "au",      NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "raw",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "paf",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "svx",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "nist",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "voc",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "ircam",   NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "w64",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mat4",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mat5",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "pvf",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "xi",      NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "htk",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "sds",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "avr",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wavex",   NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "sd2",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "flac",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "caf",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "wve",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "ogg",     NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "mpc2k",   NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "rf64",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 0, sndfile_codec_init, sndfile_codec_print_encodings },
	{ "alsa",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 1, alsa_codec_init,    alsa_codec_print_encodings },
	{ "mp3",     mp3_ext, CODEC_MODE_READ,                  0, mp3_codec_init,     mp3_codec_print_encodings },
	{ "null",    NULL,    CODEC_MODE_READ|CODEC_MODE_WRITE, 1, null_codec_init,    null_codec_print_encodings },
};

static const char *fallback_codecs[] = {
	"sndfile",
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

struct codec * init_codec(const char *type, int mode, const char *path, const char *enc, int endian, int rate, int channels)
{
	int i;
	struct codec_info *info;
	struct codec *c;
	const char *ext;
	if (type == NULL && mode == CODEC_MODE_WRITE)
		type = DEFAULT_OUTPUT_TYPE;
	if (type != NULL) {
		info = get_codec_info_by_type(type);
		if (info == NULL) {
			LOG(LL_ERROR, "dsp: error: bad type: %s\n", type);
			return NULL;
		}
		if (info->modes & mode)
			return info->init(info->type, mode, path, enc, endian, rate, channels);
		LOG(LL_ERROR, "dsp: %s: error: mode '%c' not supported\n", info->type, (mode == CODEC_MODE_READ) ? 'r' : 'w');
		return NULL;
	}
	ext = strrchr(path, '.');
	if (ext != NULL && (info = get_codec_info_by_ext(ext)) != NULL && (c = info->init(info->type, mode, path, enc, endian, rate, channels)) != NULL)
		return c;
	for (i = 0; i < LENGTH(fallback_codecs); ++i) {
		info = get_codec_info_by_type(fallback_codecs[i]);
		if (info != NULL && info->modes & mode && (c = info->init(info->type, mode, path, enc, endian, rate, channels)) != NULL)
			return c;
	}
	return NULL;
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
	fprintf(stderr, "types:\n  type:    modes: encodings:\n");
	for (i = 0; i < LENGTH(codecs); ++i) {
		fprintf(stderr, "  %-8s %c%c    ", codecs[i].type, (codecs[i].modes & CODEC_MODE_READ) ? 'r' : ' ', (codecs[i].modes & CODEC_MODE_WRITE) ? 'w' : ' ');
		codecs[i].print_encodings(codecs[i].type);
		fputc('\n', stderr);
	}
}
