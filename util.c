#include <stdlib.h>
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

int parse_effects_chain(char **argv, int argc, struct effects_chain *chain, struct stream_info *stream, char *channel_selector)
{
	int i = 1, k = 0, j, last_selector_index = -1;
	char *tmp_channel_selector;
	struct effect_info *ei = NULL;
	struct effect *e = NULL;

	while (k < argc) {
		if (argv[k][0] == ':') {
			if (parse_selector(&argv[k][1], channel_selector, stream->channels))
				return 1;
			last_selector_index = k++;
			i = k + 1;
			continue;
		}
		ei = get_effect_info(argv[k]);
		if (ei == NULL) {
			LOG(LL_ERROR, "dsp: error: no such effect: %s\n", argv[k]);
			return 1;
		}
		while (i < argc && get_effect_info(argv[i]) == NULL && argv[i][0] != ':')
			++i;
		if (LOGLEVEL(LL_VERBOSE)) {
			fprintf(stderr, "dsp: effect:");
			for (j = 0; j < i - k; ++j)
				fprintf(stderr, " %s", argv[k + j]);
			fprintf(stderr, "; channels=%d [", stream->channels);
			print_selector(channel_selector, stream->channels);
			fprintf(stderr, "] fs=%d\n", stream->fs);
		}
		e = init_effect(ei, stream, channel_selector, i - k, &argv[k]);
		if (e == NULL) {
			LOG(LL_ERROR, "dsp: error: failed to initialize effect: %s\n", argv[k]);
			return 1;
		}
		append_effect(chain, e);
		k = i;
		i = k + 1;
		if (e->ostream.channels != stream->channels) {
			tmp_channel_selector = NEW_BIT_ARRAY(e->ostream.channels);
			if (last_selector_index == -1)
				SET_BIT_ARRAY(tmp_channel_selector, stream->channels);
			else {
				if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, e->ostream.channels))
					return 1;
			}
			free(channel_selector);
			channel_selector = tmp_channel_selector;
		}
		*stream = e->ostream;
	}
	return 0;
}
