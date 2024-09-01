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

#ifndef DSP_EFFECT_H
#define DSP_EFFECT_H

#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);
	int effect_number;
};

struct effect {
	struct effect *next;
	const char *name;
	struct stream_info istream, ostream;
	char *channel_selector;  /* for use *only* by the effect */
	/* All functions may be NULL */
	sample_t * (*run)(struct effect *, ssize_t *, sample_t *, sample_t *);  /* if NULL, the effect will not be used */
	ssize_t (*delay)(struct effect *);  /* returns the latency in frames at ostream.fs */
	void (*reset)(struct effect *);
	void (*signal)(struct effect *);
	void (*plot)(struct effect *, int);
	void (*drain)(struct effect *, ssize_t *, sample_t *);
	void (*destroy)(struct effect *);
	void *data;
};

struct effects_chain {
	struct effect *head;
	struct effect *tail;
};

const struct effect_info * get_effect_info(const char *);
void destroy_effect(struct effect *);
void append_effect(struct effects_chain *, struct effect *);
int build_effects_chain(int, const char *const *, struct effects_chain *, struct stream_info *, const char *, const char *);
int build_effects_chain_from_file(struct effects_chain *, struct stream_info *, const char *, const char *, const char *);
ssize_t get_effects_chain_buffer_len(struct effects_chain *, ssize_t, int);
sample_t * run_effects_chain(struct effect *, ssize_t *, sample_t *, sample_t *);
double get_effects_chain_delay(struct effects_chain *);
void reset_effects_chain(struct effects_chain *);
void signal_effects_chain(struct effects_chain *);
void plot_effects_chain(struct effects_chain *, int);
sample_t * drain_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void destroy_effects_chain(struct effects_chain *);
void print_all_effects(void);

#endif
