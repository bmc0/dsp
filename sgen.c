#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sgen.h"
#include "util.h"

enum sgen_type {
	SGEN_TYPE_DELTA = 1,
	SGEN_TYPE_SINE,
};

struct sgen_generator {
	int type;
	char *channel_selector;
	ssize_t pos, offset;
	double freq0, freq1, v;
};

struct sgen_state {
	ssize_t w;
	int n;
	struct sgen_generator *g;
};

void sgen_run_generator(struct sgen_generator *g, struct codec *c, sample_t *buf, ssize_t frames)
{
	sample_t s;
	double t;
	ssize_t i, k, samples;
	switch (g->type) {
	case SGEN_TYPE_DELTA:
		if (g->offset >= 0) {
			if (g->offset < frames)
				for (i = 0; i < c->channels; ++i)
					if (GET_BIT(g->channel_selector, i))
						buf[g->offset * c->channels + i] += 1.0;
			g->offset -= frames;
		}
		break;
	case SGEN_TYPE_SINE:
		samples = frames * c->channels;
		for (i = 0; i < samples; i += c->channels) {
			t = (double) g->pos / c->fs;
			if (g->v != 0.0)
				s = sin(g->freq0 / g->v * (exp(t * g->v) - 1.0));
			else
				s = sin(g->freq0 * t);
			for (k = 0; k < c->channels; ++k)
				if (GET_BIT(g->channel_selector, k))
					buf[i + k] += s;
			++g->pos;
		}
		break;
	}
}

ssize_t sgen_read(struct codec *c, sample_t *buf, ssize_t frames)
{
	int i;
	struct sgen_state *state = (struct sgen_state *) c->data;
	if (c->frames > 0 && state->w + frames > c->frames)
		frames = c->frames - state->w;
	if (frames > 0) {
		memset(buf, 0, frames * c->channels * sizeof(sample_t));
		for (i = 0; i < state->n; ++i)
			sgen_run_generator(&state->g[i], c, buf, frames);
		state->w += frames;
	}
	return frames;
}

ssize_t sgen_write(struct codec *c, sample_t *buf, ssize_t frames)
{
	return 0;
}

ssize_t sgen_seek(struct codec *c, ssize_t pos)
{
	return -1;
}

ssize_t sgen_delay(struct codec *c)
{
	return 0;
}

void sgen_drop(struct codec *c)
{
	/* do nothing */
}

void sgen_pause(struct codec *c, int p)
{
	/* do nothing */
}

void sgen_destroy(struct codec *c)
{
	int i;
	struct sgen_state *state = (struct sgen_state *) c->data;
	for (i = 0; i < state->n; ++i)
		free(state->g[i].channel_selector);
	free(state->g);
	free(state);
}

static void sgen_init_generator(struct sgen_generator *g, struct codec *c)
{
	switch (g->type) {
	case SGEN_TYPE_SINE:
		g->freq0 = g->freq1 = 440.0;
		break;
	}
}

#define SGEN_PARSE_FREQ_PARAM(dest, fs, type, key, value, endptr) do { \
	dest = parse_freq(value, &(endptr)); \
	if (check_endptr(type, value, endptr, key)) \
		return -1; \
	if ((dest) <= 0.0 || (dest) >= (double) (fs) / 2.0) { \
		LOG_FMT(LL_ERROR, "%s: error: %s out of range", type, key); \
		return -1; \
	} \
} while(0)

static int sgen_parse_param(struct sgen_generator *g, struct codec *c, const char *type, const char *key, char *value)
{
	char *endptr, *value1;
	switch (g->type) {
	case SGEN_TYPE_DELTA:
		if (strcmp(key, "offset") == 0) {
			g->offset = parse_len(value, c->fs, &endptr);
			if (check_endptr(type, value, endptr, key))
				return -1;
			if (g->offset < 0 || (c->frames > 0 && g->offset >= c->frames)) {
				LOG_FMT(LL_ERROR, "%s: error: %s out of range", type, key);
				return -1;
			}
		}
		else
			return 1;
		break;
	case SGEN_TYPE_SINE:
		if (strcmp(key, "freq") == 0) {
			value1 = isolate(value, '-');
			SGEN_PARSE_FREQ_PARAM(g->freq0, c->fs, type, key, value, endptr);
			g->freq1 = g->freq0;
			if (*value1 != '\0')
				SGEN_PARSE_FREQ_PARAM(g->freq1, c->fs, type, key, value1, endptr);
		}
		else
			return 1;
		break;
	}
	return 0;
}

