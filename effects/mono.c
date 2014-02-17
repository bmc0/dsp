#include <stdio.h>
#include <stdlib.h>
#include "mono.h"
#include "../util.h"

void mono_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples = *frames * e->ostream.channels, i, k;
	sample_t tmp;
	for (i = 0; i < samples; i += e->ostream.channels) {
		tmp = 0;
		for (k = 0; k < e->ostream.channels; ++k) {
			if (GET_BIT(e->channel_selector, k))
				tmp += ibuf[i + k];
		}
		for (k = 0; k < e->ostream.channels; ++k) {
			if (GET_BIT(e->channel_selector, k))
				obuf[i + k] = tmp;
			else
				obuf[i + k] = obuf[i + k];
		}
	}
}

void mono_effect_reset(struct effect *e)
{
	/* do nothing */
}

void mono_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	*frames = 0;
}

void mono_effect_destroy(struct effect *e)
{
	free(e->channel_selector);
}

struct effect * mono_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, int argc, char **argv)
{
	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->channel_selector = NEW_BIT_ARRAY(istream->channels);
	COPY_BIT_ARRAY(e->channel_selector, channel_selector, istream->channels);
	e->ratio = 1.0;
	e->run = mono_effect_run;
	e->reset = mono_effect_reset;
	e->plot = NULL;
	e->drain = mono_effect_drain;
	e->destroy = mono_effect_destroy;
	return e;
}
