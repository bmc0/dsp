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

#ifndef DSP_EFFECT_H
#define DSP_EFFECT_H

#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);
	int effect_number;
};

enum {
	EFFECT_FLAG_PLOT_MIX         = 1<<0,  /* needs Ht*(f) for plotting */
	EFFECT_FLAG_OPT_REORDERABLE  = 1<<1,  /* may be reordered for optimization */
	EFFECT_FLAG_NO_DITHER        = 1<<2,  /* does not modify the signal such that dither is useful */
	EFFECT_FLAG_CH_DEPS_IDENTITY = 1<<3,  /* does not mix or reorder channels */
	EFFECT_FLAG_ALIGN_BARRIER    = 1<<4,  /* all input channels must be aligned */
};

struct effect {
	struct effect *prev, *next;
	const char *name;
	struct stream_info istream, ostream;
	char *channel_selector;  /* for use *only* by the effect */
	int flags;
	/* All functions may be NULL */
	int (*prepare)(struct effect *);
	sample_t * (*run)(struct effect *, ssize_t *, sample_t *, sample_t *);  /* if NULL, the effect will not be used */
	ssize_t (*delay)(struct effect *);  /* returns the latency in frames at ostream.fs */
	void (*reset)(struct effect *);
	void (*signal)(struct effect *);
	void (*plot)(struct effect *, int);
	void (*drain_samples)(struct effect *, ssize_t *);  /* cumulative drain samples for each output channel */
	sample_t * (*drain2)(struct effect *, ssize_t *, sample_t *, sample_t *);
	void (*destroy)(struct effect *);
	int (*merge)(struct effect *, struct effect *);  /* may not be called after prepare(); returns 1 if merged, 0 otherwise */
	ssize_t (*buffer_frames)(struct effect *, ssize_t);
	void (*channel_deps)(struct effect *, char **);  /* input channel dependencies for each output channel */
	void (*channel_offsets)(struct effect *, ssize_t *, ssize_t *);  /* cumulative latency and requested delay samples for each output channel */
	void *data;
};

const struct effect_info * get_effect_info(const char *);
void destroy_effect(struct effect *);
void effect_list_append(struct effect *, struct effect *);
void effect_plot_noop(struct effect *e, int);
void print_all_effects(void);
void print_effect_usage(const struct effect_info *);

#endif
