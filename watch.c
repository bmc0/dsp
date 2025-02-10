/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "util.h"

#define POLL_INTERVAL 1000  /* milliseconds */

struct watch_list {
	struct watch_node *head, *tail;
};

struct watch_node {
	struct watch_node *next;
	struct timespec last_mtime;
	pthread_mutex_t lock;
	char *path, *channel_mask;
	struct effects_chain chain, new_chain;
	struct effect *e;
	struct effects_chain_xfade_state xfade;
	ssize_t in_frames, buf_len;
	int update_chain, enforce_eof_marker;
};

static struct {
	pthread_t thread;
	pthread_mutex_t init_lock, lock;  /* note: .lock must be recursive */
	struct watch_list list;
	int init_count;
} watch_state = { .init_lock = PTHREAD_MUTEX_INITIALIZER };

static void watch_list_append(struct watch_list *list, struct watch_node *node)
{
	if (list->tail == NULL) list->head = node;
	else list->tail->next = node;
	list->tail = node;
	node->next = NULL;
}

static void watch_list_remove(struct watch_list *list, struct watch_node *node)
{
	if (node == list->head) {
		if (node == list->tail)
			list->head = list->tail = NULL;
		else list->head = node->next;
	}
	else {
		struct watch_node *prev = list->head;
		while (prev->next != node) prev = prev->next;
		if (node == list->tail) list->tail = prev;
		prev->next = node->next;
	}
}

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
			const ssize_t buf_len = get_effects_chain_buffer_len(&new_chain, node->in_frames, node->e->istream.channels);
			if (buf_len > node->buf_len) {
				pthread_mutex_unlock(&node->lock);
				LOG_FMT(LL_ERROR, "%s: error: buffer length: %s", node->e->name, node->path);
				destroy_effects_chain(&new_chain);
			}
			else {
				destroy_effects_chain(&node->new_chain);
				effects_chain_set_dither_params(&new_chain, 0, 0);  /* disable auto dither */
				node->new_chain = new_chain;
				node->update_chain = 1;
				pthread_mutex_unlock(&node->lock);
			}
		}
	}
	else destroy_effects_chain(&new_chain);
}

static void * watch_worker(void *arg)
{
	struct stat sb;
	for (;;) {
		struct timespec t = (struct timespec) {
			.tv_sec = (POLL_INTERVAL)/1000,
			.tv_nsec = ((POLL_INTERVAL)%1000)*1000000
		};
		nanosleep(&t, NULL);
		int old_cs;
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cs);
		pthread_mutex_lock(&watch_state.lock);
		for (struct watch_node *node = watch_state.list.head; node; node = node->next) {
			if (stat(node->path, &sb) < 0)
				LOG_FMT(LL_VERBOSE, "%s: warning: stat() failed: %s: %s", node->e->name, node->path, strerror(errno));
			else if (sb.st_mtim.tv_sec != node->last_mtime.tv_sec || sb.st_mtim.tv_nsec != node->last_mtime.tv_nsec) {
				node->last_mtime = sb.st_mtim;
				watch_reload(node);
			}
		}
		pthread_mutex_unlock(&watch_state.lock);
		pthread_setcancelstate(old_cs, &old_cs);
	}
	return NULL;
}

static void watch_finish_xfade(struct watch_node *node)
{
	destroy_effects_chain(&node->chain);
	node->chain = node->xfade.chain[1];
	effects_chain_xfade_reset(&node->xfade);
}

sample_t * watch_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct watch_node *node = (struct watch_node *) e->data;
	pthread_mutex_lock(&node->lock);
	if (node->update_chain && node->xfade.pos == 0) {
		node->xfade.chain[0] = node->chain;
		node->xfade.chain[1] = node->new_chain;
		node->xfade.pos = node->xfade.frames;
		if (node->buf_len == 0 || node->xfade.pos == 0)
			watch_finish_xfade(node);  /* no crossfade */
		node->new_chain = EFFECTS_CHAIN_INITIALIZER;
		node->update_chain = 0;
	}
	pthread_mutex_unlock(&node->lock);
	if (node->xfade.pos > 0) {
		sample_t *rbuf = effects_chain_xfade_run(&node->xfade, frames, ibuf, obuf);
		if (node->xfade.pos == 0) {
			watch_finish_xfade(node);
			LOG_FMT(LL_VERBOSE, "%s: info: end of crossfade", e->name);
		}
		return rbuf;
	}
	return run_effects_chain(node->chain.head, frames, ibuf, obuf);
}

ssize_t watch_effect_delay(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;
	return rint(get_effects_chain_delay(&node->chain) * e->ostream.fs);
}

void watch_effect_reset(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;
	if (node->xfade.pos > 0) watch_finish_xfade(node);
	reset_effects_chain(&node->chain);
}

void watch_effect_signal(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;
	signal_effects_chain(&node->chain);
}

sample_t * watch_effect_drain2(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct watch_node *node = (struct watch_node *) e->data;
	if (node->xfade.pos > 0) watch_finish_xfade(node);
	return drain_effects_chain(&node->chain, frames, buf1, buf2);
}

