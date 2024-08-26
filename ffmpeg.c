/*
 * This file is part of dsp.
 *
 * Copyright (c) 2014-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "ffmpeg.h"
#include "sampleconv.h"

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
static int av_initialized = 0;

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

static int get_new_frame(struct ffmpeg_state *state)
{
	AVPacket packet;
	int r;
	if (state->got_frame)
		av_frame_unref(state->frame);
	state->got_frame = 0;
	state->frame_pos = 0;
	retry:
	if ((r = avcodec_receive_frame(state->cc, state->frame)) < 0) {
		switch (r) {
		case AVERROR_EOF:
			return -1;
		case AVERROR(EAGAIN):
			skip_packet:
			if (av_read_frame(state->container, &packet) < 0) {
				avcodec_send_packet(state->cc, NULL);  /* send flush packet */
				goto retry;
			}
			if (packet.stream_index != state->stream_index) {
				av_packet_unref(&packet);
				goto skip_packet;
			}
			state->last_ts = (packet.pts == AV_NOPTS_VALUE) ? packet.dts : packet.pts;
			if (avcodec_send_packet(state->cc, &packet) < 0)
				return 1;  /* FIXME: handle decoding errors more intelligently */
			av_packet_unref(&packet);
			break;
		default:
			return 1;  /* FIXME: handle decoding errors more intelligently */
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
	seek_ts = av_rescale(pos, st->time_base.den, st->time_base.num) / c->fs;
	if (avformat_seek_file(state->container, state->stream_index, INT64_MIN, seek_ts, INT64_MAX, 0) < 0)
		return -1;
	avcodec_flush_buffers(state->cc);
	get_new_frame(state);
	pos = av_rescale(state->last_ts, st->time_base.num * c->fs, st->time_base.den);
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

void ffmpeg_destroy(struct codec *c)
{
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	av_frame_free(&state->frame);
	avcodec_free_context(&state->cc);
	avformat_close_input(&state->container);
	free(state);
	free((char *) c->type);
}

struct codec * ffmpeg_codec_init(const struct codec_params *p)
{
	int i, err;
	struct ffmpeg_state *state = NULL;
	struct codec *c = NULL;
	AVStream *st;
	const AVCodec *codec = NULL;

	if (!av_initialized) {
		if (LOGLEVEL(LL_VERBOSE))
			av_log_set_level(AV_LOG_VERBOSE);
		else if (LOGLEVEL(LL_SILENT))
			av_log_set_level(AV_LOG_QUIET);
		av_initialized = 1;
	}

	/* open input and find stream info */
	state = calloc(1, sizeof(struct ffmpeg_state));
	if ((err = avformat_open_input(&state->container, p->path, NULL, NULL)) < 0) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open input: %s: %s", codec_name, p->path, av_err2str(err));
		goto fail;
	}
	if ((err = avformat_find_stream_info(state->container, NULL)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not find stream info: %s", codec_name, av_err2str(err));
		goto fail;
	}

	/* find audio stream */
	state->stream_index = av_find_best_stream(state->container, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (state->stream_index < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not find an audio stream", codec_name);
		goto fail;
	}
	st = state->container->streams[state->stream_index];

	/* open codec */
	codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		LOG_FMT(LL_ERROR, "%s: error: failed to find decoder", codec_name);
		goto fail;
	}
	state->cc = avcodec_alloc_context3(codec);
	if (!state->cc) {
		LOG_FMT(LL_ERROR, "%s: error: failed to allocate codec context", codec_name);
		goto fail;
	}
	if ((err = avcodec_parameters_to_context(state->cc, st->codecpar)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: failed to copy codec parameters to decoder context: %s", codec_name, av_err2str(err));
		goto fail;
	}
	if ((err = avcodec_open2(state->cc, codec, NULL)) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: could not open required decoder: %s", codec_name, av_err2str(err));
		goto fail;
	}

	state->frame = av_frame_alloc();
	if (state->frame == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: failed to allocate frame", codec_name);
		goto fail;
	}
	state->planar = av_sample_fmt_is_planar(state->cc->sample_fmt);
	state->bytes = av_get_bytes_per_sample(state->cc->sample_fmt);

	c = calloc(1, sizeof(struct codec));
	i = strlen(codec->name) + 8;
	c->path = p->path;
	c->type = calloc(1, i);
	snprintf((char *) c->type, i, "ffmpeg/%s", codec->name);
	c->enc = av_get_sample_fmt_name(state->cc->sample_fmt);
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
		c->frames = av_rescale(st->duration, st->time_base.num * c->fs, st->time_base.den);
	else if (state->container->duration != AV_NOPTS_VALUE)
		c->frames = av_rescale(state->container->duration, c->fs, AV_TIME_BASE);
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
		av_frame_free(&state->frame);
	if (state->cc)
		avcodec_free_context(&state->cc);
	if (state->container)
		avformat_close_input(&state->container);
	free(state);
	return NULL;
}

void ffmpeg_codec_print_encodings(const char *type)
{
	fprintf(stdout, " <autodetected>");
}
