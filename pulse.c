#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include "pulse.h"
#include "util.h"
#include "sampleconv.h"

struct pulse_enc_info {
	const char *name;
	int fmt, bytes, prec, can_dither;
	void (*write_func)(sample_t *, char *, ssize_t);
	void (*read_func)(char *, sample_t *, ssize_t);
};

struct pulse_state {
	pa_simple *s;
	struct pulse_enc_info *enc_info;
	char *buf;
	ssize_t buf_frames;
};

ssize_t pulse_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	int err;
	struct pulse_state *state = (struct pulse_state *) c->data;

	if (pa_simple_read(state->s, (char *) buf, frames * c->channels * state->enc_info->bytes, &err) < 0) {
		LOG(LL_ERROR, "dsp: pulse: read: error: %s\n", pa_strerror(err));
		return 0;
	}
	state->enc_info->read_func((char *) buf, buf, frames * c->channels);
	return frames;
}

ssize_t pulse_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	ssize_t n, i = 0;
	int err;
	struct pulse_state *state = (struct pulse_state *) c->data;

	while (i < frames) {
		n = (frames - i > state->buf_frames) ? state->buf_frames : frames - i;
		state->enc_info->write_func(&buf[i * c->channels], state->buf, n * c->channels);
		if (pa_simple_write(state->s, state->buf, n * c->channels * state->enc_info->bytes, &err) < 0) {
			LOG(LL_ERROR, "dsp: pulse: write: error: %s\n", pa_strerror(err));
			return i;
		}
		i += n;
	}
	return i;
}

ssize_t pulse_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t pulse_delay(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	return pa_simple_get_latency(state->s, NULL) * c->fs / 1000000;
}

void pulse_drop(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	pa_simple_flush(state->s, NULL);
}

void pulse_pause(struct codec *c, int p)
{
	/* do nothing */
}

void pulse_destroy(struct codec *c)
{
	struct pulse_state *state = (struct pulse_state *) c->data;
	pa_simple_drain(state->s, NULL);
	pa_simple_free(state->s);
	free(state->buf);
	free(state);
}

static struct pulse_enc_info encodings[] = {
	{ "s16",   PA_SAMPLE_S16NE,     2, 16, 1, write_buf_s16,   read_buf_s16 },
	{ "u8",    PA_SAMPLE_U8,        1,  8, 1, write_buf_u8,    read_buf_u8 },
	{ "s24",   PA_SAMPLE_S24_32NE,  4, 24, 1, write_buf_s24,   read_buf_s24 },
	{ "s24_3", PA_SAMPLE_S24NE,     3, 24, 1, write_buf_s24_3, read_buf_s24_3 },
	{ "s32",   PA_SAMPLE_S32NE,     4, 32, 1, write_buf_s32,   read_buf_s32 },
	{ "float", PA_SAMPLE_FLOAT32NE, 4, 24, 0, write_buf_float, read_buf_float },
};

static struct pulse_enc_info * pulse_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

struct codec * pulse_codec_init(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	int err;
	pa_simple *s;
	pa_sample_spec ss;
	struct codec *c;
	struct pulse_state *state;
	struct pulse_enc_info *enc_info;

	if ((enc_info = pulse_get_enc_info(enc)) == NULL) {
		LOG(LL_ERROR, "dsp: pulse: error: bad encoding: %s\n", enc);
		return NULL;
	}
	ss.format = enc_info->fmt;
	ss.channels = channels;
	ss.rate = fs;
	s = pa_simple_new(NULL, "dsp", (mode == CODEC_MODE_WRITE) ? PA_STREAM_PLAYBACK : PA_STREAM_RECORD,
		(strcmp(path, "default") == 0) ? NULL : path, "dsp", &ss, NULL, NULL, &err);
	if (s == NULL) {
		LOG(LL_OPEN_ERROR, "dsp: pulse: failed to open device: %s\n", pa_strerror(err));
		return NULL;
	}

	state = calloc(1, sizeof(struct pulse_state));
	state->s = s;
	state->enc_info = enc_info;
	if (mode == CODEC_MODE_WRITE) {
		state->buf = calloc(dsp_globals.buf_frames * channels, enc_info->bytes);
		state->buf_frames = dsp_globals.buf_frames;
	}

	c = calloc(1, sizeof(struct codec));
	c->path = path;
	c->type = type;
	c->enc = enc_info->name;
	c->fs = fs;
	c->channels = channels;
	c->prec = enc_info->prec;
	c->interactive = (mode == CODEC_MODE_WRITE) ? 1 : 0;
	c->frames = -1;
	c->read = pulse_read;
	c->write = pulse_write;
	c->seek = pulse_seek;
	c->delay = pulse_delay;
	c->drop = pulse_drop;
	c->pause = pulse_pause;
	c->destroy = pulse_destroy;
	c->data = state;

	return c;
}

void pulse_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
