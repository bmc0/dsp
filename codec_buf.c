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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include "util.h"
#include "codec_buf.h"

#define CMD_QUEUE_LEN 8

struct read_cmd {
	enum codec_read_buf_cmd cc;
	ssize_t arg;
};

struct read_block {
	sample_t *data;
	struct codec *codec;
	int offset, frames;
};

struct read_state {
	pthread_t thread;
	struct {
		pthread_mutex_t lock;
		sem_t pending, sync;
		int error;
		struct {
			struct read_cmd c[CMD_QUEUE_LEN];
			int front, back, items;
			sem_t slots;
			ssize_t retval;
		} cmd;
		struct {
			struct read_block *b;
			char suspended, paused, rt_wait;
			int front, back, len, slots;
			int max_block_frames;
			sem_t items;
		} block;
	} queue;
};

struct write_block {
	sample_t *data;
	int frames;
};

struct write_state {
	pthread_t thread;
	struct {
		pthread_mutex_t lock;
		sem_t pending, sync;
		int error;
		struct {
			int c[CMD_QUEUE_LEN];
			int front, back, items;
			sem_t slots;
		} cmd;
		struct {
			struct write_block *b;
			char stopped, suspended;
			int front, back, len, items;
			int max_block_frames, channels;
			ssize_t fill_frames;
			sem_t slots;
		} block;
	} queue;
};

ssize_t codec_read_buf_cmd_push(void *state_data, enum codec_read_buf_cmd cmd, ssize_t arg)
{
	struct read_state *state = (struct read_state *) state_data;
	while (sem_wait(&state->queue.cmd.slots) != 0);
	pthread_mutex_lock(&state->queue.lock);
	state->queue.cmd.c[state->queue.cmd.back].cc = cmd;
	state->queue.cmd.c[state->queue.cmd.back].arg = arg;
	state->queue.cmd.back = (state->queue.cmd.back+1) % CMD_QUEUE_LEN;
	++state->queue.cmd.items;
	pthread_mutex_unlock(&state->queue.lock);
	sem_post(&state->queue.pending);
	if (cmd == CODEC_READ_BUF_CMD_SYNC
			|| cmd == CODEC_READ_BUF_CMD_SEEK
			|| cmd == CODEC_READ_BUF_CMD_SKIP) {
		while (sem_wait(&state->queue.sync) != 0);
		if (cmd == CODEC_READ_BUF_CMD_SEEK)
			return state->queue.cmd.retval;
	}
	return 0;
}

static void read_queue_suspend(struct read_state *state)
{
	if (!state->queue.block.suspended) {
		for (int i = 0; i < state->queue.block.slots; ++i)
			while (sem_trywait(&state->queue.pending) < 0 && errno == EINTR);
		state->queue.block.suspended = 1;
	}
}

static void read_queue_restore(struct read_state *state)
{
	if (state->queue.block.suspended && !state->queue.block.rt_wait) {
		for (int i = 0; i < state->queue.block.slots; ++i)
			sem_post(&state->queue.pending);
		state->queue.block.suspended = 0;
	}
}

ssize_t codec_read_buf_pull(void *state_data, sample_t *data, ssize_t frames, const struct codec *codec, int *r_next)
{
	ssize_t r = 0;
	struct read_state *state = (struct read_state *) state_data;
	while (r < frames) {
		while (sem_wait(&state->queue.block.items) != 0);
		pthread_mutex_lock(&state->queue.lock);
		struct read_block *block = &state->queue.block.b[state->queue.block.front];
		if ((r > 0 && block->frames == 0 && state->queue.block.rt_wait) || block->codec != codec) {
			if (block->codec != codec) *r_next = 1;
			sem_post(&state->queue.block.items);  /* did not read block */
			pthread_mutex_unlock(&state->queue.lock);
			return r;
		}
		if (block->frames > 0) {
			const int read_frames = MINIMUM(block->frames, frames - r);
			const int read_samples = read_frames * block->codec->channels;
			const int block_offset = block->offset * block->codec->channels;
			memcpy(data, block->data + block_offset, read_samples * sizeof(sample_t));
			data += read_samples;
			block->frames -= read_frames;
			block->offset += read_frames;
			r += read_frames;
		}
		if (block->frames == 0) {
			state->queue.block.front = (state->queue.block.front+1 < state->queue.block.len) ? state->queue.block.front+1 : 0;
			++state->queue.block.slots;
			if (!state->queue.block.suspended)
				sem_post(&state->queue.pending);
			/* restart block queue if waiting */
			if (state->queue.block.rt_wait && state->queue.block.slots == state->queue.block.len) {
				state->queue.block.rt_wait = 0;
				read_queue_restore(state);
			}
		}
		else sem_post(&state->queue.block.items);  /* partial read */
		pthread_mutex_unlock(&state->queue.lock);
	}
	return r;
}

