#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
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

int build_effects_chain(int argc, char **argv, struct effects_chain *chain, struct stream_info *stream, char *channel_selector, const char *dir)
{
	int i = 1, k = 0, j, last_selector_index = -1, old_stream_channels;
	char *tmp_channel_selector;
	struct effect_info *ei = NULL;
	struct effect *e = NULL;

	if (channel_selector == NULL) {
		channel_selector = NEW_BIT_ARRAY(stream->channels);
		SET_BIT_ARRAY(channel_selector, stream->channels);
	}
	else {
		tmp_channel_selector = NEW_BIT_ARRAY(stream->channels);
		COPY_BIT_ARRAY(tmp_channel_selector, channel_selector, stream->channels);
		channel_selector = tmp_channel_selector;
	}

	while (k < argc) {
		if (argv[k][0] == ':') {
			if (parse_selector(&argv[k][1], channel_selector, stream->channels))
				goto fail;
			last_selector_index = k++;
			i = k + 1;
			continue;
		}
		if (argv[k][0] == '@') {
			old_stream_channels = stream->channels;
			if (build_effects_chain_from_file(chain, stream, channel_selector, dir, &argv[k][1]))
				goto fail;
			if (stream->channels != old_stream_channels) {
				tmp_channel_selector = NEW_BIT_ARRAY(stream->channels);
				if (last_selector_index == -1)
					SET_BIT_ARRAY(tmp_channel_selector, stream->channels);
				else if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, stream->channels)) {
					free(tmp_channel_selector);
					goto fail;
				}
				free(channel_selector);
				channel_selector = tmp_channel_selector;
			}
			i = ++k + 1;
			continue;
		}
		ei = get_effect_info(argv[k]);
		if (ei == NULL) {
			LOG(LL_ERROR, "dsp: error: no such effect: %s\n", argv[k]);
			goto fail;
		}
		while (i < argc && get_effect_info(argv[i]) == NULL && argv[i][0] != ':' && argv[i][0] != '@')
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
			goto fail;
		}
		append_effect(chain, e);
		k = i;
		i = k + 1;
		if (e->ostream.channels != stream->channels) {
			tmp_channel_selector = NEW_BIT_ARRAY(e->ostream.channels);
			if (last_selector_index == -1)
				SET_BIT_ARRAY(tmp_channel_selector, e->ostream.channels);
			else if (parse_selector(&argv[last_selector_index][1], tmp_channel_selector, e->ostream.channels)) {
				free(tmp_channel_selector);
				goto fail;
			}
			free(channel_selector);
			channel_selector = tmp_channel_selector;
		}
		*stream = e->ostream;
	}
	free(channel_selector);
	return 0;

	fail:
	free(channel_selector);
	return 1;
}

int build_effects_chain_from_file(struct effects_chain *chain, struct stream_info *stream, char *channel_selector, const char *dir, const char *filename)
{
	char **argv = NULL, *env, *tmp = NULL, *path = NULL, *contents = NULL;
	size_t s;
	off_t file_size;
	int i, argc = 0, fd = -1;

	if (filename[0] != '\0' && filename[0] == '~' && filename[1] == '/') {
		env = getenv("HOME");
		s = strlen(env) + strlen(&filename[1]) + 1;
		path = calloc(s, sizeof(char));
		snprintf(path, s, "%s%s", env, &filename[1]);
	}
	else if (dir == NULL || filename[0] == '/')
		path = strdup(filename);
	else {
		s = strlen(dir) + 1 + strlen(filename) + 1;
		path = calloc(s, sizeof(char));
		snprintf(path, s, "%s/%s", dir, filename);
	}
	if ((fd = open(path, O_RDONLY)) == -1) {
		LOG(LL_ERROR, "dsp: error: failed to open file: %s: %s\n", path, strerror(errno));
		goto fail;
	}
	if ((file_size = lseek(fd, 0, SEEK_END)) == -1) {
		LOG(LL_ERROR, "dsp: error: failed to determine file size: %s: %s\n", path, strerror(errno));
		goto fail;
	}
	lseek(fd, 0, SEEK_SET);
	contents = malloc(file_size + 1);
	if (read(fd, contents, file_size) != file_size) {
		LOG(LL_ERROR, "dsp: error: short read: %s\n", path);
		goto fail;
	}
	contents[file_size] = '\0';
	if (gen_argv_from_string(contents, &argc, &argv))
		goto fail;
	tmp = strrchr(path, '/');
	if (tmp == NULL)
		dir = ".";
	else {
		*tmp = '\0';
		dir = path;
	}
	if (build_effects_chain(argc, argv, chain, stream, channel_selector, dir))
		goto fail;
	free(path);
	free(contents);
	for (i = 0; i < argc; ++i)
		free(argv[i]);
	free(argv);

	return 0;

	fail:
	free(path);
	free(contents);
	for (i = 0; i < argc; ++i)
		free(argv[i]);
	free(argv);
	if (fd != -1)
		close(fd);
	return 1;
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

int gen_argv_from_string(char *str, int *argc, char ***argv)
{
	int i = 0, k = 0, n = 1, esc = 0, end = 0;

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
				str[k] = '\0';
				*argv = realloc(*argv, (*argc + 1) * sizeof(char *));
				strip_char(&str[i], '\\', 1);
				(*argv)[(*argc)++] = strdup(&str[i]);
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
