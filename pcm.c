#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "pcm.h"
#include "util.h"
#include "sampleconv.h"

struct pcm_state {
	int fd;
	struct pcm_enc_info *enc_info;
	ssize_t pos;
};

struct pcm_enc_info {
	const char *name;
	int bytes, prec, can_dither;
	void (*read_func)(char *, sample_t *, ssize_t);
	void (*write_func)(sample_t *, char *, ssize_t);
};

static const char codec_name[] = "pcm";

static struct pcm_enc_info encodings[] = {
	{ "s16",    2, 16, 1, read_buf_s16,    write_buf_s16 },
	{ "u8",     1, 8,  1, read_buf_u8,     write_buf_u8 },
	{ "s8",     1, 8,  1, read_buf_s8,     write_buf_s8 },
	{ "s24",    4, 24, 1, read_buf_s24,    write_buf_s24 },
	{ "s32",    4, 32, 1, read_buf_s32,    write_buf_s32 },
	{ "float",  4, 24, 0, read_buf_float,  write_buf_float },
	{ "double", 8, 53, 0, read_buf_double, write_buf_double },
};

static struct pcm_enc_info * pcm_get_enc_info(const char *enc)
{
	int i;
	if (enc == NULL)
		return &encodings[0];
	for (i = 0; i < LENGTH(encodings); ++i)
		if (strcmp(enc, encodings[i].name) == 0)
			return &encodings[i];
	return NULL;
}

ssize_t pcm_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	ssize_t n;
	struct pcm_state *state = (struct pcm_state *) c->data;

	n = read(state->fd, buf, frames * c->channels * state->enc_info->bytes);
	if (n == -1) {
		LOG_FMT(LL_ERROR, "%s: read failed: %s", codec_name, strerror(errno));
		return 0;
	}
	n = n / state->enc_info->bytes / c->channels;
	state->enc_info->read_func((char *) buf, buf, n * c->channels);
	state->pos += n;
	return n;
}

ssize_t pcm_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	ssize_t n;
	struct pcm_state *state = (struct pcm_state *) c->data;

	state->enc_info->write_func(buf, (char *) buf, frames * c->channels);
	n = write(state->fd, buf, frames * c->channels * state->enc_info->bytes);
	if (n == -1) {
		LOG_FMT(LL_ERROR, "%s: write failed: %s", codec_name, strerror(errno));
		return 0;
	}
	n = n / state->enc_info->bytes / c->channels;
	state->pos += n;
	return n;
}

ssize_t pcm_seek(struct codec *c, ssize_t pos)
{
	off_t o;
	struct pcm_state *state = (struct pcm_state *) c->data;
	if (c->frames == -1)
		return -1;
	if (pos < 0)
		pos = 0;
	else if (pos > c->frames)
		pos = c->frames;
	o = lseek(state->fd, pos * state->enc_info->bytes * c->channels, SEEK_SET);
	if (o == -1)
		return -1;
	state->pos = o;
	return o / state->enc_info->bytes / c->channels;
}

ssize_t pcm_delay(struct codec *c)
{
	return 0;
}

void pcm_drop(struct codec *c)
{
	/* do nothing */
}

void pcm_pause(struct codec *c, int p)
{
	/* do nothing */
}

void pcm_destroy(struct codec *c)
{
	struct pcm_state *state = (struct pcm_state *) c->data;
	close(state->fd);
	free(state);
}

struct codec * pcm_codec_init(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	int fd = -1;
	off_t size;
	struct pcm_enc_info *enc_info;
	struct pcm_state *state = NULL;
	struct codec *c = NULL;

	if ((enc_info = pcm_get_enc_info(enc)) == NULL) {
		LOG_FMT(LL_ERROR, "%s: error: bad encoding: %s", codec_name, enc);
		goto fail;
	}
	if (!(endian == CODEC_ENDIAN_DEFAULT || endian == CODEC_ENDIAN_NATIVE)) {
		LOG_FMT(LL_ERROR, "%s: error: endian conversion not supported", codec_name);
		goto fail;
	}
	if (strcmp(path, "-") == 0)
		fd = (mode == CODEC_MODE_WRITE) ? STDOUT_FILENO : STDIN_FILENO;
	else if ((fd = open(path, (mode == CODEC_MODE_WRITE) ? O_WRONLY|O_CREAT|O_TRUNC : O_RDONLY, 0644)) == -1) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open file: %s: %s", codec_name, path, strerror(errno));
		goto fail;
	}

	state = calloc(1, sizeof(struct pcm_state));
	state->fd = fd;
	state->enc_info = enc_info;

	c = calloc(1, sizeof(struct codec));
	c->path = path;
	c->type = type;
	c->enc = enc_info->name;
	c->fs = fs;
	c->channels = channels;
	c->prec = enc_info->prec;
	c->can_dither = enc_info->can_dither;
	c->frames = -1;
	if (mode == CODEC_MODE_READ) {
		size = lseek(fd, 0, SEEK_END);
		c->frames = (size == -1) ? -1 : size / enc_info->bytes / channels;
		lseek(fd, 0, SEEK_SET);
	}
	c->read = pcm_read;
	c->write = pcm_write;
	c->seek = pcm_seek;
	c->delay = pcm_delay;
	c->drop = pcm_drop;
	c->pause = pcm_pause;
	c->destroy = pcm_destroy;
	c->data = state;

	return c;

	fail:
	return NULL;
}

void pcm_codec_print_encodings(const char *type)
{
	int i;
	for (i = 0; i < LENGTH(encodings); ++i)
		fprintf(stdout, " %s", encodings[i].name);
}
