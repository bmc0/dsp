/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sndfile.h>
#include "sndfile.h"
#include "util.h"

struct sndfile_type_info {
	const char *name;
	int type;
};

struct sndfile_enc_info {
	const char *name;
	int prec, can_dither, do_scale, sf_enc;
};

struct sndfile_state {
	SNDFILE *f;
	SF_INFO *info;
	sample_t scale;
};

static const char codec_name[] = "sndfile";

static struct sndfile_type_info types[] = {
	{ "sndfile", 0 },
	{ "wav",     SF_FORMAT_WAV },
	{ "aiff",    SF_FORMAT_AIFF },
	{ "au",      SF_FORMAT_AU },
	{ "raw",     SF_FORMAT_RAW },
	{ "paf",     SF_FORMAT_PAF },
	{ "svx",     SF_FORMAT_SVX },
	{ "nist",    SF_FORMAT_NIST },
	{ "voc",     SF_FORMAT_VOC },
	{ "ircam",   SF_FORMAT_IRCAM },
	{ "w64",     SF_FORMAT_W64 },
	{ "mat4",    SF_FORMAT_MAT4 },
	{ "mat5",    SF_FORMAT_MAT5 },
	{ "pvf",     SF_FORMAT_PVF },
	{ "xi",      SF_FORMAT_XI },
	{ "htk",     SF_FORMAT_HTK },
	{ "sds",     SF_FORMAT_SDS },
	{ "avr",     SF_FORMAT_AVR },
	{ "wavex",   SF_FORMAT_WAVEX },
	{ "sd2",     SF_FORMAT_SD2 },
	{ "flac",    SF_FORMAT_FLAC },
	{ "caf",     SF_FORMAT_CAF },
	{ "wve",     SF_FORMAT_WVE },
	{ "ogg",     SF_FORMAT_OGG },
	{ "mpc2k",   SF_FORMAT_MPC2K },
	{ "rf64",    SF_FORMAT_RF64 },
	{ "sf/mpeg", SF_FORMAT_MPEG },
};

static struct sndfile_enc_info encodings[] = {
	{ "s16",          16, 1, 1, SF_FORMAT_PCM_16 },
	{ "s8",            8, 1, 1, SF_FORMAT_PCM_S8 },
	{ "u8",            8, 1, 1, SF_FORMAT_PCM_U8 },
	{ "s24",          24, 1, 1, SF_FORMAT_PCM_24 },
	{ "s32",          32, 1, 1, SF_FORMAT_PCM_32 },
	{ "float",        24, 0, 0, SF_FORMAT_FLOAT  },
	{ "double",       53, 0, 0, SF_FORMAT_DOUBLE },
	{ "mu-law",       13, 0, 0, SF_FORMAT_ULAW },
	{ "a-law",        14, 0, 0, SF_FORMAT_ALAW },
	{ "ima_adpcm",    13, 0, 0, SF_FORMAT_IMA_ADPCM },
	{ "ms_adpcm",     13, 0, 0, SF_FORMAT_MS_ADPCM },
	{ "gsm6.10",      16, 0, 0, SF_FORMAT_GSM610 },
	{ "vox_adpcm",    13, 0, 0, SF_FORMAT_VOX_ADPCM },
	{ "nms_adpcm_16",  8, 0, 0, SF_FORMAT_NMS_ADPCM_16 },
	{ "nms_adpcm_24",  8, 0, 0, SF_FORMAT_NMS_ADPCM_24 },
	{ "nms_adpcm_32", 12, 0, 0, SF_FORMAT_NMS_ADPCM_32 },
	{ "g721_32",      12, 0, 0, SF_FORMAT_G721_32 },
	{ "g723_24",       8, 0, 0, SF_FORMAT_G723_24 },
	{ "g723_40",      14, 0, 0, SF_FORMAT_G723_40 },
	{ "dwvw_12",      12, 0, 0, SF_FORMAT_DWVW_12 },
	{ "dwvw_16",      16, 0, 0, SF_FORMAT_DWVW_16 },
	{ "dwvw_24",      24, 0, 0, SF_FORMAT_DWVW_24 },
	{ "dpcm_8",        8, 0, 0, SF_FORMAT_DPCM_8 },
	{ "dpcm_16",      16, 0, 0, SF_FORMAT_DPCM_16 },
	{ "vorbis",       24, 0, 0, SF_FORMAT_VORBIS },
	{ "opus",         24, 0, 0, SF_FORMAT_OPUS },
	{ "alac_16",      16, 1, 0, SF_FORMAT_ALAC_16 },
	{ "alac_20",      20, 1, 0, SF_FORMAT_ALAC_20 },
	{ "alac_24",      24, 1, 0, SF_FORMAT_ALAC_24 },
	{ "alac_32",      32, 1, 0, SF_FORMAT_ALAC_32 },
	{ "mpeg1.1",      24, 0, 0, SF_FORMAT_MPEG_LAYER_I },
	{ "mpeg1.2",      24, 0, 0, SF_FORMAT_MPEG_LAYER_II },
	{ "mpeg2.3",      24, 0, 0, SF_FORMAT_MPEG_LAYER_III },
};

