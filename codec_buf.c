/*
 * This file is part of dsp.
 *
 * Copyright (c) 2024 Michael Barbour <barbour.michael.0@gmail.com>
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

enum write_cmd {
	WRITE_CMD_DROP_BLOCK_QUEUE,  /* drop blocks in queue (async) */
	WRITE_CMD_DROP_ALL,          /* as above, but drop output codec buffer as well (async) */
	WRITE_CMD_PAUSE,             /* pause output codec (async) */
	WRITE_CMD_UNPAUSE,           /* unpause output codec (async) */
	WRITE_CMD_DRAIN,             /* if running, write pending blocks and stop (sync) */
	WRITE_CMD_SYNC,              /* wait for previous command to be read (sync, obviously) */
	WRITE_CMD_TERM,              /* terminate after stop (async) */
};

struct buf_block {
	int frames;
	sample_t *data;
};

struct write_state {
	pthread_t thread;
	struct codec *codec;
	void (*error_cb)(int);
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
			struct buf_block *b;
			char stopped, suspended;
			int front, back, len, items;
			int max_block_frames, channels;
			ssize_t fill_frames;
			sem_t slots;
		} block;
	} queue;
};

static void write_cmd_push(struct write_state *state, enum write_cmd cmd)
{
	while (sem_wait(&state->queue.cmd.slots) != 0);
	pthread_mutex_lock(&state->queue.lock);
	state->queue.cmd.c[state->queue.cmd.back] = cmd;
	state->queue.cmd.back = (state->queue.cmd.back+1) % CMD_QUEUE_LEN;
	++state->queue.cmd.items;
	pthread_mutex_unlock(&state->queue.lock);
	sem_post(&state->queue.pending);
	if (cmd == WRITE_CMD_DRAIN || cmd == WRITE_CMD_SYNC)
		while (sem_wait(&state->queue.sync) != 0);
}

