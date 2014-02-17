#include <stdio.h>
#include <stdlib.h>
#include "remix.h"
#include "../util.h"

struct remix_state {
	char **channel_selectors;
};

void remix_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, j;
	struct remix_state *state = (struct remix_state *) e->data;
	for (i = 0; i < *frames; ++i) {
		for (k = 0; k < e->ostream.channels; ++k) {
			obuf[i * e->ostream.channels + k] = 0;
			for (j = 0; j < e->istream.channels; ++j) {
				if (GET_BIT(state->channel_selectors[k], j))
					obuf[i * e->ostream.channels + k] += ibuf[i * e->istream.channels + j];
			}
		}
	}
}

void remix_effect_reset(struct effect *e)
{
	/* do nothing */
}

void remix_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	*frames = 0;
}

void remix_effect_destroy(struct effect *e)
{
	int i;
	struct remix_state *state = (struct remix_state *) e->data;
	for (i = 0; i < e->ostream.channels; ++i)
		free(state->channel_selectors[i]);
	free(state->channel_selectors);
	free(state);
}

struct effect * remix_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, int argc, char **argv)
{
	struct effect *e;
	struct remix_state *state;
	int i, out_channels;

	if (argc <= 1) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}

	out_channels = argc - 1;
	state = calloc(1, sizeof(struct remix_state));
	state->channel_selectors = calloc(out_channels, sizeof(char *));
	for (i = 0; i < out_channels; ++i) {
		state->channel_selectors[i] = NEW_BIT_ARRAY(istream->channels);
		if (strcmp(argv[i + 1], ".") != 0 && parse_selector(argv[i + 1], state->channel_selectors[i], istream->channels))
			goto fail;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = istream->channels;
	e->ostream.channels = out_channels;
	e->ratio = (double) e->ostream.channels / e->istream.channels;
	e->run = remix_effect_run;
	e->reset = remix_effect_reset;
	e->drain = remix_effect_drain;
	e->destroy = remix_effect_destroy;
	e->data = state;
	return e;

	fail:
	for (i = 0; i < out_channels; ++i)
		if (state->channel_selectors)
			free(state->channel_selectors[i]);
	free(state->channel_selectors);
	free(state);
	return NULL;
}
