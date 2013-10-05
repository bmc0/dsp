#include <stdio.h>
#include <stdlib.h>
#include "mono.h"

void mono_effect_run(struct effect *e, sample_t *s)
{
	int i;
	sample_t tmp = 0;
	for (i = 0; i < dsp_globals.channels; ++i)
		tmp += s[i];
	for (i = 0; i < dsp_globals.channels; ++i)
		s[i] = tmp;
}

void mono_effect_plot(struct effect *e, int i)
{
	printf("H%d(f)=0\n", i);
}

void mono_effect_destroy(struct effect *e)
{
	/* nothing to free */
}

struct effect * mono_effect_init(struct effect_info *ei, int argc, char **argv)
{
	struct effect *e = calloc(1, sizeof(struct effect));
	e->run = mono_effect_run;
	e->plot = mono_effect_plot;
	e->destroy = mono_effect_destroy;
	return e;
}
