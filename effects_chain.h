/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#ifndef DSP_EFFECTS_CHAIN_H
#define DSP_EFFECTS_CHAIN_H

#include "dsp.h"
#include "effect.h"

struct effects_chain {
	struct effect *head, *tail;
	struct stream_info istream, ostream;
	struct { int n, d; } ratio;
	ssize_t drain_frames, iframes, oframes;
	ssize_t zero_ref;
	int delay, frac;
};

#define EFFECTS_CHAIN_INITIALIZER {0}
#define IS_EFFECTS_CHAIN_START(x) is_effect_or_token(x)
#define EFFECTS_FILE_EOF_MARKER "#EOF#"

void effects_chain_append(struct effects_chain *, struct effect *);
int is_effect_or_token(const char *);
int build_effects_chain_from_argv(int, const char *const *, struct effects_chain *, struct stream_info *, const char *, const char *);
int build_effects_chain_from_string(const char *, const char *, struct effects_chain *, struct stream_info *, const char *, const char *);
int build_effects_chain_from_file(const char *, struct effects_chain *, struct stream_info *, const char *, const char *, int);
ssize_t get_effects_chain_buffer_len(struct effects_chain *, ssize_t, int);
ssize_t get_effects_chain_max_out_frames(struct effects_chain *, ssize_t);
int effects_chain_needs_dither(struct effects_chain *);
int effects_chain_set_dither_params(struct effects_chain *, int, int);
sample_t * run_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
double get_effects_chain_delay(struct effects_chain *, int);
void reset_effects_chain(struct effects_chain *);
void signal_effects_chain(struct effects_chain *);
void plot_effects_chain(struct effects_chain *, int);
sample_t * drain_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void destroy_effects_chain(struct effects_chain *);

struct effects_chain_xfade_state {
	sample_t *buf;
	struct effects_chain chain[2];
	ssize_t frames, pos;
};

#define EFFECTS_CHAIN_XFADE_TIME 100  /* milliseconds */
#define EFFECTS_CHAIN_XFADE_STATE_INITIALIZER { \
	.chain[0] = EFFECTS_CHAIN_INITIALIZER, \
	.chain[1] = EFFECTS_CHAIN_INITIALIZER, \
}

void effects_chain_xfade_reset(struct effects_chain_xfade_state *);
sample_t * effects_chain_xfade_run(struct effects_chain_xfade_state *, ssize_t *, sample_t *, sample_t *);

#endif
