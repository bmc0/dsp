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

#ifndef DSP_DSP_H
#define DSP_DSP_H

#include <stdio.h>
#include <sys/types.h>

enum {
	LL_SILENT = 0,
	LL_ERROR,
	LL_OPEN_ERROR,
	LL_NORMAL,
	LL_VERBOSE,
};
#define LOGLEVEL(l) (dsp_globals.loglevel >= (l))
#define dsp_log_printf(...) fprintf(stderr, __VA_ARGS__)
#define dsp_log_puts(str)   fputs(str, stderr)
#define dsp_log_putc(c)     putc(c, stderr)
#define LOG_FMT(l, fmt, ...) \
	do { \
		if (LOGLEVEL(l)) { \
			dsp_log_acquire(); \
			dsp_log_printf("%s: " fmt "\n", dsp_globals.prog_name, __VA_ARGS__); \
			dsp_log_release(); \
		} \
	} while (0)
#define LOG_S(l, s) \
	do { \
		if (LOGLEVEL(l)) { \
			dsp_log_acquire(); \
			dsp_log_printf("%s: %s\n", dsp_globals.prog_name, s); \
			dsp_log_release(); \
		} \
	} while (0)

#define DEFAULT_FS       44100
#define DEFAULT_CHANNELS     1
#define BIT_PERFECT          1

#define DEFAULT_BLOCK_FRAMES     2048
#define DEFAULT_INPUT_BUF_RATIO    64
#define DEFAULT_OUTPUT_BUF_RATIO    8

typedef double sample_t;

struct dsp_globals {
	int loglevel;
	const char *prog_name;
};

struct stream_info {
	int fs, channels;
};

extern struct dsp_globals dsp_globals;
void dsp_log_acquire(void);
void dsp_log_release(void);

#ifndef LADSPA_FRONTEND
#define DSP_STATUSLINES
#define DSP_STATUSLINE_MAX_LEN 256
struct statusline_state {
	struct statusline_state *next, *prev;
	char s[DSP_STATUSLINE_MAX_LEN];
};

void dsp_statuslines_acquire(void);
void dsp_statuslines_release(void);
void dsp_statusline_register(struct statusline_state *);
void dsp_statusline_unregister(struct statusline_state *);

/* must wrap in dsp_log_{acquire,release}() calls */
void dsp_get_term_size(int *, int *);
#endif

#endif
