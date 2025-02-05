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

#ifndef DSP_UTIL_H
#define DSP_UTIL_H

#include <stdint.h>
#include <string.h>
#include "dsp.h"

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define MAXIMUM(a, b) (((a) > (b)) ? (a) : (b))
#define MINIMUM(a, b) (((a) < (b)) ? (a) : (b))
#define CHECK_RANGE(cond, name, action) \
	do { if (!(cond)) { \
		LOG_FMT(LL_ERROR, "%s: error: %s out of range", argv[0], name); \
		action; \
	} } while (0)
#define CHECK_FREQ(var, fs, name, action) \
	CHECK_RANGE((var) >= 0.0 && (var) < (double) (fs) / 2.0, name, action)
#define CHECK_ENDPTR(str, endptr, param_name, action) \
	do { if (check_endptr(argv[0], str, endptr, param_name)) { action; } } while (0)
#if 0
#define GET_BIT(x, o) (((char *) x)[(int) (o) / 8] & (1 << ((int) (o) % 8)))
#define SET_BIT(x, o) ((char *) x)[(int) (o) / 8] |= (1 << ((int) (o) % 8))
#define SET_SELECTOR(x, n) memset(x, 0xff, ((n) - 1) / 8 + 1)
#define CLEAR_SELECTOR(x, n) memset(x, 0x00, ((n) - 1) / 8 + 1)
#define NEW_SELECTOR(n) calloc(((n) - 1) / 8 + 1, sizeof(char))
#define COPY_SELECTOR(dest, src, n) memcpy(dest, src, ((n) - 1) / 8 + 1)
#else
#define GET_BIT(x, o) ((x)[(o)])
#define SET_BIT(x, o) (x)[(o)] = 1
#define SET_SELECTOR(x, n) memset(x, 0x01, n)
#define CLEAR_SELECTOR(x, n) memset(x, 0x00, n)
#define NEW_SELECTOR(n) calloc(n, sizeof(char))
#define COPY_SELECTOR(dest, src, n) memcpy(dest, src, n)
#endif
#define TEST_BIT(x, o, s) (!!GET_BIT(x, o) == !!(s))
#define IS_POWER_OF_2(x) ((x) && !((x)&((x)-1)))
#define IS_WHITESPACE(x) ((x) == ' ' || (x) == '\t' || (x) == '\n' || (x) == '\r')
#define PM_RAND_MAX 0x7fffffff

struct dsp_getopt_state {
	const char *arg;
	int ind, opt, sp;
};
#define DSP_GETOPT_STATE_INITIALIZER ((struct dsp_getopt_state) { .ind = 1, .sp = 1 })

int check_endptr(const char *, const char *, const char *, const char *);
double parse_freq(const char *, char **);
ssize_t parse_len(const char *, int, char **);
double parse_len_frac(const char *, double, char **);
int parse_selector(const char *, char *, int);
int parse_selector_masked(const char *, char *, const char *, int);
void print_selector(const char *, int);
int num_bits_set(const char *, int);
int gen_argv_from_string(const char *, int *, char ***);
char * get_file_contents(const char *);
char * construct_full_path(const char *, const char *);
char * isolate(char *, char);
int dsp_getopt(struct dsp_getopt_state *, int, const char *const *, const char *);
#ifdef HAVE_FFTW3
ssize_t next_fast_fftw_len(ssize_t);
void dsp_fftw_acquire(void);
void dsp_fftw_release(void);
int dsp_fftw_load_wisdom(void);   /* Not MT-safe. Call before planning; returns true if a path was specified. */
void dsp_fftw_save_wisdom(void);  /* Called at exit--do not use anywhere else. */
#endif

#ifdef INT64_MAX
#define PM_RAND_R_DEFINE_FUNC(func_name, A) \
	static inline uint32_t func_name(uint32_t *s) \
	{ \
		uint64_t p = (uint64_t) *s * (A); \
		uint32_t r = (p & 0x7fffffff) + (p >> 31); \
		r = (r & 0x7fffffff) + (r >> 31); \
		return *s = r; \
	}
#else
#define PM_RAND_R_DEFINE_FUNC(func_name, A) \
	static inline uint32_t func_name(uint32_t *s) \
	{ \
		uint32_t l = (*s & 0x7fff) * (A); \
		uint32_t h = (*s >> 15) * (A); \
		uint32_t r = l + ((h & 0xffff) << 15) + (h >> 16); \
		r = (r & 0x7fffffff) + (r >> 31); \
		return *s = r; \
	}
#endif

PM_RAND_R_DEFINE_FUNC(pm_rand1_r, 48271)
PM_RAND_R_DEFINE_FUNC(pm_rand2_r, 16807)

static inline uint32_t pm_rand(void)
{
	static uint32_t s = 1;
	return pm_rand1_r(&s);
}

static inline sample_t tpdf_dither_get_mult(int prec)
{
	if (prec < 1 || prec > 32)
		return 0.0;
	uint32_t d = ((uint32_t) 1) << (prec - 1);
	return 1.0 / ((sample_t) PM_RAND_MAX * d);
}

static inline sample_t tpdf_noise(sample_t mult)
{
#if 1
	/* Faster and gives better quality noise */
	static uint32_t s0 = 1, s1 = 1;
	int32_t n1 = pm_rand1_r(&s0);
	int32_t n2 = pm_rand2_r(&s1);
	return (n1 - n2) * mult;
#else
	int32_t n1 = pm_rand();
	int32_t n2 = pm_rand();
	return (n1 - n2) * mult;
#endif
}

static inline ssize_t ratio_mult_ceil(ssize_t v, int n, int d)
{
	long long int r = (long long int) v * n;
	return (ssize_t) ((r % d != 0) ? r / d + 1 : r / d);
}

static inline int find_gcd(int a, int b)
{
	int c;
	while (b != 0) {
		c = b;
		b = a % b;
		a = c;
	}
	return a;
}

#endif
