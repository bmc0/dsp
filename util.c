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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_FFTW3
	#include <complex.h>
	#include <fftw3.h>
	#include <pthread.h>
#endif
#include "util.h"

int check_endptr(const char *name, const char *str, const char *endptr, const char *param_name)
{
	if (endptr == str || *endptr != '\0') {
		if (!LOGLEVEL(LL_ERROR)) return 1;
		dsp_log_acquire();
		dsp_log_printf("%s: ", dsp_globals.prog_name);
		if (name) dsp_log_printf("%s: ", name);
		dsp_log_printf("failed to parse %s: %s\n", param_name, str);
		dsp_log_release();
		return 1;
	}
	return 0;
}

double parse_freq(const char *s, char **r_endptr)
{
	char *endptr;
	double f = strtod(s, &endptr);
	if (endptr != s) {
		if (*endptr == 'k') {
			f *= 1000.0;
			++endptr;
		}
		if (*endptr != '\0')
			dsp_perror(DSP_ETRCHAR, __func__, endptr);
	}
	if (r_endptr) *r_endptr = endptr;
	return f;
}

static double parse_len_frac_2(const char *s, double fs, char **r_endptr, int verbose)
{
	char *endptr;
	double d = strtod(s, &endptr);
	double samples = d * fs;
	if (endptr != s) {
		switch (*endptr) {
		case 'm':
			d /= 1000.0;
		case 's':
			samples = d * fs;
			++endptr;
			break;
		case 'S':
			samples = d;
			++endptr;
			break;
		}
		if (verbose && *endptr != '\0')
			dsp_perror(DSP_ETRCHAR, __func__, endptr);
	}
	if (r_endptr) *r_endptr = endptr;
	return samples;
}

ssize_t parse_len(const char *s, int fs, char **r_endptr)
{
	return lround(parse_len_frac_2(s, fs, r_endptr, 1));
}

double parse_len_frac(const char *s, double fs, char **r_endptr)
{
	return parse_len_frac_2(s, fs, r_endptr, 1);
}

ssize_t parse_timespec(const char *s, int fs, char **r_endptr)
{
	char *endptr;
	if (!strchr(s, ':'))
		return lround(parse_len_frac_2(s, fs, r_endptr, 0));
	double v = strtod(s, &endptr);
	const double sign = (signbit(v)) ? -1.0 : 1.0;
	for (int i = 0; *endptr == ':' && i < 2; ++i)
		v = v*60.0 + strtod(endptr+1, &endptr)*sign;
	if (r_endptr) *r_endptr = endptr;
	return lround(v * fs);
}

static void set_range(char *b, int n, int start, int end, int dash)
{
	if (start == -1 && end == -1) {
		start = 0;
		end = n - 1;
	}
	else if (start == -1)
		start = 0;
	else if (end == -1) {
		if (dash)
			end = n - 1;
		else
			end = start;
	}
	for (; start <= end; ++start)
		SET_BIT(b, start);
}

int parse_selector(const char *s, char *b, int n)
{
	int v, start = -1, end = -1, dash = 0;
	CLEAR_SELECTOR(b, n);
	if (s[0] == '\0' || (s[0] == '-' && s[1] == '\0')) {
		SET_SELECTOR(b, n);
		return 0;
	}
	while (*s != '\0') {
		if (*s >= '0' && *s <= '9') {
			v = atoi(s);
			if (v > n - 1 || v < 0) {
				LOG_FMT(LL_ERROR, "%s: error: value out of range: %d", __func__, v);
				return 1;
			}
			if (dash) {
				if (v < start) {
					LOG_FMT(LL_ERROR, "%s: error: malformed range: %d-%d", __func__, (start == -1) ? 0 : start, v);
					return 1;
				}
				end = v;
			}
			else
				start = v;
			while (*s >= '0' && *s <= '9')
				++s;
		}
		else if (*s == '-') {
			if (dash) {
				LOG_FMT(LL_ERROR, "%s: syntax error: '-' unexpected", __func__);
				return 1;
			}
			dash = 1;
			++s;
		}
		else if (*s == ',' || *s == '\0' ) {
			if (start == -1 && end == -1 && !dash) {
				LOG_FMT(LL_ERROR, "%s: syntax error: ',' unexpected", __func__);
				return 1;
			}
			set_range(b, n, start, end, dash);
			start = end = -1;
			dash = 0;
			if (*s != '\0')
				++s;
		}
		else {
			LOG_FMT(LL_ERROR, "%s: syntax error: invalid character: %c", __func__, *s);
			return 1;
		}
	}
	if (start == -1 && end == -1 && !dash) {
		LOG_FMT(LL_ERROR, "%s: syntax error: ',' unexpected", __func__);
		return 1;
	}
	set_range(b, n, start, end, dash);
	return 0;
}