static void read_queue_drop(struct read_state *state, const struct codec *codec, int from_back)
{
	while (state->queue.block.slots < state->queue.block.len) {
		const int idx = (from_back)
			? (state->queue.block.back > 0) ? state->queue.block.back-1 : state->queue.block.len-1
			: state->queue.block.front;
		struct read_block *block = &state->queue.block.b[idx];
		if (block->codec != codec)
			break;
		if (!state->queue.block.suspended)
			sem_post(&state->queue.pending);
		while (sem_trywait(&state->queue.block.items) < 0 && errno == EINTR);
		++state->queue.block.slots;
		if (from_back) state->queue.block.back = idx;
		else state->queue.block.front = (idx+1 < state->queue.block.len) ? idx+1 : 0;
	}
}

static struct codec * read_queue_seek(struct read_state *state, struct codec *codec, ssize_t *pos)
{
	struct codec *prev_codec = codec;
	if (state->queue.block.slots == state->queue.block.len) {  /* block queue is empty */
		if (codec) *pos = codec->seek(codec, *pos);
		return codec;
	}
	struct codec *sc = state->queue.block.b[state->queue.block.front].codec;
	if (sc == NULL) goto fail;
	for (;;) {
		const int idx = (state->queue.block.back > 0) ? state->queue.block.back-1 : state->queue.block.len-1;
		struct read_block *block = &state->queue.block.b[idx];
		if (block->codec != sc) {
			if (block->codec == NULL || block->codec->seek(block->codec, 0) == 0)
				read_queue_drop(state, block->codec, 1);
			else {
				codec = block->codec;
				goto fail;
			}
		}
		else if (block->codec == sc) {
			*pos = sc->seek(sc, *pos);
			if (*pos >= 0) read_queue_drop(state, sc, 0);
			codec = sc;
			goto done;
		}
	}
	fail:
	*pos = -1;
	done:
	if (*pos >= 0 && codec != prev_codec)
		state->queue.block.rt_wait = 0;
	if (!state->queue.block.paused)
		read_queue_restore(state);
	return codec;
}

static struct codec * read_queue_skip(struct read_state *state, struct codec *codec)
{
	read_queue_drop(state, state->queue.block.b[state->queue.block.front].codec, 0);
	if (state->queue.block.slots == state->queue.block.len) {  /* block queue is empty */
		if (codec && !state->queue.block.rt_wait) codec = codec->next;
		state->queue.block.rt_wait = 0;
	}
	if (!state->queue.block.paused) read_queue_restore(state);
	return codec;
}

