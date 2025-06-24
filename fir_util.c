/*
 * This file is part of dsp.
 *
 * Copyright (c) 2024-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <math.h>
#include "fir_util.h"

sample_t * fir_read_filter(const struct effect_info *ei, const struct stream_info *istream, const char *dir, const struct codec_params *p, int *channels, ssize_t *frames)
{
	static const char coefs_str_prefix[] = "coefs:";
	static const char file_str_prefix[] = "file:";

	if (!ei || !p || !p->path || !channels || !frames)
		return NULL;
	sample_t *data = NULL;
	const char *path = p->path;

	if (strncmp(path, coefs_str_prefix, LENGTH(coefs_str_prefix)-1) == 0) {
		char *endptr;
		path += LENGTH(coefs_str_prefix)-1;

		int filter_channels = 1;
		ssize_t i = 1, filter_frames = 1;
		for (const char *s = path; *s; ++s) {
			if (*s == ',') ++i;
			else if (*s == '/') {
				++filter_channels;
				if (i > filter_frames) filter_frames = i;
				i = 1;
			}
		}
		if (i > filter_frames) filter_frames = i;

		sample_t *ch_data = data = calloc(filter_frames * filter_channels, sizeof(sample_t));
		char *coefs_str = strdup(path);
		char *ch = coefs_str;
		while (*ch != '\0') {
			char *next_ch = isolate(ch, '/');
			char *coef = ch;
			for (i = 0; *coef != '\0'; ++i) {
				char *next_coef = isolate(coef, ',');
				if (*coef != '\0') {
					ch_data[filter_channels * i] = strtod(coef, &endptr);
					if (check_endptr(ei->name, coef, endptr, "coefficient")) {
						free(data);
						free(coefs_str);
						return NULL;
					}
				}
				coef = next_coef;
			}
			ch_data += 1;
			ch = next_ch;
		}
		free(coefs_str);
		*channels = filter_channels;
		*frames = filter_frames;
	}
	else {
		if (strncmp(path, file_str_prefix, LENGTH(file_str_prefix)-1) == 0)
			path += LENGTH(file_str_prefix)-1;
		char *fp = construct_full_path(dir, path, istream);
		struct codec_params c_params = *p;
		c_params.path = fp;
		c_params.mode = CODEC_MODE_READ;
		if (p->fs == 0) c_params.fs = istream->fs;
		struct codec *c = init_codec(&c_params);
		if (c == NULL) {
			LOG_FMT(LL_ERROR, "%s: error: failed to open filter file: %s", ei->name, fp);
			free(fp);
			return NULL;
		}
		LOG_FMT(LL_VERBOSE, "%s: input file: %s: type=%s enc=%s precision=%d channels=%d fs=%d",
			ei->name, c->path, c->type, c->enc, c->prec, c->channels, c->fs);
		free(fp);
		*channels = c->channels;
		*frames = c->frames;
		if (c->fs != istream->fs) {
			if (p->fs > 0) {
				LOG_FMT(LL_ERROR, "%s: error: sample rate mismatch: fs=%d filter_fs=%d", ei->name, istream->fs, c->fs);
				destroy_codec(c);
				return NULL;
			}
			else LOG_FMT(LL_VERBOSE, "%s: info: ignoring sample rate mismatch: fs=%d filter_fs=%d", ei->name, istream->fs, c->fs);
		}
		data = calloc(c->frames * c->channels, sizeof(sample_t));
		if (c->read(c, data, c->frames) != c->frames) {
			LOG_FMT(LL_ERROR, "%s: error: short read", ei->name);
			destroy_codec(c);
			free(data);
			return NULL;
		}
		destroy_codec(c);
	}
	return data;
}

int fir_parse_opts(const struct effect_info *ei, const struct stream_info *istream, struct codec_params *p, struct dsp_getopt_state *g, int argc, const char *const *argv, const char *optstr,
	int (*extra_opts_fn)(const struct effect_info *, const struct stream_info *, const struct codec_params *, int, const char *, void *), void *extra_opts_data)
{
	int opt, err;
	char *endptr;

	*p = (struct codec_params) CODEC_PARAMS_AUTO(NULL, CODEC_MODE_READ);
	p->fs = istream->fs;
	p->channels = istream->channels;
	if (optstr == NULL) optstr = FIR_INPUT_CODEC_OPTS;

	while ((opt = dsp_getopt(g, argc, argv, optstr)) != -1) {
		switch (opt) {
		case 't': p->type   = g->arg; break;
		case 'e': p->enc    = g->arg; break;
		case 'B': p->endian = CODEC_ENDIAN_BIG;    break;
		case 'L': p->endian = CODEC_ENDIAN_LITTLE; break;
		case 'N': p->endian = CODEC_ENDIAN_LITTLE; break;
		case 'r':
			if (strcmp(g->arg, "any") == 0)
				p->fs = 0;
			else {
				p->fs = lround(parse_freq(g->arg, &endptr));
				if (check_endptr(ei->name, g->arg, endptr, "sample rate"))
					return 1;
				if (p->fs <= 0) {
					LOG_FMT(LL_ERROR, "%s: error: sample rate must be > 0", ei->name);
					return 1;
				}
				if (p->fs != istream->fs) {
					LOG_FMT(LL_ERROR, "%s: error: sample rate mismatch: stream_fs=%d requested_fs=%d", ei->name, istream->fs, p->fs);
					return 1;
				}
			}
			break;
		case 'c':
			p->channels = strtol(g->arg, &endptr, 10);
			if (check_endptr(ei->name, g->arg, endptr, "number of channels"))
				return 1;
			if (p->channels <= 0) {
				LOG_FMT(LL_ERROR, "%s: error: number of channels must be > 0", ei->name);
				return 1;
			}
			break;
		case ':':
			LOG_FMT(LL_ERROR, "%s: error: expected argument to option '%c'", ei->name, g->opt);
			return 1;
		default:
			if (opt == '?' || extra_opts_fn == NULL) {
				LOG_FMT(LL_ERROR, "%s: error: illegal option '%c'", ei->name, g->opt);
				return 1;
			}
			else if ((err = extra_opts_fn(ei, istream, p, opt, g->arg, extra_opts_data)) != 0)
				return err;
		}
	}
	return 0;
}
