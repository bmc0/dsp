/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2024 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <mad.h>
#include "mp3.h"

/* largest possible frame size (http://www.mars.org/pipermail/mad-dev/2002-January/000425.html) */
/* #define MP3_BUF_SIZE (2881 + MAD_BUFFER_GUARD) */
#define MP3_BUF_SIZE (1<<12)

struct mp3_state {
	int fd;
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	ssize_t pcm_pos;
	unsigned char *buf;
};

static const char codec_name[] = "mp3";

static ssize_t refill_buffer(struct mp3_state *state)
{
	ssize_t r, rem = state->stream.bufend - state->stream.next_frame;
	memmove(state->buf, state->stream.next_frame, rem);
	if ((r = read(state->fd, state->buf + rem, MP3_BUF_SIZE - rem)) == -1) {
		LOG_FMT(LL_ERROR, "%s: error: read failure: %s", codec_name, strerror(errno));
		return 0;
	}
	if (r == 0)
		return 0;
	mad_stream_buffer(&state->stream, state->buf, r + rem);
	state->stream.error = 0;
	return r;
}

ssize_t mp3_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	struct mp3_state *state = (struct mp3_state *) c->data;
	ssize_t buf_pos = 0, samples = frames * c->channels;
	while (buf_pos < samples) {
		if (state->pcm_pos >= state->synth.pcm.length) {
			state->pcm_pos = 0;
			while (mad_frame_decode(&state->frame, &state->stream)) {
				if (MAD_RECOVERABLE(state->stream.error))
					continue;
				if (state->stream.error == MAD_ERROR_BUFLEN) {
					if (refill_buffer(state) == 0)
						return 0;
					continue;
				}
				LOG_FMT(LL_ERROR, "%s: non-recoverable MAD error", codec_name);
				return 0;
			}
			mad_synth_frame(&state->synth, &state->frame);
		}

		buf[buf_pos++] = mad_f_todouble(state->synth.pcm.samples[0][state->pcm_pos]);
		if (c->channels == 2) buf[buf_pos++] = mad_f_todouble(state->synth.pcm.samples[1][state->pcm_pos]);
		++state->pcm_pos;
	}
	return buf_pos / c->channels;
}

ssize_t mp3_seek(struct codec *c, ssize_t pos)
{
	struct mp3_state *state = (struct mp3_state *) c->data;
	ssize_t fpos = 0;

	if (pos < 0)
		pos = 0;
	else if (pos >= c->frames)
		pos = c->frames - 1;

	if (lseek(state->fd, 0, SEEK_SET) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: lseek failed", codec_name);
		return -1;
	}

	mad_stream_finish(&state->stream);
	mad_frame_finish(&state->frame);
	mad_synth_finish(&state->synth);

	mad_stream_init(&state->stream);
	mad_frame_init(&state->frame);
	mad_synth_init(&state->synth);

	if (read(state->fd, state->buf, MP3_BUF_SIZE) == -1) {
		LOG_FMT(LL_ERROR, "%s: error: read failed: %s", codec_name, strerror(errno));
		return -1;
	}
	mad_stream_buffer(&state->stream, state->buf, MP3_BUF_SIZE);
	state->stream.error = 0;
	while (fpos < pos) {
		while (mad_header_decode(&state->frame.header, &state->stream)) {
			if (MAD_RECOVERABLE(state->stream.error))
				continue;
			if (state->stream.error == MAD_ERROR_BUFLEN) {
				if (refill_buffer(state) == 0)
					return pos;
				continue;
			}
			LOG_FMT(LL_ERROR, "%s: non-recoverable MAD error", codec_name);
			return pos;
		}
		fpos += mad_timer_count(state->frame.header.duration, state->frame.header.samplerate);
	}

	mad_frame_decode(&state->frame, &state->stream);
	mad_synth_frame(&state->synth, &state->frame);

	return fpos;
}

ssize_t mp3_delay(struct codec *c)
{
	return 0;
}

void mp3_drop(struct codec *c)
{
	/* do nothing */
}

void mp3_pause(struct codec *c, int p)
{
	/* do nothing */
}

