/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include "watch.h"
#include "effects_chain.h"
#include "util.h"
#include "list_util.h"

#define POLL_INTERVAL 1000  /* milliseconds */

struct watch_node {
	struct watch_node *prev, *next;
	struct timespec last_mtime;
	pthread_mutex_t lock;
	char *path, *channel_mask;
	struct effects_chain chain, new_chain, xfade_chain;
	sample_t *ec_buf;
	struct effect *e;
	struct effects_chain_xfade_state xfade;
	ssize_t in_frames, out_frames;
	int update_chain, enforce_eof_marker;
};

struct watch_list {
	struct watch_node *head, *tail;
};

static struct {
	pthread_t thread;
	pthread_mutex_t init_lock, lock;
	struct watch_list list;
	int init_count;
} watch_state = {
	.init_lock = PTHREAD_MUTEX_INITIALIZER,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static void watch_reload(struct watch_node *node)
{
	struct effects_chain new_chain = EFFECTS_CHAIN_INITIALIZER;
	struct stream_info stream = node->e->istream;
	LOG_FMT(LL_NORMAL, "%s: info: reloading %s", node->e->name, node->path);
	if (build_effects_chain_from_file(node->path, &new_chain, &stream, node->channel_mask, NULL, node->enforce_eof_marker) == 0) {
		if (stream.fs != node->e->ostream.fs) {
			LOG_FMT(LL_ERROR, "%s: error: sample rate mismatch: %s", node->e->name, node->path);
			destroy_effects_chain(&new_chain);
		}
		else if (stream.channels != node->e->ostream.channels) {
			LOG_FMT(LL_ERROR, "%s: error: channels mismatch: %s", node->e->name, node->path);
			destroy_effects_chain(&new_chain);
		}
		else {
			pthread_mutex_lock(&node->lock);
			const ssize_t out_frames = get_effects_chain_max_out_frames(&new_chain, node->in_frames);
			if (out_frames > node->out_frames) {
				LOG_FMT(LL_ERROR, "%s: error: buffer length: %s", node->e->name, node->path);
				destroy_effects_chain(&new_chain);
			}
			else if (effects_chain_realloc_buffers(&new_chain, node->in_frames)) {
				destroy_effects_chain(&new_chain);
			}
			else {
				destroy_effects_chain(&node->new_chain);
				effects_chain_set_dither_params(&new_chain, 0, 0);  /* disable auto dither */
				node->new_chain = new_chain;
				node->update_chain = 1;
			}
			pthread_mutex_unlock(&node->lock);
		}
	}
	else destroy_effects_chain(&new_chain);
}

static void * watch_worker(void *arg)
{
	const struct timespec poll_int = {
		.tv_sec = (POLL_INTERVAL)/1000,
		.tv_nsec = ((POLL_INTERVAL)%1000)*1000000
	};
	for (;;) {
		nanosleep(&poll_int, NULL);
		int old_cs;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cs);
		pthread_mutex_lock(&watch_state.lock);
		struct watch_list reload_list = watch_state.list;
		watch_state.list.tail = watch_state.list.head = NULL;
		pthread_mutex_unlock(&watch_state.lock);
		LIST_FOREACH(&reload_list, node) {
			struct stat sb;
			if (stat(node->path, &sb) < 0)
				LOG_FMT(LL_VERBOSE, "%s: warning: stat() failed: %s: %s", node->e->name, node->path, strerror(errno));
			else if (sb.st_mtim.tv_sec != node->last_mtime.tv_sec || sb.st_mtim.tv_nsec != node->last_mtime.tv_nsec) {
				node->last_mtime = sb.st_mtim;
				watch_reload(node);
			}
		}
		pthread_mutex_lock(&watch_state.lock);
		LIST_CONCAT(&watch_state.list, &reload_list);
		pthread_mutex_unlock(&watch_state.lock);
		pthread_setcancelstate(old_cs, &old_cs);
	}
	return NULL;
}

static void watch_finish_xfade(struct watch_node *node)
{
	destroy_effects_chain(&node->chain);
	node->chain = node->xfade_chain;
	node->ec_buf = effects_chain_get_input_buffer(&node->chain);
	effects_chain_xfade_reset(&node->xfade);
	node->xfade_chain = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
}

static sample_t * watch_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct watch_node *node = (struct watch_node *) e->data;
	pthread_mutex_lock(&node->lock);
	if (node->update_chain && node->xfade.pos == 0) {
		node->xfade_chain = node->new_chain;
		effects_chain_xfade_begin(&node->xfade, &node->chain, &node->xfade_chain, EFFECTS_CHAIN_XFADE_TIME);
		if (!node->ec_buf || node->xfade.pos == 0)
			watch_finish_xfade(node);  /* no crossfade */
		node->new_chain = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
		node->update_chain = 0;
	}
	pthread_mutex_unlock(&node->lock);
	if (!node->ec_buf) {
		memset(ibuf, 0, *frames * e->ostream.channels * sizeof(sample_t));
		return ibuf;
	}
	memcpy(node->ec_buf, ibuf, *frames * e->istream.channels * sizeof(sample_t));
	if (node->xfade.pos > 0) {
		node->ec_buf = effects_chain_xfade_run(&node->xfade, frames);
		if (node->xfade.pos == 0) {
			watch_finish_xfade(node);
			LOG_FMT(LL_VERBOSE, "%s: info: end of crossfade", e->name);
		}
	}
	else node->ec_buf = run_effects_chain(&node->chain, frames);
	memcpy(ibuf, node->ec_buf, *frames * e->ostream.channels * sizeof(sample_t));
	return ibuf;
}

static void watch_effect_reset(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;
	if (node->xfade.pos > 0) watch_finish_xfade(node);
	reset_effects_chain(&node->chain);
}

