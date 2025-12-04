/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/version.h>
#include <pthread.h>
#include "ffmpeg.h"
#include "sampleconv.h"
#include "dlsym.h"
#include "util.h"

struct ffmpeg_state {
	AVFormatContext *container;
	AVCodecContext *cc;
	AVFrame *frame;
	void (*read_func)(void *, sample_t *, ssize_t);
	void (*readp_func)(void **, sample_t *, int, ssize_t, ssize_t);
	int planar, bytes, stream_index, got_frame;
	ssize_t frame_pos;
	int64_t last_ts;
};

static const char codec_name[] = "ffmpeg";

static pthread_mutex_t ffmpeg_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int ffmpeg_open_count = 0;
void *dl_handle_libavcodec  = NULL;
void *dl_handle_libavformat = NULL;
void *dl_handle_libavutil   = NULL;

/* libavcodec symbols */
DLSYM_PROTOTYPE(avcodec_find_decoder);
DLSYM_PROTOTYPE(avcodec_alloc_context3);
DLSYM_PROTOTYPE(avcodec_parameters_to_context);
DLSYM_PROTOTYPE(avcodec_open2);
DLSYM_PROTOTYPE(avcodec_receive_frame);
DLSYM_PROTOTYPE(avcodec_send_packet);
DLSYM_PROTOTYPE(avcodec_flush_buffers);
DLSYM_PROTOTYPE(avcodec_free_context);
DLSYM_PROTOTYPE(av_packet_unref);

/* libavformat symbols */
DLSYM_PROTOTYPE(avformat_open_input);
DLSYM_PROTOTYPE(avformat_find_stream_info);
DLSYM_PROTOTYPE(av_find_best_stream);
DLSYM_PROTOTYPE(av_read_frame);
DLSYM_PROTOTYPE(avformat_seek_file);
DLSYM_PROTOTYPE(avformat_close_input);

/* libavutil symbols */
DLSYM_PROTOTYPE(av_log_set_level);
DLSYM_PROTOTYPE(av_strerror);
DLSYM_PROTOTYPE(av_frame_alloc);
DLSYM_PROTOTYPE(av_sample_fmt_is_planar);
DLSYM_PROTOTYPE(av_get_bytes_per_sample);
DLSYM_PROTOTYPE(av_get_sample_fmt_name);
DLSYM_PROTOTYPE(av_rescale);
DLSYM_PROTOTYPE(av_frame_free);
DLSYM_PROTOTYPE(av_frame_unref);

/* Planar sample conversion functions */