void mp3_destroy(struct codec *c)
{
	struct mp3_state *state = (struct mp3_state *) c->data;
	close(state->fd);
	mad_stream_finish(&state->stream);
	mad_frame_finish(&state->frame);
	mad_synth_finish(&state->synth);
	free(state->buf);
	free(state);
}

static ssize_t mp3_get_nframes(struct mp3_state *state)
{
	ssize_t len = 0;

	mad_stream_init(&state->stream);
	mad_frame_init(&state->frame);
	mad_synth_init(&state->synth);

	if (read(state->fd, state->buf, MP3_BUF_SIZE) == -1) {
		LOG_FMT(LL_ERROR, "%s: error: read failed: %s", codec_name, strerror(errno));
		len = -1;
		goto done;
	}
	mad_stream_buffer(&state->stream, state->buf, MP3_BUF_SIZE);
	state->stream.error = 0;
	for (;;) {
		while (mad_header_decode(&state->frame.header, &state->stream)) {
			if (MAD_RECOVERABLE(state->stream.error))
				continue;
			if (state->stream.error == MAD_ERROR_BUFLEN) {
				if (refill_buffer(state) == 0)
					goto done;
				continue;
			}
			LOG_FMT(LL_ERROR, "%s: non-recoverable MAD error", codec_name);
			len = -1;
			goto done;
		}
		len += mad_timer_count(state->frame.header.duration, state->frame.header.samplerate);
	}

	done:
	lseek(state->fd, 0, SEEK_SET);
	mad_stream_finish(&state->stream);
	mad_frame_finish(&state->frame);
	mad_synth_finish(&state->synth);
	return len;
}

struct codec * mp3_codec_init(const struct codec_params *p)
{
	struct mp3_state *state = NULL;
	struct codec *c = NULL;
	ssize_t nframes;

	state = calloc(1, sizeof(struct mp3_state));
	if ((state->fd = open(p->path, O_RDONLY)) == -1) {
		LOG_FMT(LL_OPEN_ERROR, "%s: error: failed to open file: %s: %s", codec_name, p->path, strerror(errno));
		goto fail;
	}
	state->buf = calloc(MP3_BUF_SIZE, 1);

	if ((nframes = mp3_get_nframes(state)) < 0)
		goto fail;

	mad_stream_init(&state->stream);
	mad_frame_init(&state->frame);
	mad_synth_init(&state->synth);

	if (read(state->fd, state->buf, MP3_BUF_SIZE) == -1) {
		LOG_FMT(LL_ERROR, "%s: error: read failed: %s", codec_name, strerror(errno));
		goto fail;
	}
	mad_stream_buffer(&state->stream, state->buf, MP3_BUF_SIZE);
	state->stream.error = 0;
	while (mad_frame_decode(&state->frame, &state->stream)) {
		if (MAD_RECOVERABLE(state->stream.error))
			continue;
		if (state->stream.error == MAD_ERROR_BUFLEN) {
			if (refill_buffer(state) == 0)
				goto buf_fill_fail;
			continue;
		}
		buf_fill_fail:
		LOG_FMT(LL_ERROR, "%s: error: no valid frame found", codec_name);
		goto fail;
	}
	mad_synth_frame(&state->synth, &state->frame);

	c = calloc(1, sizeof(struct codec));
	c->path = p->path;
	c->type = p->type;
	c->enc = "mad_f";
	c->fs = state->frame.header.samplerate;
	c->channels = MAD_NCHANNELS(&state->frame.header);
	c->prec = 24;
	c->frames = nframes;
	c->read = mp3_read;
	c->seek = mp3_seek;
	c->delay = mp3_delay;
	c->drop = mp3_drop;
	c->pause = mp3_pause;
	c->destroy = mp3_destroy;
	c->data = state;

	return c;

	fail:
	if (state->fd != -1)
		close(state->fd);
	mad_stream_finish(&state->stream);
	mad_frame_finish(&state->frame);
	mad_synth_finish(&state->synth);
	free(state->buf);
	free(state);
	return NULL;
}

void mp3_codec_print_encodings(const char *type)
{
	fprintf(stdout, " mad_f");
}
