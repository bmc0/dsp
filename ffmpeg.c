#include <stdlib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include "ffmpeg.h"
#include "sampleconv.h"

struct ffmpeg_state {
	AVFormatContext *container;
	AVCodecContext *cc;
	AVFrame *frame;
	void (*read_func)(char *, sample_t *, ssize_t);
	void (*readp_func)(char **, sample_t *, int, ssize_t, ssize_t);
	int planar, bytes, stream_index, got_frame;
	ssize_t frame_pos;
};

static int av_registered = 0;

/* Planar sample conversion functions */

static void read_buf_u8p(char **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	unsigned char **inn = (unsigned char **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = U8_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_s16p(char **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	signed short **inn = (signed short **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = S16_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_s32p(char **in, sample_t *out, int channels, ssize_t start, ssize_t s)
{
	signed int **inn = (signed int **) in;
	int c = channels;
	ssize_t out_s = s * channels;
	while (s-- > 0) {
		while (c-- > 0)
			out[--out_s] = S32_TO_SAMPLE(inn[c][start + s]);
		c = channels;
	}
}

static void read_buf_floatp(char **in, sample_t *out, int channels, ssize_t start, ssize_t s)
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

static void read_buf_doublep(char **in, sample_t *out, int channels, ssize_t start, ssize_t s)
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

ssize_t ffmpeg_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	AVPacket packet;
	int r, done = 0;  /* set to 1 at EOF */
	ssize_t buf_pos = 0, avail = 0;
	if (state->got_frame)
		avail = state->frame->nb_samples - state->frame_pos;
	while (buf_pos < frames && !(done && avail == 0)) {
		if (avail == 0) {
			if (state->got_frame)
				av_frame_unref(state->frame);
			state->got_frame = 0;
			state->frame_pos = 0;
			if ((r = avcodec_receive_frame(state->cc, state->frame)) < 0) {
				switch (r) {
				case AVERROR_EOF:
					done = 1;
					continue;
				case AVERROR(EAGAIN):
					skip_packet:
					if (av_read_frame(state->container, &packet) < 0) {
						avcodec_send_packet(state->cc, NULL);  /* send flush packet */
						continue;
					}
					if (packet.stream_index != state->stream_index) {
						av_packet_unref(&packet);
						goto skip_packet;
					}
					if (avcodec_send_packet(state->cc, &packet) < 0)
						return 0;  /* FIXME: handle decoding errors more intelligently */
					av_packet_unref(&packet);
					break;
				default:
					return 0;  /* FIXME: handle decoding errors more intelligently */
				}
			}
			state->got_frame = 1;
		}
		if (state->got_frame) {
			avail = (avail > frames - buf_pos) ? frames - buf_pos : avail;
			if (state->planar)
				state->readp_func((char **) state->frame->extended_data, &buf[buf_pos * c->channels],
					c->channels, state->frame_pos, avail);
			else
				state->read_func((char *) &state->frame->extended_data[0][state->frame_pos * state->bytes * c->channels],
					&buf[buf_pos * c->channels], avail * c->channels);
			buf_pos += avail;
			state->frame_pos += avail;
			avail = state->frame->nb_samples - state->frame_pos;
		}
	}
	return buf_pos;
}

ssize_t ffmpeg_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	return 0;
}

ssize_t ffmpeg_seek(struct codec *c, ssize_t pos)
{
	AVStream *st;
	int64_t timestamp;
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	if (pos < 0)
		pos = 0;
	else if (pos >= c->frames)
		pos = c->frames - 1;
	st = state->container->streams[state->stream_index];
	timestamp = pos * st->time_base.den / st->time_base.num / c->fs;
	if (av_seek_frame(state->container, state->stream_index, timestamp, AVSEEK_FLAG_FRAME) < 0)
		return -1;
	avcodec_flush_buffers(state->cc);
	if (state->got_frame)
		av_frame_unref(state->frame);
	state->got_frame = 0;
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
	avcodec_close(state->cc);
	avformat_close_input(&state->container);
	free(state);
	free((char *) c->type);
}

struct codec * ffmpeg_codec_init(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	int i, err;
	struct ffmpeg_state *state = NULL;
	struct codec *c = NULL;
	AVStream *st;
	AVCodec *codec = NULL;

	if (!av_registered) {
		if (LOGLEVEL(LL_VERBOSE))
			av_log_set_level(AV_LOG_VERBOSE);
		else if (LOGLEVEL(LL_SILENT))
			av_log_set_level(AV_LOG_QUIET);
		av_register_all();
		av_registered = 1;
	}

	/* open input and find stream info */
	state = calloc(1, sizeof(struct ffmpeg_state));
	if ((err = avformat_open_input(&state->container, path, NULL, NULL)) < 0) {
		LOG(LL_OPEN_ERROR, "dsp: ffmpeg: error: failed to open %s: %s: %s\n",
			(mode == CODEC_MODE_WRITE) ? "output" : "input", path, av_err2str(err));
		goto fail;
	}
	if ((err = avformat_find_stream_info(state->container, NULL)) < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: could not find stream info: %s\n", av_err2str(err));
		goto fail;
	}

	/* find audio stream */
	state->stream_index = av_find_best_stream(state->container, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (state->stream_index < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: could not find an audio stream\n");
		goto fail;
	}
	st = state->container->streams[state->stream_index];

	/* open codec */
	codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to find decoder\n");
		goto fail;
	}
	state->cc = avcodec_alloc_context3(codec);
	if (!state->cc) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to allocate codec context\n");
		goto fail;
	}
	if ((err = avcodec_parameters_to_context(state->cc, st->codecpar)) < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to copy codec parameters to decoder context: %s\n", av_err2str(err));
		goto fail;
	}
	if ((err = avcodec_open2(state->cc, codec, NULL)) < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: could not open required decoder: %s\n", av_err2str(err));
		goto fail;
	}

	state->frame = av_frame_alloc();
	if (state->frame == NULL) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to allocate frame\n");
		goto fail;
	}
	state->planar = av_sample_fmt_is_planar(state->cc->sample_fmt);
	state->bytes = av_get_bytes_per_sample(state->cc->sample_fmt);

	c = calloc(1, sizeof(struct codec));
	i = strlen(codec->name) + 8;
	c->path = path;
	c->type = calloc(1, i);
	snprintf((char *) c->type, i, "ffmpeg/%s", codec->name);
	c->enc = av_get_sample_fmt_name(state->cc->sample_fmt);
	c->fs = state->cc->sample_rate;
	c->channels = state->cc->channels;
	switch (state->cc->sample_fmt) {
	case AV_SAMPLE_FMT_U8:
	case AV_SAMPLE_FMT_U8P:
		c->prec = 8;
		c->can_dither = 1;
		state->read_func = read_buf_u8;
		state->readp_func = read_buf_u8p;
		break;
	case AV_SAMPLE_FMT_S16:
	case AV_SAMPLE_FMT_S16P:
		c->prec = 16;
		c->can_dither = 1;
		state->read_func = read_buf_s16;
		state->readp_func = read_buf_s16p;
		break;
	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_S32P:
		c->prec = 32;
		c->can_dither = 1;
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
		LOG(LL_ERROR, "dsp: ffmpeg: error: unhandled sample format\n");
		goto fail;
	}
	c->frames = st->duration * st->time_base.num * c->fs / st->time_base.den;
	c->read = ffmpeg_read;
	c->write = ffmpeg_write;
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
		avcodec_close(state->cc);
	if (state->container)
		avformat_close_input(&state->container);
	free(state);
	return NULL;
}

void ffmpeg_codec_print_encodings(const char *type)
{
	fprintf(stdout, " <autodetected>");
}