static void read_buf_u8p(void **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	uint8_t **inn = (uint8_t **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = U8_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_s16p(void **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	int16_t **inn = (int16_t **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = S16_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_s32p(void **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	int32_t **inn = (int32_t **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = S32_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_floatp(void **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	float **inn = (float **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = FLOAT_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_doublep(void **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	double **inn = (double **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = DOUBLE_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static char * ffmpeg_get_err_str(int err, char *err_buf)
{
	sym_av_strerror(err, err_buf, AV_ERROR_MAX_STRING_SIZE);
	return err_buf;
}

#define FFMPEG_ERRSTR(err) ffmpeg_get_err_str(err, (char [AV_ERROR_MAX_STRING_SIZE]){0})

static int get_new_frame(struct ffmpeg_state *state)
{
	AVPacket packet;
	int err;
	if (state->got_frame)
		sym_av_frame_unref(state->frame);
	state->got_frame = 0;
	state->frame_pos = 0;
	retry:
	if ((err = sym_avcodec_receive_frame(state->cc, state->frame)) < 0) {
		switch (err) {
		case AVERROR_EOF:
			return -1;
		case AVERROR(EAGAIN):
			skip_packet:
			if (sym_av_read_frame(state->container, &packet) < 0) {
				sym_avcodec_send_packet(state->cc, NULL);  /* send flush packet */
				goto retry;
			}
			if (packet.stream_index != state->stream_index) {
				sym_av_packet_unref(&packet);
				goto skip_packet;
			}
			state->last_ts = (packet.pts == AV_NOPTS_VALUE) ? packet.dts : packet.pts;
			if ((err = sym_avcodec_send_packet(state->cc, &packet)) < 0) {
				sym_av_packet_unref(&packet);
				if (err == AVERROR_INVALIDDATA) {
					LOG_FMT(LL_VERBOSE, "%s: warning: skipping invalid data", codec_name);
					goto skip_packet;
				}
				goto print_error;
			}
			sym_av_packet_unref(&packet);
			break;
		default:
			print_error:
			LOG_FMT(LL_ERROR, "%s: decoding error: %s", codec_name, FFMPEG_ERRSTR(err));
			return 1;
		}
	}
	state->got_frame = 1;
	return 0;
}

ssize_t ffmpeg_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	int r, done = 0;  /* set to 1 at EOF */
	ssize_t buf_pos = 0, avail = 0;
	if (state->got_frame)
		avail = state->frame->nb_samples - state->frame_pos;
	while (buf_pos < frames && !(done && avail == 0)) {
		if (avail == 0) {
			r = get_new_frame(state);
			if (r < 0) {
				done = 1;
				continue;
			}
			else if (r > 0)
				return 0;
		}
		if (state->got_frame) {
			avail = (avail > frames - buf_pos) ? frames - buf_pos : avail;
			if (state->planar)
				state->readp_func((void **) state->frame->extended_data, &buf[buf_pos * c->channels],
					c->channels, state->frame_pos, avail);
			else
				state->read_func(&state->frame->extended_data[0][state->frame_pos * state->bytes * c->channels],
					&buf[buf_pos * c->channels], avail * c->channels);
			buf_pos += avail;
			state->frame_pos += avail;
			avail = state->frame->nb_samples - state->frame_pos;
		}
	}
	return buf_pos;
}

ssize_t ffmpeg_seek(struct codec *c, ssize_t pos)
{
	AVStream *st;
	int64_t seek_ts;
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	if (c->frames == -1)
		return -1;
	if (pos < 0)
		pos = 0;
	else if (pos >= c->frames)
		pos = c->frames - 1;
	st = state->container->streams[state->stream_index];
	seek_ts = sym_av_rescale(pos, st->time_base.den, st->time_base.num) / c->fs;
	if (sym_avformat_seek_file(state->container, state->stream_index, INT64_MIN, seek_ts, INT64_MAX, 0) < 0)
		return -1;
	sym_avcodec_flush_buffers(state->cc);
	get_new_frame(state);
	pos = sym_av_rescale(state->last_ts, st->time_base.num * c->fs, st->time_base.den);
	return pos;
}

ssize_t ffmpeg_delay(struct codec *c)
{
	return 0;
}

void ffmpeg_drop(struct codec *c)
{
	/* do nothing */
}

void ffmpeg_pause(struct codec *c, int p)
{
	/* do nothing */
}

static void ffmpeg_dl_cleanup(void)
{
	if (dl_handle_libavcodec) dlclose(dl_handle_libavcodec);
	if (dl_handle_libavformat) dlclose(dl_handle_libavformat);
	if (dl_handle_libavutil) dlclose(dl_handle_libavutil);
}

void ffmpeg_destroy(struct codec *c)
{
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	sym_av_frame_free(&state->frame);
	sym_avcodec_free_context(&state->cc);
	sym_avformat_close_input(&state->container);
	free(state);
	free((char *) c->type);
	pthread_mutex_lock(&ffmpeg_init_lock);
	--ffmpeg_open_count;
	if (ffmpeg_open_count == 0)
		ffmpeg_dl_cleanup();
	pthread_mutex_unlock(&ffmpeg_init_lock);
}

struct codec * ffmpeg_codec_init(const struct codec_params *p)
{
	int i, err;
	struct ffmpeg_state *state = NULL;
	struct codec *c = NULL;
	AVStream *st;
	const AVCodec *codec = NULL;

	pthread_mutex_lock(&ffmpeg_init_lock);
	if (ffmpeg_open_count == 0) {
		int dl_fail = 0, dlopen_flags = RTLD_LAZY|RTLD_GLOBAL;

		if (!(dl_handle_libavcodec = try_dlopen("libavcodec.so." XSTR(LIBAVCODEC_VERSION_MAJOR), dlopen_flags))) dl_fail = 1;
		if (!(dl_handle_libavformat = try_dlopen("libavformat.so." XSTR(LIBAVFORMAT_VERSION_MAJOR), dlopen_flags))) dl_fail = 1;
		if (!(dl_handle_libavutil = try_dlopen("libavutil.so." XSTR(LIBAVUTIL_VERSION_MAJOR), dlopen_flags))) dl_fail = 1;

		if (dl_fail) {
			dl_failed:
			ffmpeg_dl_cleanup();
			pthread_mutex_unlock(&ffmpeg_init_lock);
			return NULL;
		}

		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_find_decoder)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_alloc_context3)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_parameters_to_context)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_open2)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_receive_frame)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_send_packet)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_flush_buffers)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, avcodec_free_context)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavcodec, av_packet_unref)) dl_fail = 1;

		if (!DLSYM_RESOLVE(dl_handle_libavformat, avformat_open_input)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavformat, avformat_find_stream_info)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavformat, av_find_best_stream)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavformat, av_read_frame)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavformat, avformat_seek_file)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavformat, avformat_close_input)) dl_fail = 1;

		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_log_set_level)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_strerror)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_frame_alloc)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_sample_fmt_is_planar)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_get_bytes_per_sample)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_get_sample_fmt_name)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_rescale)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_frame_free)) dl_fail = 1;
		if (!DLSYM_RESOLVE(dl_handle_libavutil, av_frame_unref)) dl_fail = 1;

		if (dl_fail) goto dl_failed;

		++ffmpeg_open_count;
		if (LOGLEVEL(LL_VERBOSE))
			sym_av_log_set_level(AV_LOG_VERBOSE);
		else if (LOGLEVEL(LL_SILENT))
			sym_av_log_set_level(AV_LOG_QUIET);
	}
	pthread_mutex_unlock(&ffmpeg_init_lock);

	/* open input and find stream info */
	state = calloc(1, sizeof(struct ffmpeg_state));
	if ((err = sym_avformat_open_input(&state->container, p->path, NULL, NULL)) < 0) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open input: %s: %s", codec_name, p->path, FFMPEG_ERRSTR(err));
		goto fail;
	}
	if ((err = sym_avformat_find_stream_info(state->container, NULL)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not find stream info: %s", codec_name, FFMPEG_ERRSTR(err));
		goto fail;
	}

	/* find audio stream */
	state->stream_index = sym_av_find_best_stream(state->container, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (state->stream_index < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not find an audio stream", codec_name);
		goto fail;
	}
	st = state->container->streams[state->stream_index];

	/* open codec */
	codec = sym_avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		LOG_FMT(LL_ERROR, "%s: error: failed to find decoder", codec_name);
		goto fail;
	}
	state->cc = sym_avcodec_alloc_context3(codec);
	if (!state->cc) {
		LOG_FMT(LL_ERROR, "%s: error: failed to allocate codec context", codec_name);
		goto fail;
	}
	if ((err = sym_avcodec_parameters_to_context(state->cc, st->codecpar)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to copy codec parameters to decoder context: %s", codec_name, FFMPEG_ERRSTR(err));
		goto fail;
	}
	if ((err = sym_avcodec_open2(state->cc, codec, NULL)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not open required decoder: %s", codec_name, FFMPEG_ERRSTR(err));
		goto fail;
	}

	state->frame = sym_av_frame_alloc();
	if (state->frame == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: failed to allocate frame", codec_name);
		goto fail;
	}
	state->planar = sym_av_sample_fmt_is_planar(state->cc->sample_fmt);
	state->bytes = sym_av_get_bytes_per_sample(state->cc->sample_fmt);

	c = calloc(1, sizeof(struct codec));
	i = strlen(codec->name) + 8;
	c->path = p->path;
	c->type = calloc(1, i);
	snprintf((char *) c->type, i, "ffmpeg/%s", codec->name);
	c->enc = sym_av_get_sample_fmt_name(state->cc->sample_fmt);
	c->fs = state->cc->sample_rate;
	#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
		c->channels = state->cc->ch_layout.nb_channels;
	#else
		c->channels = state->cc->channels;
	#endif
	switch (state->cc->sample_fmt) {
	case AV_SAMPLE_FMT_U8:
	case AV_SAMPLE_FMT_U8P:
		c->prec = 8;
		c->hints |= CODEC_HINT_CAN_DITHER;
		state->read_func = read_buf_u8;
		state->readp_func = read_buf_u8p;
		break;
	case AV_SAMPLE_FMT_S16:
	case AV_SAMPLE_FMT_S16P:
		c->prec = 16;
		c->hints |= CODEC_HINT_CAN_DITHER;
		state->read_func = read_buf_s16;
		state->readp_func = read_buf_s16p;
		break;
	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_S32P:
		c->prec = 32;
		c->hints |= CODEC_HINT_CAN_DITHER;
		state->read_func = read_buf_s32;
		state->readp_func = read_buf_s32p;
		break;
	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_FLTP:
		c->prec = 24;
		state->read_func = read_buf_float;
		state->readp_func = read_buf_floatp;
		break;
	case AV_SAMPLE_FMT_DBL:
	case AV_SAMPLE_FMT_DBLP:
		c->prec = 53;
		state->read_func = read_buf_double;
		state->readp_func = read_buf_doublep;
		break;
	default:
		LOG_FMT(LL_ERROR, "%s: error: unhandled sample format", codec_name);
		goto fail;
	}
	if (st->duration != AV_NOPTS_VALUE)
		c->frames = sym_av_rescale(st->duration, st->time_base.num * c->fs, st->time_base.den);
	else if (state->container->duration != AV_NOPTS_VALUE)
		c->frames = sym_av_rescale(state->container->duration, c->fs, AV_TIME_BASE);
	else
		c->frames = -1;
	c->read = ffmpeg_read;
	c->seek = ffmpeg_seek;
	c->delay = ffmpeg_delay;
	c->drop = ffmpeg_drop;
	c->pause = ffmpeg_pause;
	c->destroy = ffmpeg_destroy;
	c->data = state;

	return c;

	fail:
	free(c);
	if (state->frame)
		sym_av_frame_free(&state->frame);
	if (state->cc)
		sym_avcodec_free_context(&state->cc);
	if (state->container)
		sym_avformat_close_input(&state->container);
	free(state);
	return NULL;
}

void ffmpeg_codec_print_encodings(const char *type)
{
	fprintf(stdout, " <autodetected>");
}