static void * read_worker(void *arg)
{
	struct codec_read_buf *rb = (struct codec_read_buf *) arg;
	struct codec *codec = rb->cur_codec;
	struct read_state *state = (struct read_state *) rb->data;
	char done = 0;
	while (!done) {
		while (sem_wait(&state->queue.pending) != 0);
		pthread_mutex_lock(&state->queue.lock);
		if (state->queue.cmd.items > 0) {
			struct read_cmd cmd = state->queue.cmd.c[state->queue.cmd.front];
			state->queue.cmd.front = (state->queue.cmd.front+1) % CMD_QUEUE_LEN;
			--state->queue.cmd.items;
			switch (cmd.cc) {
			case CODEC_READ_BUF_CMD_SYNC:
				sem_post(&state->queue.sync);
				break;
			case CODEC_READ_BUF_CMD_SEEK:
				codec = read_queue_seek(state, codec, &cmd.arg);
				state->queue.cmd.retval = cmd.arg;
				sem_post(&state->queue.sync);
				break;
			case CODEC_READ_BUF_CMD_PAUSE:
				if (codec) codec->pause(codec, 1);
				read_queue_suspend(state);
				state->queue.block.paused = 1;
				break;
			case CODEC_READ_BUF_CMD_UNPAUSE:
				if (codec) codec->pause(codec, 0);
				read_queue_restore(state);
				state->queue.block.paused = 0;
				break;
			case CODEC_READ_BUF_CMD_SKIP:
				codec = read_queue_skip(state, codec);
				sem_post(&state->queue.sync);
				break;
			case CODEC_READ_BUF_CMD_TERM:
				done = 1;
				break;
			default:
				LOG_FMT(LL_ERROR, "read_worker: BUG: unrecognized command: %d", cmd);
			}
			pthread_mutex_unlock(&state->queue.lock);
			sem_post(&state->queue.cmd.slots);
		}
		else if (!state->queue.block.suspended && state->queue.block.slots > 0) {
			struct read_block *block = &state->queue.block.b[state->queue.block.back];
			--state->queue.block.slots;
			state->queue.block.back = (state->queue.block.back+1 < state->queue.block.len) ? state->queue.block.back+1 : 0;
			pthread_mutex_unlock(&state->queue.lock);

			const ssize_t r = (codec) ? codec->read(codec, block->data, state->queue.block.max_block_frames) : 0;
			block->offset = 0;
			block->frames = MAXIMUM(r, 0);
			block->codec = codec;

			/* note: a block with zero frames and a non-NULL codec field indicates the end of that codec */
			if (r <= 0 && codec) {
				codec = codec->next;
				/* if codec is real time, wait until the block queue empties */
				if (codec && (codec->hints & CODEC_HINT_REALTIME)) {
					/* LOG_FMT(LL_VERBOSE, "read_worker: info: suspending queue for \"%s\"...", codec->path); */
					pthread_mutex_lock(&state->queue.lock);
					read_queue_suspend(state);
					state->queue.block.rt_wait = 1;
					pthread_mutex_unlock(&state->queue.lock);
				}
			}
			sem_post(&state->queue.block.items);
		}
		else {
			LOG_S(LL_ERROR, "read_worker: BUG: woken up but nothing to do");
			pthread_mutex_unlock(&state->queue.lock);
		}
	}
	return NULL;
}

ssize_t codec_read_buf_delay_nw(struct codec_read_buf *rb)
{
	struct read_state *state = (struct read_state *) rb->data;
	struct codec *codec = rb->cur_codec;
	pthread_mutex_lock(&state->queue.lock);
	ssize_t fill_frames = 0;
	for (int i = state->queue.block.slots, k = state->queue.block.front; i < state->queue.block.len; ++i) {
		struct read_block *block = &state->queue.block.b[k];
		if (block->codec != codec) break;
		fill_frames += block->frames;
		k = (k+1 < state->queue.block.len) ? k+1 : 0;
	}
	ssize_t d = fill_frames + ((codec) ? codec->delay(codec) : 0);
	pthread_mutex_unlock(&state->queue.lock);
	return d;
}

static void read_state_destroy(struct read_state *state)
{
	pthread_mutex_destroy(&state->queue.lock);
	sem_destroy(&state->queue.pending);
	sem_destroy(&state->queue.sync);
	sem_destroy(&state->queue.cmd.slots);
	if (state->queue.block.b)
		free(state->queue.block.b[0].data);
	free(state->queue.block.b);
	sem_destroy(&state->queue.block.items);
	free(state);
}