int parse_selector_masked(const char *s, char *b, const char *mask, int n)
{
	int r;
	CLEAR_SELECTOR(b, n);
	const int nb = num_bits_set(mask, n);
	char *b_tmp = NEW_SELECTOR(nb);
	if (check_alloc(__func__, b_tmp)) return 1;
	if ((r = parse_selector(s, b_tmp, nb))) {
		free(b_tmp);
		return r;
	}
	for (int i = 0, k = 0; i < nb; ++i, ++k) {
		while (k < n && !GET_BIT(mask, k)) ++k;
		if (k == n) {
			LOG_FMT(LL_ERROR, "%s(): BUG: too many channels", __func__);
			free(b_tmp);
			return 1;
		}
		if (GET_BIT(b_tmp, i))
			SET_BIT(b, k);
	}
	free(b_tmp);
	return 0;
}

void print_selector(const char *b, int n)
{
	int i, c, l = 0, f = 1, range_start = -1;
	for (i = 0; i < n; ++i) {
		c = GET_BIT(b, i);
		if (c && l)
			range_start = (range_start == -1) ? i - 1 : range_start;
		else if (!c && range_start != -1) {
			dsp_log_printf("%s%d%s%d", (f) ? "" : ",", range_start, (i - range_start == 2) ? "," : "-", i - 1);
			range_start = -1;
			f = 0;
		}
		else if (l) {
			dsp_log_printf("%s%d", (f) ? "" : ",", i - 1);
			f = 0;
		}
		l = c;
	}
	if (range_start != -1)
		dsp_log_printf("%s%d%s%d", (f) ? "" : ",", range_start, (i - range_start == 2) ? "," : "-", i - 1);
	else if (l)
		dsp_log_printf("%s%d", (f) ? "" : ",", n - 1);
}

int num_bits_set(const char *b, int n)
{
	int c = 0;
	for (int i = 0; i < n; ++i)
		if (GET_BIT(b, i)) ++c;
	return c;
}

char * get_file_contents(const char *path)
{
	ssize_t s = 2048, p = 0, r;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		return NULL;
	char *c = malloc(s * sizeof(char));
	if (check_alloc(__func__, c)) goto fail;
	while ((r = read(fd, &c[p], s - p)) > -1) {
		p += r;
		if (p >= s) {
			s += 2048;
			char *c_tmp = realloc(c, s * sizeof(char));
			if (check_alloc(__func__, c_tmp)) goto fail;
			c = c_tmp;
		}
		if (r == 0) {
			c[p] = '\0';
			close(fd);
			return c;
		}
	}
	fail:
	free(c);
	close(fd);
	return NULL;
}