static void watch_effect_signal(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;
	signal_effects_chain(&node->chain);
}

static sample_t * watch_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct watch_node *node = (struct watch_node *) e->data;
	if (node->xfade.pos > 0) watch_finish_xfade(node);
	node->ec_buf = drain_effects_chain(&node->chain, frames);
	if (*frames > 0) memcpy(buf1, node->ec_buf, *frames * e->ostream.channels * sizeof(sample_t));
	return buf1;
}

static void watch_node_destroy(struct watch_node *node)
{
	pthread_mutex_destroy(&node->lock);
	destroy_effects_chain(&node->chain);
	destroy_effects_chain(&node->new_chain);
	destroy_effects_chain(&node->xfade_chain);
	free(node->path);
	free(node->channel_mask);
	free(node);
}

static void watch_effect_destroy(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;

	pthread_mutex_lock(&watch_state.lock);
	LIST_REMOVE(&watch_state.list, node);
	pthread_mutex_unlock(&watch_state.lock);

	watch_node_destroy(node);
	pthread_mutex_lock(&watch_state.init_lock);
	if (--watch_state.init_count == 0) {
		pthread_cancel(watch_state.thread);
		pthread_join(watch_state.thread, NULL);
		/* LOG_FMT(LL_VERBOSE, "%s: info: worker thread exited", e->name); */
	}
	pthread_mutex_unlock(&watch_state.init_lock);
}

static ssize_t watch_effect_buffer_frames(struct effect *e, ssize_t in_frames)
{
	struct watch_node *node = (struct watch_node *) e->data;
	pthread_mutex_lock(&node->lock);
	if (in_frames > node->in_frames) {
		node->in_frames = in_frames;
		node->out_frames = get_effects_chain_max_out_frames(&node->chain, node->in_frames);
		if (effects_chain_realloc_buffers(&node->chain, node->in_frames)) {
			destroy_effects_chain(&node->chain);
			node->ec_buf = NULL;
		}
		else node->ec_buf = effects_chain_get_input_buffer(&node->chain);
	}
	const ssize_t out_frames = node->out_frames;
	pthread_mutex_unlock(&node->lock);
	return out_frames;
}

static void watch_effect_channel_deps(struct effect *e, char **deps)
{
	struct watch_node *node = (struct watch_node *) e->data;
	for (int i = 0; i < e->ostream.channels; ++i) {
		if (i >= e->istream.channels || GET_BIT(node->channel_mask, i))
			COPY_SELECTOR(deps[i], node->channel_mask, e->istream.channels);
	}
}

struct effect * watch_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	struct effects_chain chain = EFFECTS_CHAIN_INITIALIZER;
	struct watch_node *node = NULL;
	struct effect *e = NULL;
	struct stat sb;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int enforce_eof_marker = 0, opt;

	while ((opt = dsp_getopt(&g, argc-1, argv, "e")) != -1) {
		switch (opt) {
		case 'e': enforce_eof_marker = 1; break;
		default:
			dsp_getopt_print_error(&g, opt, argv[0]);
			goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		print_effect_usage(ei);
		return NULL;
	}

	struct stream_info stream = *istream;
	char *path = construct_full_path(dir, argv[g.ind], istream->fs, num_bits_set(channel_selector, istream->channels));
	if (!path || build_effects_chain_from_file(path, &chain, &stream, channel_selector, NULL, enforce_eof_marker))
		goto open_fail;
	if (stat(path, &sb) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: stat() failed: %s: %s", argv[0], path, strerror(errno));
		goto open_fail;
	}

	node = calloc(1, sizeof(struct watch_node));
	if (check_alloc(ei->name, node)) goto fail;
	node->last_mtime = sb.st_mtim;
	pthread_mutex_init(&node->lock, NULL);
	node->path = path;
	node->channel_mask = NEW_SELECTOR(istream->channels);
	if (check_alloc(ei->name, node->channel_mask)) goto fail;
	COPY_SELECTOR(node->channel_mask, channel_selector, istream->channels);
	node->chain = chain;
	node->enforce_eof_marker = enforce_eof_marker;
	node->xfade = (struct effects_chain_xfade_state) EFFECTS_CHAIN_XFADE_STATE_INITIALIZER;

	e = calloc(1, sizeof(struct effect));
	if (check_alloc(ei->name, e)) goto fail;
	e->name = ei->name;
	e->istream = *istream;
	e->ostream = stream;

	e->run = watch_effect_run;
	e->reset = watch_effect_reset;
	e->signal = watch_effect_signal;
	e->drain2 = watch_effect_drain2;
	e->destroy = watch_effect_destroy;
	e->buffer_frames = watch_effect_buffer_frames;
	e->channel_deps = watch_effect_channel_deps;

	e->data = node;
	node->e = e;

	pthread_mutex_lock(&watch_state.init_lock);
	if (watch_state.init_count == 0) {
		/* LOG_FMT(LL_VERBOSE, "%s: info: starting worker thread", argv[0]); */
		if ((errno = pthread_create(&watch_state.thread, NULL, watch_worker, NULL)) != 0) {
			LOG_FMT(LL_ERROR, "%s: error: pthread_create() failed: %s", argv[0], strerror(errno));
			pthread_mutex_unlock(&watch_state.init_lock);
			goto fail;
		}
	}
	++watch_state.init_count;
	pthread_mutex_unlock(&watch_state.init_lock);
	pthread_mutex_lock(&watch_state.lock);
	LIST_APPEND(&watch_state.list, node);
	pthread_mutex_unlock(&watch_state.lock);
	return e;

	open_fail:
	destroy_effects_chain(&chain);
	free(path);
	return NULL;

	fail:
	if (node) watch_node_destroy(node);
	free(e);
	return NULL;
}