static void watch_node_destroy(struct watch_node *node)
{
	pthread_mutex_destroy(&node->lock);
	destroy_effects_chain(&node->chain);
	destroy_effects_chain(&node->xfade.chain[1]);
	destroy_effects_chain(&node->new_chain);
	free(node->xfade.buf);
	free(node->path);
	free(node->channel_mask);
	free(node);
}

void watch_effect_destroy(struct effect *e)
{
	struct watch_node *node = (struct watch_node *) e->data;

	pthread_mutex_lock(&watch_state.lock);
	watch_list_remove(&watch_state.list, node);
	pthread_mutex_unlock(&watch_state.lock);

	watch_node_destroy(node);
	pthread_mutex_lock(&watch_state.init_lock);
	if (--watch_state.init_count == 0) {
		pthread_cancel(watch_state.thread);
		pthread_join(watch_state.thread, NULL);
		pthread_mutex_destroy(&watch_state.lock);
		/* LOG_FMT(LL_VERBOSE, "%s: info: worker thread exited", e->name); */
	}
	pthread_mutex_unlock(&watch_state.init_lock);
}

ssize_t watch_effect_buffer_frames(struct effect *e, ssize_t in_frames)
{
	struct watch_node *node = (struct watch_node *) e->data;
	pthread_mutex_lock(&node->lock);
	const ssize_t buf_len = get_effects_chain_buffer_len(&node->chain, in_frames, e->istream.channels);
	const ssize_t buf_frames = ratio_mult_ceil(buf_len, 1, e->ostream.channels);
	if (buf_len > node->buf_len) {
		node->in_frames = in_frames;
		node->buf_len = buf_len;
		free(node->xfade.buf);
		node->xfade.buf = calloc(node->buf_len, sizeof(sample_t));
	}
	pthread_mutex_unlock(&node->lock);
	return buf_frames;
}

struct effect * watch_effect_init(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const char *dir, int argc, const char *const *argv)
{
	pthread_mutexattr_t m_attr;
	struct effects_chain chain = EFFECTS_CHAIN_INITIALIZER;
	struct watch_node *node;
	struct effect *e;
	struct stat sb;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	int enforce_eof_marker = 0, opt;

	while ((opt = dsp_getopt(&g, argc-1, argv, "e")) != -1) {
		switch (opt) {
		case 'e': enforce_eof_marker = 1; break;
		default: goto print_usage;
		}
	}
	if (g.ind != argc-1) {
		print_usage:
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}

	struct stream_info stream = *istream;
	char *path = construct_full_path(dir, argv[g.ind]);
	if (build_effects_chain_from_file(path, &chain, &stream, channel_selector, NULL, enforce_eof_marker))
		goto open_fail;
	if (stat(path, &sb) < 0) {
		LOG_FMT(LL_ERROR, "%s: error: stat() failed: %s: %s", argv[0], path, strerror(errno));
		goto open_fail;
	}

	node = calloc(1, sizeof(struct watch_node));
	node->last_mtime = sb.st_mtim;
	pthread_mutex_init(&node->lock, NULL);
	node->path = path;
	node->channel_mask = NEW_SELECTOR(istream->channels);
	COPY_SELECTOR(node->channel_mask, channel_selector, istream->channels);
	node->chain = chain;
	node->enforce_eof_marker = enforce_eof_marker;
	node->xfade = EFFECTS_CHAIN_XFADE_STATE_INITIALIZER;
	node->xfade.istream = *istream;
	node->xfade.ostream = stream;
	node->xfade.frames = lround((EFFECTS_CHAIN_XFADE_TIME)/1000.0 * stream.fs);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream = *istream;
	e->ostream = stream;

	e->run = watch_effect_run;
	e->delay = watch_effect_delay;
	e->reset = watch_effect_reset;
	e->signal = watch_effect_signal;
	e->drain2 = watch_effect_drain2;
	e->destroy = watch_effect_destroy;
	e->buffer_frames = watch_effect_buffer_frames;

	e->data = node;
	node->e = e;

	pthread_mutex_lock(&watch_state.init_lock);
	if (watch_state.init_count == 0) {
		pthread_mutexattr_init(&m_attr);
		pthread_mutexattr_settype(&m_attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&watch_state.lock, &m_attr);
		pthread_mutexattr_destroy(&m_attr);
		/* LOG_FMT(LL_VERBOSE, "%s: info: starting worker thread", argv[0]); */
		if ((errno = pthread_create(&watch_state.thread, NULL, watch_worker, NULL)) != 0) {
			LOG_FMT(LL_ERROR, "%s: error: pthread_create() failed: %s", argv[0], strerror(errno));
			pthread_mutex_destroy(&watch_state.lock);
			watch_node_destroy(node);
			free(e);
			pthread_mutex_unlock(&watch_state.init_lock);
			return NULL;
		}
	}
	++watch_state.init_count;
	pthread_mutex_unlock(&watch_state.init_lock);
	pthread_mutex_lock(&watch_state.lock);
	watch_list_append(&watch_state.list, node);
	pthread_mutex_unlock(&watch_state.lock);

	return e;

	open_fail:
	destroy_effects_chain(&chain);
	free(path);
	return NULL;
}
