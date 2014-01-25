#include <stdlib.h>
#include "util.h"
#include "dsp.h"

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
	CLEAR_BIT_ARRAY(b, n);
	if (s[0] == '\0' || (s[0] == '-' && s[1] == '\0')) {
		SET_BIT_ARRAY(b, n);
		return 0;
	}
	while (*s != '\0') {
		if (*s >= '0' && *s <= '9') {
			v = atoi(s);
			if (v > n - 1 || v < 0) {
				LOG(LL_ERROR, "dsp: parse_selector: value out of range: %d\n", v);
				return 1;
			}
			if (dash)
				end = atoi(s);
			else
				start = atoi(s);
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
	set_range(b, n, start, end, dash);
	return 0;
}

void print_selector(char *b, int n)
{
	int i, c, l = 0, f = 1, range_start = -1;
	for (i = 0; i < n; ++i) {
		c = GET_BIT(b, i);
		if (c && l)
			range_start = (range_start == -1) ? i - 1 : range_start;
		else if (!c && range_start != -1) {
			fprintf(stderr, "%s%d-%d", (f) ? "" : ",", range_start, i - 1);
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
		fprintf(stderr, "%s%d-%d", (f) ? "" : ",", range_start, n - 1);
	else if (l)
		fprintf(stderr, "%s%d", (f) ? "" : ",", n - 1);
}
