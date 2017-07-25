#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "util.h"

double parse_freq(const char *s)
{
	size_t len = strlen(s);
	if (len < 1)
		return 0;
	if (s[len - 1] == 'k')
		return atof(s) * 1000;
	else
		return atof(s);
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
				LOG(LL_ERROR, "dsp: parse_selector: error: value out of range: %d\n", v);
				return 1;
			}
			if (dash) {
				if (v < start) {
					LOG(LL_ERROR, "dsp: parse_selector: error: malformed range: %d-%d\n", (start == -1) ? 0 : start, v);
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
				LOG(LL_ERROR, "dsp: parse_selector: syntax error: '-' unexpected\n");
				return 1;
			}
			dash = 1;
			++s;
		}
		else if (*s == ',' || *s == '\0' ) {
			if (start == -1 && end == -1 && !dash) {
				LOG(LL_ERROR, "dsp: parse_selector: syntax error: ',' unexpected\n");
				return 1;
			}
			set_range(b, n, start, end, dash);
			start = end = -1;
			dash = 0;
			if (*s != '\0')
				++s;
		}
		else {
			LOG(LL_ERROR, "dsp: parse_selector: syntax error: invalid character: %c\n", *s);
			return 1;
		}
	}
	if (start == -1 && end == -1 && !dash) {
		LOG(LL_ERROR, "dsp: parse_selector: syntax error: ',' unexpected\n");
		return 1;
	}
	set_range(b, n, start, end, dash);
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
			fprintf(stderr, "%s%d%s%d", (f) ? "" : ",", range_start, (i - range_start == 2) ? "," : "-", i - 1);
			range_start = -1;
			f = 0;
		}
		else if (l) {
			fprintf(stderr, "%s%d", (f) ? "" : ",", i - 1);
			f = 0;
		}
		l = c;
	}
	if (range_start != -1)
		fprintf(stderr, "%s%d%s%d", (f) ? "" : ",", range_start, (i - range_start == 2) ? "," : "-", i - 1);
	else if (l)
		fprintf(stderr, "%s%d", (f) ? "" : ",", n - 1);
}

#define IS_WHITESPACE(x) (x == ' ' || x == '\t' || x == '\n')

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
		i = strlen(env) + strlen(&path[1]) + 1;
		p = calloc(i, sizeof(char));
		snprintf(p, i, "%s%s", env, &path[1]);
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
