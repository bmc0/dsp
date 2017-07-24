#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include "alsa.h"
#include "util.h"
#include "sampleconv.h"

struct alsa_enc_info {
	const char *name;
	snd_pcm_format_t fmt;
	int bytes, prec, can_dither;
	void (*write_func)(sample_t *, char *, ssize_t);
	void (*read_func)(char *, sample_t *, ssize_t);
};

struct alsa_state {
	snd_pcm_t *dev;
	struct alsa_enc_info *enc_info;
	snd_pcm_sframes_t delay;
	char *buf;
	ssize_t buf_frames;
};

ssize_t alsa_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	ssize_t n;
	struct alsa_state *state = (struct alsa_state *) c->data;

	try_again:
	n = snd_pcm_readi(state->dev, (char *) buf, frames);
	if (n < 0) {
		if (n == -EPIPE)
			LOG(LL_ERROR, "dsp: alsa: warning: overrun occurred\n");
		n = snd_pcm_recover(state->dev, n, 1);
		if (n < 0) {
			LOG(LL_ERROR, "dsp: alsa: error: read failed\n");
			return 0;
		}
		else
			goto try_again;
	}
	state->enc_info->read_func((char *) buf, buf, n * c->channels);
	return n;
}

ssize_t alsa_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	ssize_t n, i = 0;
	struct alsa_state *state = (struct alsa_state *) c->data;

	while (i < frames) {
		n = (frames - i > state->buf_frames) ? state->buf_frames : frames - i;
		state->enc_info->write_func(&buf[i * c->channels], state->buf, n * c->channels);
		try_again:
		n = snd_pcm_writei(state->dev, state->buf, n);
		if (n < 0) {
			if (n == -EPIPE)
				LOG(LL_ERROR, "dsp: alsa: warning: underrun occurred\n");
			n = snd_pcm_recover(state->dev, n, 1);
			if (n < 0) {
				LOG(LL_ERROR, "dsp: alsa: error: write failed\n");
				return i;
			}
			else
				goto try_again;
		}
		i += n;
	}
	return i;
}

ssize_t alsa_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t alsa_delay(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) == SND_PCM_STATE_PAUSED)
		return state->delay;
	snd_pcm_delay(state->dev, &state->delay);
	return state->delay;
}

void alsa_drop(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	snd_pcm_drop(state->dev);
	snd_pcm_prepare(state->dev);
	state->delay = 0;
}

void alsa_pause(struct codec *c, int p)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) != SND_PCM_STATE_PAUSED)
		snd_pcm_delay(state->dev, &state->delay);
	snd_pcm_pause(state->dev, p);
}

void alsa_destroy(struct codec *c)
{
	struct alsa_state *state = (struct alsa_state *) c->data;
	if (snd_pcm_state(state->dev) == SND_PCM_STATE_RUNNING)
		snd_pcm_drain(state->dev);
	snd_pcm_close(state->dev);
	free(state->buf);
	free(state);
}

static struct alsa_enc_info encodings[] = {
	{ "s16",    SND_PCM_FORMAT_S16,     2, 16, 1, write_buf_s16,    read_buf_s16 },
	{ "u8",     SND_PCM_FORMAT_U8,      1, 8,  1, write_buf_u8,     read_buf_u8 },
	{ "s8",     SND_PCM_FORMAT_S8,      1, 8,  1, write_buf_s8,     read_buf_s8 },
	{ "s24",    SND_PCM_FORMAT_S24,     4, 24, 1, write_buf_s24,    read_buf_s24 },
	{ "s24_3",  SND_PCM_FORMAT_S24_3LE, 3, 24, 1, write_buf_s24_3,  read_buf_s24_3 },
	{ "s32",    SND_PCM_FORMAT_S32,     4, 32, 1, write_buf_s32,    read_buf_s32 },
	{ "float",  SND_PCM_FORMAT_FLOAT,   4, 24, 0, write_buf_float,  read_buf_float },
	{ "double", SND_PCM_FORMAT_FLOAT64, 8, 53, 0, write_buf_double, read_buf_double },
};

static struct alsa_enc_info * alsa_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

struct codec * alsa_codec_init(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	int err;
	snd_pcm_t *dev = NULL;
	snd_pcm_hw_params_t *p = NULL;
	struct codec *c = NULL;
	struct alsa_state *state = NULL;
	snd_pcm_uframes_t buf_frames;
	struct alsa_enc_info *enc_info;

	if ((err = snd_pcm_open(&dev, path, (mode == CODEC_MODE_WRITE) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		LOG(LL_OPEN_ERROR, "dsp: alsa: error: failed to open device: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((enc_info = alsa_get_enc_info(enc)) == NULL) {
		LOG(LL_ERROR, "dsp: alsa: error: bad encoding: %s\n", enc);
		goto fail;
	}
	if ((err = snd_pcm_hw_params_malloc(&p)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to allocate hw params: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_any(dev, p)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to initialize hw params: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_access(dev, p, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set access: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_format(dev, p, enc_info->fmt)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set format: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_rate(dev, p, (unsigned int) fs, 0)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set rate: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params_set_channels(dev, p, channels)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set channels: %s\n", snd_strerror(err));
		goto fail;
	}
	buf_frames = dsp_globals.buf_frames;
	if ((err = snd_pcm_hw_params_set_buffer_size_min(dev, p, &buf_frames)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set buffer size minimum: %s\n", snd_strerror(err));
		goto fail;
	}
	buf_frames = dsp_globals.buf_frames * dsp_globals.max_buf_ratio;
	if ((err = snd_pcm_hw_params_set_buffer_size_max(dev, p, &buf_frames)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set buffer size maximum: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_hw_params(dev, p)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to set params: %s\n", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_prepare(dev)) < 0) {
		LOG(LL_ERROR, "dsp: alsa: error: failed to prepare device: %s\n", snd_strerror(err));
		goto fail;
	}

	state = calloc(1, sizeof(struct alsa_state));
	state->dev = dev;
	state->enc_info = enc_info;
	state->delay = 0;
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
	c->can_dither = enc_info->can_dither;
	c->interactive = (mode == CODEC_MODE_WRITE) ? 1 : 0;
	c->frames = -1;
	c->read = alsa_read;
	c->write = alsa_write;
	c->seek = alsa_seek;
	c->delay = alsa_delay;
	c->drop = alsa_drop;
	c->pause = alsa_pause;
	c->destroy = alsa_destroy;
	c->data = state;

	snd_pcm_hw_params_free(p);

	return c;

	fail:
	if (p != NULL)
		snd_pcm_hw_params_free(p);
	if (dev != NULL)
		snd_pcm_close(dev);
	return NULL;
}

void alsa_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