char * construct_full_path(const char *dir, const char *path, int fs, int channels)
{
	int pos = 0, len = strlen(path) + 16 + 1;
	char *fp = NULL;
	if (path[0] != '\0' && path[0] == '~' && path[1] == '/') {
		char *env = getenv("HOME");
		path += 1;
		if (env) {
			len += strlen(env);
			fp = calloc(len, sizeof(char));
			if (check_alloc(__func__, fp)) return NULL;
			pos = snprintf(fp, len, "%s", env);
		}
		else LOG_FMT(LL_ERROR, "%s(): warning: $HOME is unset", __func__);
	}
	else if (dir != NULL && path[0] != '/') {
		len += strlen(dir) + 1;
		fp = calloc(len, sizeof(char));
		if (check_alloc(__func__, fp)) return NULL;
		pos = snprintf(fp, len, "%s/", dir);
	}
	if (!fp) {
		fp = calloc(len, sizeof(char));
		if (check_alloc(__func__, fp)) return NULL;
	}
	while (*path != '\0') {
		int w = 1, has_subst = (path[0] == '%' && path[1] != '\0');
		if (has_subst) {
			++path;
			write_subst:
			switch (*path) {
			case 'r':
				w = snprintf(fp+pos, len-pos, "%d", fs);
				break;
			case 'k':
				w = snprintf(fp+pos, len-pos, "%.10g", fs/1000.0);
				break;
			case 'c':
				w = snprintf(fp+pos, len-pos, "%d", channels);
				break;
			case '%':
				if (pos+1 < len) fp[pos] = '%';
				break;
			default:
				--path;
				has_subst = 0;
				if (pos+1 < len) fp[pos] = '%';
			}
		}
		else if (pos+1 < len) fp[pos] = *path;
		if (pos+w >= len) {
			while (len <= pos+w) len += 32;
			char *fp_tmp = realloc(fp, len);
			/* useless !fp_tmp silences GCC warning... */
			if (!fp_tmp && check_alloc(__func__, fp_tmp)) {
				free(fp);
				return NULL;
			}
			fp = fp_tmp;
			if (has_subst) goto write_subst;
			else fp[pos] = *path;
		}
		pos += w;
		++path;
	}
	fp[pos] = '\0';
	return fp;
}

char * isolate(char *s, char c)
{
	while (*s && *s != c) ++s;
	if (*s != '\0') *s++ = '\0';
	return s;
}

/*
 * Slightly modified version of AT&T public domain getopt():
 *   - Returns ':' for a missing option argument.
 *   - Supports optional arguments (two colons).
 *   - Thread safe.
*/
#define IS_OPT(S) ((S)[0] == '-' && (S)[1] != '\0')
int dsp_getopt(struct dsp_getopt_state *g, int argc, const char *const *argv, const char *opts)
{
	int c;
	const char *cp;

	if (g->sp == 1) {
		if (g->ind >= argc || !IS_OPT(argv[g->ind]))
			return -1;
		else if (strcmp(argv[g->ind], "--") == 0) {
			++g->ind;
			return -1;
		}
	}
	g->opt = c = argv[g->ind][g->sp];
	if (c == ':' || (cp = strchr(opts, c)) == NULL) {
		if (argv[g->ind][++g->sp] == '\0') {
			++g->ind;
			g->sp = 1;
		}
		g->opt = c;
		return '?';
	}
	if (cp[1] == ':') {
		if (argv[g->ind][g->sp + 1] != '\0')
			g->arg = &argv[g->ind++][g->sp + 1];
		else if (cp[2] == ':') {
			++g->ind;
			g->arg = NULL;
		}
		else if (++g->ind >= argc) {
			g->sp = 1;
			return ':';
		}
		else g->arg = argv[g->ind++];
		g->sp = 1;
	}
	else {
		if (argv[g->ind][++g->sp] == '\0') {
			++g->ind;
			g->sp = 1;
		}
		g->arg = NULL;
	}
	return c;
}

void dsp_getopt_print_error(struct dsp_getopt_state *g, int opt, const char *name)
{
	if (!LOGLEVEL(LL_ERROR)) return;
	const char *errmsg = (opt == ':')
		? "expected argument to option"
		: "unrecognized option";
	dsp_log_acquire();
	dsp_log_printf("%s: ", dsp_globals.prog_name);
	if (name) dsp_log_printf("%s: ", name);
	dsp_log_printf("%s '%c'\n", errmsg, g->opt);
	dsp_log_release();
}