static void sgen_prepare_generator(struct sgen_generator *g, struct codec *c)
{
	switch (g->type) {
	case SGEN_TYPE_SINE:
		g->freq0 *= 2.0 * M_PI;
		g->freq1 *= 2.0 * M_PI;
		g->v = (c->frames > 0 && g->freq0 != g->freq1) ? log(g->freq1 / g->freq0) / ((double) c->frames / c->fs) : 0.0;
		break;
	}
}

struct codec * sgen_codec_init(const char *path, const char *type, const char *enc, int fs, int channels, int endian, int mode)
{
	char *args = NULL, *arg, *gen_type, *len_str, *next_arg, *next_type, *value, *endptr;
	int parse_ret;
	struct codec *c;
	struct sgen_state *state;
	struct sgen_generator *g;

	c = calloc(1, sizeof(struct codec));
	c->path = path;
	c->type = type;
	c->enc = "sample_t";
	c->fs = fs;
	c->channels = channels;
	c->prec = 53;
	c->frames = -1;
	c->read = sgen_read;
	c->write = sgen_write;
	c->seek = sgen_seek;
	c->delay = sgen_delay;
	c->drop = sgen_drop;
	c->pause = sgen_pause;
	c->destroy = sgen_destroy;

	state = calloc(1, sizeof(struct sgen_state));
	state->n = 0;
	c->data = state;

	args = arg = strdup(path);
	len_str = isolate(arg, '+');
	if (*len_str != '\0') {
		c->frames = parse_len(len_str, fs, &endptr);
		if (check_endptr(type, len_str, endptr, "length"))
			goto fail;
		if (c->frames <= 0) {
			LOG_FMT(LL_ERROR, "%s: error: length cannot be <= 0", type);
			goto fail;
		}
		/* LOG_FMT(LL_VERBOSE, "%s: info: length=%zd", type, c->frames); */
	}
	while (*arg != '\0') {
		next_type = isolate(arg, '/');
		next_arg = isolate(arg, ':');
		value = isolate(arg, '@');
		/* LOG_FMT(LL_VERBOSE, "%s: info: type=%s channel_selector=%s", type, arg, value); */
		state->g = realloc(state->g, (state->n + 1) * sizeof(struct sgen_generator));
		g = &state->g[state->n];
		memset(g, 0, sizeof(struct sgen_generator));
		g->channel_selector = NEW_SELECTOR(channels);
		SET_SELECTOR(g->channel_selector, channels);
		++state->n;

		gen_type = arg;
		if (strcmp(gen_type, "delta") == 0)     g->type = SGEN_TYPE_DELTA;
		else if (strcmp(gen_type, "sine") == 0) g->type = SGEN_TYPE_SINE;
		else {
			LOG_FMT(LL_ERROR, "%s: error: illegal type: %s", type, gen_type);
			goto fail;
		}
		sgen_init_generator(g, c);
		if (*value != '\0' && parse_selector(value, g->channel_selector, channels))
			goto fail;

		arg = next_arg;
		while (*arg != '\0') {
			next_arg = isolate(arg, ':');
			value = isolate(arg, '=');
			/* LOG_FMT(LL_VERBOSE, "%s: %s: arg: key=%s value=%s", type, gen_type, arg, value); */
			parse_ret = sgen_parse_param(g, c, type, arg, value);
			if (parse_ret == 1) {
				LOG_FMT(LL_ERROR, "%s: %s: error: illegal parameter: %s", type, gen_type, arg);
				goto fail;
			}
			else if (parse_ret == -1)
				goto fail;
			arg = next_arg;
		}
		sgen_prepare_generator(g, c);
		arg = next_type;
	}

	done:
	free(args);
	return c;

	fail:
	sgen_destroy(c);
	free(c);
	c = NULL;
	goto done;
}

void sgen_codec_print_encodings(const char *type)
{
	fprintf(stdout, " sample_t");
}
