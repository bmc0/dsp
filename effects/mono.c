#include <stdio.h>
#include <stdlib.h>
#include "mono.h"

void mono_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t samples = *frames * e->ostream.channels, i, k;
	sample_t tmp;
	for (i = 0; i < samples; i += e->ostream.channels) {
		tmp = 0;
		for (k = 0; k < e->ostream.channels; ++k)
			tmp += ibuf[i + k];
		for (k = 0; k < e->ostream.channels; ++k)
			obuf[i + k] = tmp;
	}
}

void mono_effect_reset(struct effect *e)
{
	/* do nothing */
}

void mono_effect_plot(struct effect *e, int i)
{
	printf("H%d(f)=0\n", i);
}

void mono_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	*frames = 0;
}

void mono_effect_destroy(struct effect *e)
{
	/* nothing to free */
}

struct effect * mono_effect_init(struct effect_info *ei, struct stream_info *istream, int argc, char **argv)
{
	struct effect *e = calloc(1, sizeof(struct effect));
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->ratio = 1.0;
	e->run = mono_effect_run;
	e->reset = mono_effect_reset;
	e->plot = mono_effect_plot;
	e->drain = mono_effect_drain;
	e->destroy = mono_effect_destroy;
	return e;
}