#ifdef HAVE_FFTW3
ssize_t next_fast_fftw_len(ssize_t min_len)
{
	ssize_t best = min_len * 7, bound = min_len * 2, p2 = 1;
	for (int i2 = 0; p2 <= bound; ++i2) {
		ssize_t p3 = p2;
		for (int i3 = 0; p3 <= bound; ++i3) {
			ssize_t p5 = p3;
			for (int i5 = 0; p5 <= bound; ++i5) {
				ssize_t p7 = p5;
				for (int i7 = 0; p7 <= bound; ++i7) {
					if (p7 < best && p7 >= min_len) {
						best = p7;
						/* LOG_FMT(LL_VERBOSE, "next_fast_fftw_len(): best=%zd (2^%d * 3^%d * 5^%d * 7^%d)", best, i2, i3, i5, i7); */
						(void) i2; (void) i3; (void) i5; (void) i7;  /* silence unused variable warning */
					}
					p7 *= 7;
				}
				p5 *= 5;
			}
			p3 *= 3;
		}
		p2 *= 2;
	}
	return best;
}

static pthread_mutex_t fftw_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *wisdom_path = NULL;
static int wisdom_loaded = 0;

void dsp_fftw_acquire(void)
{
	pthread_mutex_lock(&fftw_lock);
}

void dsp_fftw_release(void)
{
	pthread_mutex_unlock(&fftw_lock);
}

int dsp_fftw_load_wisdom(void)
{
	if (!wisdom_loaded) {
		wisdom_loaded = 1;
		#ifdef LADSPA_FRONTEND
			wisdom_path = getenv("LADSPA_DSP_FFTW_WISDOM_PATH");
		#else
			wisdom_path = getenv("DSP_FFTW_WISDOM_PATH");
		#endif
		if (wisdom_path) {
			if (fftw_import_wisdom_from_filename(wisdom_path))
				LOG_FMT(LL_VERBOSE, "info: loaded FFTW wisdom: %s", wisdom_path);
			else LOG_FMT(LL_VERBOSE, "info: failed to load FFTW wisdom: %s", wisdom_path);
		}
	}
	return (wisdom_path != NULL);
}

void dsp_fftw_save_wisdom(void)
{
	if (wisdom_path) {
		if (fftw_export_wisdom_to_filename(wisdom_path))
			LOG_FMT(LL_VERBOSE, "info: saved FFTW wisdom: %s", wisdom_path);
		else LOG_FMT(LL_VERBOSE, "info: failed to save FFTW wisdom: %s", wisdom_path);
	}
	wisdom_loaded = 0;
}
#endif

int dsp_log_printf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	const int r = vfprintf(stderr, format, ap);
	va_end(ap);
	return r;
}

int dsp_log_puts(const char *s)
{
	return fputs(s, stderr);
}

int dsp_log_putc(int c)
{
	return putc(c, stderr);
}

void dsp_log_safe_fmt(const char *format, ...)
{
	va_list ap;
	dsp_log_acquire();
	fprintf(stderr, "%s: ", dsp_globals.prog_name);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	putc('\n', stderr);
	dsp_log_release();
}

const char *err_strs[] = {
	[DSP_ENONE]     = "no error",
	[DSP_ENOMEM]    = "no memory",
	[DSP_EREAD]     = "read",
	[DSP_EWRITE]    = "write",
	[DSP_ESEEK]     = "seek",
	[DSP_EBADENC]   = "bad encoding",
	[DSP_ERANGE]    = "parameter out of range",
	[DSP_ENOEFFNUM] = "BUG: unknown effect number",
	[DSP_ETRCHAR]   = "trailing characters",
};

const char * dsp_strerror(int e)
{
	if (e < 0 || e >= LENGTH(err_strs))
		return "BUG: unknown error";
	return err_strs[e];
}

void dsp_perror(int e, const char *name, const char *msg)
{
	if (!LOGLEVEL(LL_ERROR)) return;
	dsp_log_acquire();
	fprintf(stderr, "%s: ", dsp_globals.prog_name);
	if (name) fprintf(stderr, "%s: ", name);
	fprintf(stderr, "error: %s", dsp_strerror(e));
	if (msg) fprintf(stderr, ": %s", msg);
	putc('\n', stderr);
	dsp_log_release();
}