void codec_read_buf_destroy_nw(struct codec_read_buf *rb)
{
	struct read_state *state = (struct read_state *) rb->data;
	codec_read_buf_cmd_push(state, CODEC_READ_BUF_CMD_TERM, 0);
	pthread_join(state->thread, NULL);
	read_state_destroy(state);
}

struct codec_read_buf * codec_read_buf_init(struct codec_list *codecs, int block_frames, int n_blocks, void (*error_cb)(int))
{
	int do_buf = 0, max_channels = 0;
	struct codec_read_buf *rb = calloc(1, sizeof(struct codec_read_buf));
	rb->codecs = codecs;
	rb->cur_codec = codecs->head;
	rb->error_cb = error_cb;

	if (n_blocks < CODEC_BUF_MIN_BLOCKS) return rb;
	for (struct codec *c = codecs->head; c; c = c->next) {
		max_channels = MAXIMUM(max_channels, c->channels);
		if (!(c->hints & CODEC_HINT_NO_BUF))
			do_buf = 1;
	}
	if (!do_buf) return rb;

	struct read_state *state = calloc(1, sizeof(struct read_state));
	pthread_mutex_init(&state->queue.lock, NULL);
	sem_init(&state->queue.pending, 0, n_blocks);
	sem_init(&state->queue.sync, 0, 0);
	sem_init(&state->queue.cmd.slots, 0, CMD_QUEUE_LEN);
	state->queue.block.len = n_blocks;
	state->queue.block.max_block_frames = MAXIMUM(block_frames, 8);
	state->queue.block.b = calloc(n_blocks, sizeof(struct read_block));
	const size_t block_samples = state->queue.block.max_block_frames * max_channels;
	state->queue.block.b[0].data = calloc(block_samples * n_blocks, sizeof(sample_t));
	for (int i = 1; i < n_blocks; ++i)
		state->queue.block.b[i].data = state->queue.block.b[0].data + (block_samples * i);
	sem_init(&state->queue.block.items, 0, 0);
	state->queue.block.slots = n_blocks;
	rb->data = state;

	if ((errno = pthread_create(&state->thread, NULL, read_worker, rb)) != 0) {
		LOG_FMT(LL_ERROR, "%s(): error: pthread_create() failed: %s", __func__, strerror(errno));
		read_state_destroy(state);
		free(rb);
		return NULL;
	}

	LOG_S(LL_VERBOSE, "info: read buffer enabled");
	return rb;
}

void codec_write_buf_cmd_push(void *state_data, enum codec_write_buf_cmd cmd)
{
	struct write_state *state = (struct write_state *) state_data;
	while (sem_wait(&state->queue.cmd.slots) != 0);
	pthread_mutex_lock(&state->queue.lock);
	state->queue.cmd.c[state->queue.cmd.back] = cmd;
	state->queue.cmd.back = (state->queue.cmd.back+1) % CMD_QUEUE_LEN;
	++state->queue.cmd.items;
	pthread_mutex_unlock(&state->queue.lock);
	sem_post(&state->queue.pending);
	if (cmd == CODEC_WRITE_BUF_CMD_DRAIN || cmd == CODEC_WRITE_BUF_CMD_SYNC)
		while (sem_wait(&state->queue.sync) != 0);
}

void codec_write_buf_push(void *state_data, sample_t *data, ssize_t frames)
{
	struct write_state *state = (struct write_state *) state_data;
	while (frames > 0) {
		const int block_frames = MINIMUM(state->queue.block.max_block_frames, frames);
		const int block_samples = block_frames * state->queue.block.channels;
		while (sem_wait(&state->queue.block.slots) != 0);
		pthread_mutex_lock(&state->queue.lock);
		if (!state->queue.error) {
			struct write_block *block = &state->queue.block.b[state->queue.block.back];
			block->frames = block_frames;
			memcpy(block->data, data, block_samples * sizeof(sample_t));
			state->queue.block.back = (state->queue.block.back+1 < state->queue.block.len) ? state->queue.block.back+1 : 0;
			state->queue.block.fill_frames += block_frames;
			++state->queue.block.items;
			state->queue.block.stopped = 0;
			if (!state->queue.block.suspended)
				sem_post(&state->queue.pending);
			pthread_mutex_unlock(&state->queue.lock);
		}
		else {
			/* LOG_FMT(LL_ERROR, "%s(): warning: discarded block", __func__); */
			pthread_mutex_unlock(&state->queue.lock);
			sem_post(&state->queue.block.slots);  /* did not write block */
		}
		data += block_samples;
		frames -= block_frames;
	}
}

