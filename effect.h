/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2025 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <string.h>
#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);
	int effect_number;
};

enum {
	EFFECT_FLAG_PLOT_MIX        = 1<<0,
	EFFECT_FLAG_OPT_REORDERABLE = 1<<1,
	EFFECT_FLAG_NO_DITHER       = 1<<2,  /* does not modify the signal such that dither is useful */
};

struct effect {
	struct effect *next;
	const char *name;
	struct stream_info istream, ostream;
	char *channel_selector;  /* for use *only* by the effect */
	int flags;
	/* All functions may be NULL */
	sample_t * (*run)(struct effect *, ssize_t *, sample_t *, sample_t *);  /* if NULL, the effect will not be used */
	ssize_t (*delay)(struct effect *);  /* returns the latency in frames at ostream.fs */
	void (*reset)(struct effect *);
	void (*signal)(struct effect *);
	void (*plot)(struct effect *, int);
	void (*drain)(struct effect *, ssize_t *, sample_t *);
	sample_t * (*drain2)(struct effect *, ssize_t *, sample_t *, sample_t *);
	void (*destroy)(struct effect *);
	struct effect * (*merge)(struct effect *, struct effect *);
	ssize_t (*buffer_frames)(struct effect *, ssize_t);
	void *data;
};

struct effects_chain {
	struct effect *head;
	struct effect *tail;
};

#define EFFECTS_CHAIN_INITIALIZER_BARE { NULL, NULL }  /* needed for GCC 12 and earlier */
#define EFFECTS_CHAIN_INITIALIZER ((struct effects_chain) EFFECTS_CHAIN_INITIALIZER_BARE)
#define IS_EFFECTS_CHAIN_START(x) ( \
	get_effect_info(x) != NULL \
	|| (x)[0] == ':' \
	|| (x)[0] == '@' \
	|| strcmp(x, "!") == 0 \
	|| strcmp(x, "{") == 0 \
)
#define EFFECTS_FILE_EOF_MARKER "#EOF#"

const struct effect_info * get_effect_info(const char *);
void destroy_effect(struct effect *);
void append_effect(struct effects_chain *, struct effect *);
int build_effects_chain(int, const char *const *, struct effects_chain *, struct stream_info *, const char *);
int build_effects_chain_from_file(const char *, struct effects_chain *, struct stream_info *, const char *, const char *, int);
ssize_t get_effects_chain_buffer_len(struct effects_chain *, ssize_t, int);
ssize_t get_effects_chain_max_out_frames(struct effects_chain *, ssize_t);
int effects_chain_needs_dither(struct effects_chain *);
int effects_chain_set_dither_params(struct effects_chain *, int, int);
sample_t * run_effects_chain(struct effect *, ssize_t *, sample_t *, sample_t *);
double get_effects_chain_delay(struct effects_chain *);
void reset_effects_chain(struct effects_chain *);
void signal_effects_chain(struct effects_chain *);
void plot_effects_chain(struct effects_chain *, int, int, int);
sample_t * drain_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void destroy_effects_chain(struct effects_chain *);
void print_all_effects(void);

struct effects_chain_xfade_state {
	sample_t *buf;
	struct effects_chain chain[2];
	struct stream_info istream, ostream;
	ssize_t frames, pos;
	int has_output;
};

#define EFFECTS_CHAIN_XFADE_TIME 100  /* milliseconds */
#define EFFECTS_CHAIN_XFADE_STATE_INITIALIZER ((struct effects_chain_xfade_state) { \
	.chain[0] = EFFECTS_CHAIN_INITIALIZER_BARE, \
	.chain[1] = EFFECTS_CHAIN_INITIALIZER_BARE, \
})

void effects_chain_xfade_reset(struct effects_chain_xfade_state *);
sample_t * effects_chain_xfade_run(struct effects_chain_xfade_state *, ssize_t *, sample_t *, sample_t *);

#endif