ssize_t sndfile_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	int e = 0;
	struct sndfile_state *state = (struct sndfile_state *) c->data;
	const sf_count_t r = sf_readf_double(state->f, buf, frames);
	if (r != frames && (e = sf_error(state->f)) != SF_ERR_NO_ERROR)
		LOG_FMT(LL_ERROR, "%s: %s", __func__, sf_error_number(e));
	return (ssize_t) r;
}

static inline void buf_scale_int(sample_t *buf, const sample_t s, ssize_t samples)
{
	const sample_t c = s - 1.0;
	for (sample_t *end = buf + samples; buf < end; ++buf) {
		*buf *= s;
		if (*buf > c) *buf = c;
	}
}

ssize_t sndfile_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	int e = 0;
	struct sndfile_state *state = (struct sndfile_state *) c->data;
	if (state->scale > 1.0) buf_scale_int(buf, state->scale, frames * c->channels);
	const sf_count_t r = sf_writef_double(state->f, buf, frames);
	if (r != frames && (e = sf_error(state->f)) != SF_ERR_NO_ERROR)
		LOG_FMT(LL_ERROR, "%s: %s", __func__, sf_error_number(e));
	return (ssize_t) r;
}

ssize_t sndfile_seek(struct codec *c, ssize_t pos)
{
	int e = 0;
	struct sndfile_state *state = (struct sndfile_state *) c->data;
	if (!state->info->seekable)
		return -1;
	if (pos < 0)
		pos = 0;
	else if (pos >= c->frames)
		pos = c->frames - 1;
	const sf_count_t r = sf_seek(state->f, pos, SEEK_SET);
	if (r < 0 && (e = sf_error(state->f)) != SF_ERR_NO_ERROR)
		LOG_FMT(LL_ERROR, "%s: %s", __func__, sf_error_number(e));
	return (ssize_t) r;
}

ssize_t sndfile_delay(struct codec *c)
{
	return 0;
}

void sndfile_drop(struct codec *c)
{
	/* do nothing */
}

void sndfile_pause(struct codec *c, int p)
{
	/* do nothing */
}

void sndfile_destroy(struct codec *c)
{
	struct sndfile_state *state = (struct sndfile_state *) c->data;
	sf_close(state->f);
	free(state->info);
	free(state);
}

static int sndfile_get_type(const char *t)
{
	int i;
	for (i = 0; i < LENGTH(types); ++i)
		if (strcmp(t, types[i].name) == 0)
			return types[i].type;
	return -1;
}

static const char * sndfile_get_type_name(int f)
{
	int i;
	for (i = 0; i < LENGTH(types); ++i)
		if (types[i].type == (f & SF_FORMAT_TYPEMASK))
			return types[i].name;
	return "unknown";
}