static void write_queue_drop(struct write_state *state)
{
	while (state->queue.block.items > 0) {
		if (!state->queue.block.suspended)
			while (sem_trywait(&state->queue.pending) < 0 && errno == EINTR);
		state->queue.block.back = (state->queue.block.back > 0) ? state->queue.block.back-1 : state->queue.block.len-1;
		struct write_block *block = &state->queue.block.b[state->queue.block.back];
		state->queue.block.fill_frames -= block->frames;
		--state->queue.block.items;
		sem_post(&state->queue.block.slots);
	}
	state->queue.block.stopped = 1;
}

static void write_queue_suspend(struct write_state *state)
{
	if (!state->queue.block.suspended) {
		for (int i = 0; i < state->queue.block.items; ++i)
			while (sem_trywait(&state->queue.pending) < 0 && errno == EINTR);
		state->queue.block.suspended = 1;
	}
}

static void write_queue_restore(struct write_state *state)
{
	if (state->queue.block.suspended) {
		for (int i = 0; i < state->queue.block.items; ++i)
			sem_post(&state->queue.pending);
		state->queue.block.suspended = 0;
	}
}

static void * write_worker(void *arg)
{
	struct codec_write_buf *wb = (struct codec_write_buf *) arg;
	struct codec *codec = wb->codec;
	struct write_state *state = (struct write_state *) wb->data;
	char done = 0, drain = 0;
	while (!(done && state->queue.block.stopped)) {
		while (sem_wait(&state->queue.pending) != 0);
		pthread_mutex_lock(&state->queue.lock);
		if (state->queue.cmd.items > 0) {
			enum codec_write_buf_cmd cmd = state->queue.cmd.c[state->queue.cmd.front];
			state->queue.cmd.front = (state->queue.cmd.front+1) % CMD_QUEUE_LEN;
			--state->queue.cmd.items;
			switch (cmd) {
			case CODEC_WRITE_BUF_CMD_DROP_ALL:
				if (!state->queue.error) codec->drop(codec);
			case CODEC_WRITE_BUF_CMD_DROP_BLOCK_QUEUE:
				write_queue_drop(state);
				break;
			case CODEC_WRITE_BUF_CMD_PAUSE:
				if (!state->queue.error) codec->pause(codec, 1);
				write_queue_suspend(state);
				break;
			case CODEC_WRITE_BUF_CMD_UNPAUSE:
				if (!state->queue.error) codec->pause(codec, 0);
				write_queue_restore(state);
				break;
			case CODEC_WRITE_BUF_CMD_DRAIN:
				if (state->queue.block.suspended)
					write_queue_drop(state);
				if (state->queue.block.stopped)
					sem_post(&state->queue.sync);
				else drain = 1;
				break;
			case CODEC_WRITE_BUF_CMD_SYNC:
				sem_post(&state->queue.sync);
				break;
			case CODEC_WRITE_BUF_CMD_TERM:
				done = 1;
				break;
			default:
				LOG_FMT(LL_ERROR, "write_worker: BUG: unrecognized command: %d", cmd);
			}
			pthread_mutex_unlock(&state->queue.lock);
			sem_post(&state->queue.cmd.slots);
		}
		else if (!state->queue.block.suspended && state->queue.block.items > 0) {
			struct write_block *block = &state->queue.block.b[state->queue.block.front];
			state->queue.block.front = (state->queue.block.front+1 < state->queue.block.len) ? state->queue.block.front+1 : 0;
			state->queue.block.fill_frames -= block->frames;
			--state->queue.block.items;
			const char stopped = state->queue.block.stopped = (state->queue.block.items == 0);
			pthread_mutex_unlock(&state->queue.lock);

			if (!state->queue.error && block->frames > 0) {
				if (codec->write(codec, block->data, block->frames) != block->frames) {
					pthread_mutex_lock(&state->queue.lock);
					state->queue.error = 1;
					write_queue_drop(state);
					pthread_mutex_unlock(&state->queue.lock);
					if (wb->error_cb)
						wb->error_cb(CODEC_BUF_ERROR_SHORT_WRITE);
				}
			}
			sem_post(&state->queue.block.slots);
			if (drain && stopped) {
				drain = 0;
				sem_post(&state->queue.sync);
			}
		}
		else {
			LOG_S(LL_ERROR, "write_worker: BUG: items supposedly pending but nothing found");
			pthread_mutex_unlock(&state->queue.lock);
		}
	}
	return NULL;
}

