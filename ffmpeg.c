#include <stdlib.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include "ffmpeg.h"

struct ffmpeg_state {
	AVFormatContext *container;
	AVCodecContext *cc;
	AVAudioResampleContext *avr;
	AVFrame *frame;
	AVPacket packet;
	int bytes, stream_index;
};

static int av_registered = 0;

ssize_t ffmpeg_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	sample_t *buf_ptr;
	int avail, len, got_frame = 0, done = 0;
	ssize_t buf_pos = 0, samples = frames * c->channels;
	avail = avresample_available(state->avr);
	while (buf_pos < samples && !(done && avail == 0)) {
		if (avail == 0) {
			skip_frame:
			if (state->packet.size <= 0) {
				av_free_packet(&state->packet);
				if (av_read_frame(state->container, &state->packet) < 0) {
					done = 1;
					continue;
				}
			}
			if (state->packet.stream_index == state->stream_index) {
				got_frame = 0;
				if ((len = avcodec_decode_audio4(state->cc, state->frame, &got_frame, &state->packet)) < 0) {
					state->packet.size = 0;
					goto skip_frame;
				}
				state->packet.size -= len;
				state->packet.data += len;
				if (!got_frame)
					goto skip_frame;

				avresample_convert(state->avr, NULL, 0, 0, state->frame->data, state->frame->linesize[0], state->frame->nb_samples);
				av_frame_unref(state->frame);
			}
			else
				state->packet.size = 0;
		}
		else {
			avail = (avail > (samples - buf_pos) / c->channels) ? (samples - buf_pos) / c->channels : avail;
			buf_ptr = &buf[buf_pos];
			avail = avresample_read(state->avr, (uint8_t **) &buf_ptr, avail);
			buf_pos += avail * c->channels;
		}
		avail = avresample_available(state->avr);
	}
	return buf_pos / c->channels;
}

ssize_t ffmpeg_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	return 0;
}

ssize_t ffmpeg_seek(struct codec *c, ssize_t pos)
{
	AVRational time_base;
	int64_t timestamp;
	struct ffmpeg_state *state = (struct ffmpeg_state *) c->data;
	if (pos < 0)
		pos = 0;
	else if (pos >= c->frames)
		pos = c->frames - 1;
	time_base.num = state->container->streams[state->stream_index]->time_base.num;
	time_base.den = state->container->streams[state->stream_index]->time_base.den;
	timestamp = pos * time_base.den / time_base.num / c->fs;
	if (av_seek_frame(state->container, state->stream_index, timestamp, AVSEEK_FLAG_FRAME) < 0)
		return -1;
	/* drop any pending frames in the output FIFO */
	avresample_close(state->avr);
	avresample_open(state->avr);
	return pos;
}

ssize_t ffmpeg_delay(struct codec *c)
{
	return 0;
}

void ffmpeg_reset(struct codec *c)
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
	av_free_packet(&state->packet);
	av_frame_free(&state->frame);
	avresample_free(&state->avr);
	avformat_close_input(&state->container);
	free(state);
	free((char *) c->type);
}

struct codec * ffmpeg_codec_init(const char *type, int mode, const char *path, const char *enc, int endian, int rate, int channels)
{
	int i, err;
	struct ffmpeg_state *state = NULL;
	struct codec *c = NULL;
	AVCodec *codec = NULL;
	AVRational time_base;

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
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to open %s: %s: %s\n", (mode == CODEC_MODE_WRITE) ? "output" : "input", path, av_err2str(err));
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

	/* open codec */
	state->cc = state->container->streams[state->stream_index]->codec;
	codec = avcodec_find_decoder(state->cc->codec_id);
	if ((err = avcodec_open2(state->cc, codec, NULL)) < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: could not open required decoder: %s\n", av_err2str(err));
		goto fail;
	}
	state->frame = av_frame_alloc();
	if (state->frame == NULL) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: failed to allocate frame\n");
		goto fail;
	}

	/* set up avresample */
	state->avr = avresample_alloc_context();
	av_opt_set_int(state->avr, "in_channel_layout", state->cc->channel_layout, 0);
	av_opt_set_int(state->avr, "out_channel_layout", state->cc->channel_layout, 0);
	av_opt_set_int(state->avr, "in_sample_rate", state->cc->sample_rate, 0);
	av_opt_set_int(state->avr, "out_sample_rate", state->cc->sample_rate, 0);
	av_opt_set_int(state->avr, "in_sample_fmt", state->cc->sample_fmt, 0);
	av_opt_set_int(state->avr, "out_sample_fmt", AV_SAMPLE_FMT_DBL, 0);
	if ((err = avresample_open(state->avr)) < 0) {
		LOG(LL_ERROR, "dsp: ffmpeg: error: could not open audio resample context: %s\n", av_err2str(err));
		goto fail;
	}
	state->bytes = av_get_bytes_per_sample(state->cc->sample_fmt);

	c = calloc(1, sizeof(struct codec));
	i = strlen(codec->name) + 8;
	c->type = calloc(1, i);
	snprintf((char *) c->type, i, "ffmpeg/%s", codec->name);
	c->enc = av_get_sample_fmt_name(state->cc->sample_fmt);
	c->path = path;
	c->fs = state->cc->sample_rate;
	switch (state->cc->sample_fmt) {
		case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  c->prec = 8; break;
		case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: c->prec = 16; break;
		case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: c->prec = 32; break;
		case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: c->prec = 24; break;
		case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: c->prec = 53; break;
		default: c->prec = 16;
	}
	c->channels = state->cc->channels;
	time_base.num = state->container->streams[state->stream_index]->time_base.num;
	time_base.den = state->container->streams[state->stream_index]->time_base.den;
	c->frames = state->container->streams[state->stream_index]->duration * time_base.num * c->fs / time_base.den;
	c->read = ffmpeg_read;
	c->write = ffmpeg_write;
	c->seek = ffmpeg_seek;
	c->delay = ffmpeg_delay;
	c->reset = ffmpeg_reset;
	c->pause = ffmpeg_pause;
	c->destroy = ffmpeg_destroy;
	c->data = state;

	return c;

	fail:
	if (state->avr != NULL)
		avresample_free(&state->avr);
	if (state->frame != NULL)
		av_frame_free(&state->frame);
	if (state->container != NULL)
		avformat_close_input(&state->container);
	free(state);
	return NULL;
}

void ffmpeg_codec_print_encodings(const char *type)
{
	fprintf(stdout, " <autodetected>");
}