static int sndfile_get_sf_enc(const char *enc)
{
	int i;
	if (enc == NULL)
		return encodings[0].sf_enc;
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return encodings[i].sf_enc;
	return 0;
}

static struct sndfile_enc_info * sndfile_get_enc_info(int f)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		if (encodings[i].sf_enc == (f & SF_FORMAT_SUBMASK))
			return &encodings[i];
	return NULL;
}

static int sndfile_get_endian(int endian)
{
	switch (endian) {
	case CODEC_ENDIAN_BIG:
		return SF_ENDIAN_BIG;
	case CODEC_ENDIAN_LITTLE:
		return SF_ENDIAN_LITTLE;
	case CODEC_ENDIAN_NATIVE:
		return SF_ENDIAN_CPU;
	default:
		return SF_ENDIAN_FILE;
	}
}

struct codec * sndfile_codec_init(const struct codec_params *p)
{
	SNDFILE *f = NULL;
	SF_INFO *info = NULL;
	struct codec *c = NULL;
	struct sndfile_state *state = NULL;
	struct sndfile_enc_info *enc_info = NULL;

	info = calloc(1, sizeof(SF_INFO));
	info->samplerate = p->fs;
	info->channels = p->channels;
	info->format = ((p->type == NULL) ? 0 : sndfile_get_type(p->type)) | sndfile_get_sf_enc(p->enc) | sndfile_get_endian(p->endian);
	if (info->format == -1) {
		LOG_FMT(LL_ERROR, "%s: error: bad format type or encoding: %s: type=%s enc=%s", codec_name, p->path, p->type, p->enc);
		goto fail;
	}
	f = sf_open(p->path, (p->mode == CODEC_MODE_WRITE) ? SFM_WRITE : SFM_READ, info);
	if (f == NULL) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open file: %s: %s", codec_name, p->path, sf_strerror(NULL));
		goto fail;
	}

	enc_info = sndfile_get_enc_info(info->format);
	state = calloc(1, sizeof(struct sndfile_state));
	state->f = f;
	state->info = info;
#if BIT_PERFECT
	if (p->mode == CODEC_MODE_WRITE && enc_info->do_scale) {
		state->scale = (sample_t) (((uint32_t) 1) << (enc_info->prec - 1));
		sf_command(f, SFC_SET_NORM_DOUBLE, NULL, SF_FALSE);
	}
#endif

	c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = sndfile_get_type_name(info->format);
	c->enc = (enc_info) ? enc_info->name : "unknown";
	c->fs = info->samplerate;
	c->channels = info->channels;
	c->prec = (enc_info) ? enc_info->prec : 0;
	if (enc_info && enc_info->can_dither) c->hints |= CODEC_HINT_CAN_DITHER;
	c->frames = info->frames;
	if (p->mode == CODEC_MODE_READ) c->read = sndfile_read;
	else c->write = sndfile_write;
	c->seek = sndfile_seek;
	c->delay = sndfile_delay;
	c->drop = sndfile_drop;
	c->pause = sndfile_pause;
	c->destroy = sndfile_destroy;
	c->data = state;

	return c;

	fail:
	if (f)
		sf_close(f);
	free(info);
	return NULL;
}

void sndfile_codec_print_encodings(const char *type)
{
	SF_INFO info;
	int i;
	info.format = sndfile_get_type(type);
	if (info.format == -1)
		return;
	else if (info.format == 0)
		fprintf(stdout, " <autodetected>");
	else {
		info.format |= SF_ENDIAN_FILE;
		info.samplerate = DEFAULT_FS;
		info.channels = 1;
		for (i = 0; i < LENGTH(encodings); ++i) {
			info.format &= ~SF_FORMAT_SUBMASK;
			info.format |= encodings[i].sf_enc;
			if (sf_format_check(&info))
				fprintf(stdout, " %s", encodings[i].name);
		}
	}
}