ssize_t codec_write_buf_delay_nw(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	pthread_mutex_lock(&state->queue.lock);
	ssize_t d = state->queue.block.fill_frames + wb->codec->delay(wb->codec);
	pthread_mutex_unlock(&state->queue.lock);
	return d;
}

static void write_state_destroy(struct write_state *state)
{
	pthread_mutex_destroy(&state->queue.lock);
	sem_destroy(&state->queue.pending);
	sem_destroy(&state->queue.sync);
	sem_destroy(&state->queue.cmd.slots);
	if (state->queue.block.b)
		free(state->queue.block.b[0].data);
	free(state->queue.block.b);
	sem_destroy(&state->queue.block.slots);
	free(state);
}

void codec_write_buf_destroy_nw(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	codec_write_buf_cmd_push(state, CODEC_WRITE_BUF_CMD_DRAIN);
	codec_write_buf_cmd_push(state, CODEC_WRITE_BUF_CMD_TERM);
	pthread_join(state->thread, NULL);
	write_state_destroy(state);
}

struct codec_write_buf * codec_write_buf_init(struct codec *codec, int block_frames, int n_blocks, void (*error_cb)(int))
{
	struct codec_write_buf *wb = calloc(1, sizeof(struct codec_write_buf));
	wb->codec = codec;
	wb->error_cb = error_cb;

	if (n_blocks < CODEC_BUF_MIN_BLOCKS || (codec->hints & CODEC_HINT_NO_BUF))
		return wb;

	struct write_state *state = calloc(1, sizeof(struct write_state));
	pthread_mutex_init(&state->queue.lock, NULL);
	sem_init(&state->queue.pending, 0, 0);
	sem_init(&state->queue.sync, 0, 0);
	sem_init(&state->queue.cmd.slots, 0, CMD_QUEUE_LEN);
	state->queue.block.stopped = 1;
	state->queue.block.len = n_blocks;
	state->queue.block.channels = codec->channels;
	state->queue.block.max_block_frames = MAXIMUM(block_frames, 8);
	state->queue.block.b = calloc(n_blocks, sizeof(struct write_block));
	const size_t block_samples = state->queue.block.max_block_frames * codec->channels;
	state->queue.block.b[0].data = calloc(block_samples * n_blocks, sizeof(sample_t));
	for (int i = 1; i < n_blocks; ++i)
		state->queue.block.b[i].data = state->queue.block.b[0].data + (block_samples * i);
	sem_init(&state->queue.block.slots, 0, n_blocks);
	wb->data = state;

	if ((errno = pthread_create(&state->thread, NULL, write_worker, wb)) != 0) {
		LOG_FMT(LL_ERROR, "%s(): error: pthread_create() failed: %s", __func__, strerror(errno));
		write_state_destroy(state);
		free(wb);
		return NULL;
	}

	LOG_S(LL_VERBOSE, "info: write buffer enabled");
	return wb;
}
