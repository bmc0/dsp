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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <complex.h>
#include <fftw3.h>
#include <pthread.h>
#include "util.h"

int check_endptr(const char *name, const char *str, const char *endptr, const char *param_name)
{
	if (endptr == str || *endptr != '\0') {
		if (name == NULL) LOG_FMT(LL_ERROR, "failed to parse %s: %s", param_name, str);
		else LOG_FMT(LL_ERROR, "%s: failed to parse %s: %s", name, param_name, str);
		return 1;
	}
	return 0;
}

double parse_freq(const char *s, char **endptr)
{
	double f = strtod(s, endptr);
	if (*endptr != NULL && *endptr != s) {
		if (**endptr == 'k') {
			f *= 1000.0;
			++(*endptr);
		}
		if (**endptr != '\0') LOG_FMT(LL_ERROR, "%s(): trailing characters: %s", __func__, *endptr);
	}
	return f;
}

ssize_t parse_len(const char *s, int fs, char **endptr)
{
	double d = strtod(s, endptr);
	ssize_t samples = lround(d * fs);
	if (*endptr != NULL && *endptr != s) {
		switch (**endptr) {
		case 'm':
			d /= 1000.0;
		case 's':
			samples = lround(d * fs);
			++(*endptr);
			break;
		case 'S':
			samples = lround(d);
			++(*endptr);
			break;
		}
		if (**endptr != '\0') LOG_FMT(LL_ERROR, "%s(): trailing characters: %s", __func__, *endptr);
	}
	return samples;
}

double parse_len_frac(const char *s, double fs, char **endptr)
{
	double d = strtod(s, endptr);
	double samples = d * fs;
	if (*endptr != NULL && *endptr != s) {
		switch (**endptr) {
		case 'm':
			d /= 1000.0;
		case 's':
			samples = d * fs;
			++(*endptr);
			break;
		case 'S':
			samples = d;
			++(*endptr);
			break;
		}
		if (**endptr != '\0') LOG_FMT(LL_ERROR, "%s(): trailing characters: %s", __func__, *endptr);
	}
	return samples;
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
				LOG_FMT(LL_ERROR, "%s(): error: value out of range: %d", __func__, v);
				return 1;
			}
			if (dash) {
				if (v < start) {
					LOG_FMT(LL_ERROR, "%s(): error: malformed range: %d-%d", __func__, (start == -1) ? 0 : start, v);
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
				LOG_FMT(LL_ERROR, "%s(): syntax error: '-' unexpected", __func__);
				return 1;
			}
			dash = 1;
			++s;
		}
		else if (*s == ',' || *s == '\0' ) {
			if (start == -1 && end == -1 && !dash) {
				LOG_FMT(LL_ERROR, "%s(): syntax error: ',' unexpected", __func__);
				return 1;
			}
			set_range(b, n, start, end, dash);
			start = end = -1;
			dash = 0;
			if (*s != '\0')
				++s;
		}
		else {
			LOG_FMT(LL_ERROR, "%s(): syntax error: invalid character: %c", __func__, *s);
			return 1;
		}
	}
	if (start == -1 && end == -1 && !dash) {
		LOG_FMT(LL_ERROR, "%s(): syntax error: ',' unexpected", __func__);
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
	if ((r = parse_selector(s, b_tmp, nb))) {
		free(b_tmp);
		return r;
	}
	for (int i = 0, k = 0; i < nb; ++i, ++k) {
		while (k < n && !GET_BIT(mask, k)) ++k;
		if (k == n) {
			LOG_FMT(LL_ERROR, "%s(): BUG: too many channels", __func__);
			break;
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

static void strip_char(char *str, char c, int is_esc)
{
	int i = 0, k = 0, esc = 0;
	while (str[k] != '\0') {
		if (!esc && str[k] == c) {
			if (is_esc)
				esc = 1;
			++k;
		}
		else {
			str[i++] = str[k++];
			esc = 0;
		}
	}
	str[i] = '\0';
}

int gen_argv_from_string(const char *str, int *argc, char ***argv)
{
	int i = 0, k = 0, n = 1, esc = 0, end = 0;
	char *tmp;

	*argc = 0;
	*argv = NULL;

	while (!end) {
		if (!esc && str[k] == '\\') {
			esc = 1;
			n = 0;
		}
		else if (n && str[k] == '#') {
			while (str[k] != '\0' && str[k] != '\n')
				++k;
			i = k;
			n = 0;
			continue;
		}
		else if ((!esc && IS_WHITESPACE(str[k])) || str[k] == '\0') {
			if (str[k] == '\n')
				n = 1;
			else if (str[k] == '\0')
				end = 1;
			if (i != k) {
				tmp = strndup(&str[i], k - i);
				strip_char(tmp, '\\', 1);
				*argv = realloc(*argv, (*argc + 1) * sizeof(char *));
				(*argv)[(*argc)++] = tmp;
				i = k;
			}
			++i;
		}
		else {
			if (n && !IS_WHITESPACE(str[k]))
				n = 0;
			esc = 0;
		}
		++k;
	}
	return 0;
}

char * get_file_contents(const char *path)
{
	const size_t g = 512;
	ssize_t s = g, p = 0, r;
	int fd;
	char *c;

	if ((fd = open(path, O_RDONLY)) < 0)
		return NULL;
	c = calloc(s, sizeof(char));
	while ((r = read(fd, &c[p], s - p)) > -1) {
		p += r;
		if (p >= s) {
			s += g;
			c = realloc(c, s * sizeof(char));
		}
		if (r == 0) {
			c[p] = '\0';
			close(fd);
			return c;
		}
	}
	free(c);
	close(fd);
	return NULL;
}

char * construct_full_path(const char *dir, const char *path)
{
	int i;
	char *env, *p;
	if (path[0] != '\0' && path[0] == '~' && path[1] == '/') {
		env = getenv("HOME");
		if (env) {
			i = strlen(env) + strlen(&path[1]) + 1;
			p = calloc(i, sizeof(char));
			snprintf(p, i, "%s%s", env, &path[1]);
		}
		else {
			LOG_FMT(LL_ERROR, "%s(): warning: $HOME is unset", __func__);
			p = strdup(&path[1]);
		}
	}
	else if (dir == NULL || path[0] == '/')
		p = strdup(path);
	else {
		i = strlen(dir) + 1 + strlen(path) + 1;
		p = calloc(i, sizeof(char));
		snprintf(p, i, "%s/%s", dir, path);
	}
	return p;
}

char * isolate(char *s, char c)
{
	while (*s && *s != c) ++s;
	if (*s != '\0') *s++ = '\0';
	return s;
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