static void write_queue_push(struct write_state *state, sample_t *data, ssize_t frames)
{
	while (frames > 0) {
		const int block_frames = MINIMUM(state->queue.block.max_block_frames, frames);
		const int block_samples = block_frames * state->queue.block.channels;
		while (sem_wait(&state->queue.block.slots) != 0);
		pthread_mutex_lock(&state->queue.lock);
		if (!state->queue.error) {
			struct buf_block *block = &state->queue.block.b[state->queue.block.back];
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
		struct buf_block *block = &state->queue.block.b[state->queue.block.back];
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
	struct write_state *state = (struct write_state *) arg;
	char done = 0, drain = 0;
	while (!(done && state->queue.block.stopped)) {
		while (sem_wait(&state->queue.pending) != 0);
		pthread_mutex_lock(&state->queue.lock);
		if (state->queue.cmd.items > 0) {
			enum write_cmd cmd = state->queue.cmd.c[state->queue.cmd.front];
			state->queue.cmd.front = (state->queue.cmd.front+1) % CMD_QUEUE_LEN;
			--state->queue.cmd.items;
			switch (cmd) {
			case WRITE_CMD_DROP_ALL:
				if (!state->queue.error) state->codec->drop(state->codec);
			case WRITE_CMD_DROP_BLOCK_QUEUE:
				write_queue_drop(state);
				break;
			case WRITE_CMD_PAUSE:
				if (!state->queue.error) state->codec->pause(state->codec, 1);
				write_queue_suspend(state);
				break;
			case WRITE_CMD_UNPAUSE:
				if (!state->queue.error) state->codec->pause(state->codec, 0);
				write_queue_restore(state);
				break;
			case WRITE_CMD_DRAIN:
				if (state->queue.block.suspended)
					write_queue_drop(state);
				if (state->queue.block.stopped)
					sem_post(&state->queue.sync);
				else drain = 1;
				break;
			case WRITE_CMD_SYNC:
				sem_post(&state->queue.sync);
				break;
			case WRITE_CMD_TERM:
				done = 1;
				break;
			default:
				LOG_FMT(LL_ERROR, "write_worker: BUG: unrecognized command: %d", cmd);
			}
			pthread_mutex_unlock(&state->queue.lock);
			sem_post(&state->queue.cmd.slots);
		}
		else if (!state->queue.block.suspended && state->queue.block.items > 0) {
			struct buf_block *block = &state->queue.block.b[state->queue.block.front];
			state->queue.block.front = (state->queue.block.front+1 < state->queue.block.len) ? state->queue.block.front+1 : 0;
			state->queue.block.fill_frames -= block->frames;
			--state->queue.block.items;
			const char stopped = state->queue.block.stopped = (state->queue.block.items == 0);
			pthread_mutex_unlock(&state->queue.lock);

			if (!state->queue.error && block->frames > 0) {
				if (state->codec->write(state->codec, block->data, block->frames) != block->frames) {
					pthread_mutex_lock(&state->queue.lock);
					state->queue.error = 1;
					write_queue_drop(state);
					pthread_mutex_unlock(&state->queue.lock);
					if (state->error_cb)
						state->error_cb(CODEC_BUF_ERROR_SHORT_WRITE);
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

void codec_write_buf_write(struct codec_write_buf *wb, sample_t *buf, ssize_t frames)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_queue_push(state, buf, frames);
}

ssize_t codec_write_buf_delay(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	pthread_mutex_lock(&state->queue.lock);
	ssize_t d = state->queue.block.fill_frames + state->codec->delay(state->codec);
	pthread_mutex_unlock(&state->queue.lock);
	return d;
}

void codec_write_buf_drop(struct codec_write_buf *wb, int drop_all)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_cmd_push(state, (drop_all) ? WRITE_CMD_DROP_ALL : WRITE_CMD_DROP_BLOCK_QUEUE);
}

void codec_write_buf_pause(struct codec_write_buf *wb, int pause)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_cmd_push(state, (pause) ? WRITE_CMD_PAUSE : WRITE_CMD_UNPAUSE);
}

void codec_write_buf_drain(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_cmd_push(state, WRITE_CMD_DRAIN);
}

void codec_write_buf_sync(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_cmd_push(state, WRITE_CMD_SYNC);
}

void write_state_destroy(struct write_state *state)
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

void codec_write_buf_destroy(struct codec_write_buf *wb)
{
	struct write_state *state = (struct write_state *) wb->data;
	write_cmd_push(state, WRITE_CMD_DRAIN);
	write_cmd_push(state, WRITE_CMD_TERM);
	pthread_join(state->thread, NULL);
	write_state_destroy(state);
	free(wb);
}

struct codec_write_buf * codec_write_buf_init(struct codec *c, int block_frames, int n_blocks, void (*error_cb)(int))
{
	struct write_state *state = calloc(1, sizeof(struct write_state));
	state->codec = c;

	pthread_mutex_init(&state->queue.lock, NULL);
	sem_init(&state->queue.pending, 0, 0);
	sem_init(&state->queue.sync, 0, 0);
	state->error_cb = error_cb;
	sem_init(&state->queue.cmd.slots, 0, CMD_QUEUE_LEN);
	state->queue.block.stopped = 1;
	state->queue.block.len = MAXIMUM(n_blocks, 2);
	state->queue.block.channels = c->channels;
	state->queue.block.max_block_frames = MAXIMUM(block_frames, 8);
	state->queue.block.b = calloc(n_blocks, sizeof(struct buf_block));
	state->queue.block.b[0].data = calloc(block_frames * n_blocks * c->channels, sizeof(sample_t));
	for (int i = 1; i < n_blocks; ++i)
		state->queue.block.b[i].data = state->queue.block.b[0].data + (block_frames * c->channels * i);
	sem_init(&state->queue.block.slots, 0, n_blocks);

	if ((errno = pthread_create(&state->thread, NULL, write_worker, state)) != 0) {
		LOG_FMT(LL_ERROR, "%s(): error: pthread_create() failed: %s", __func__, strerror(errno));
		write_state_destroy(state);
		return NULL;
	}

	struct codec_write_buf *wb = calloc(1, sizeof(struct codec_write_buf));
	wb->write = codec_write_buf_write;
	wb->delay = codec_write_buf_delay;
	wb->drop = codec_write_buf_drop;
	wb->pause = codec_write_buf_pause;
	wb->drain = codec_write_buf_drain;
	wb->sync = codec_write_buf_sync;
	wb->destroy = codec_write_buf_destroy;
	wb->data = state;

	return wb;
}
