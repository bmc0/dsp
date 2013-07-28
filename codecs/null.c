#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "null.h"

ssize_t null_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	memset(buf, 0, frames * c->channels * sizeof(sample_t));
	return frames;
}

ssize_t null_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	return frames;
}

ssize_t null_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t null_delay(struct codec *c)
{
	return 0;
}

void null_reset(struct codec *c)
{
	/* do nothing */
}

void null_pause(struct codec *c, int p)
{
	/* do nothing */
}

void null_destroy(struct codec *c)
{
	/* nothing to clean up */
}

struct codec * null_codec_init(const char *type, int mode, const char *path, const char *enc, int endian, int rate, int channels)
{
	struct codec *c = calloc(1, sizeof(struct codec));
	c->type = type;
	c->enc = "sample_t";
	c->path = "null";
	c->fs = SELECT_FS(rate);
	c->prec = 53;
	c->channels = SELECT_CHANNELS(channels);
	c->read = null_read;
	c->write = null_write;
	c->seek = null_seek;
	c->delay = null_delay;
	c->reset = null_reset;
	c->pause = null_pause;
	c->destroy = null_destroy;
	c->data = NULL;
	
	return c;
}

void null_codec_print_encodings(const char *type)
{
	fprintf(stderr, " sample_t");
}
