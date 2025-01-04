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

	if (n_blocks < CODEC_BUF_MIN_BLOCKS || (codec->hints & CODEC_HINT_NO_OUT_BUF))
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
