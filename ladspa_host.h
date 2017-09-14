#ifndef _LADSPA_HOST_H
#define _LADSPA_HOST_H

#include "dsp.h"
#include "effect.h"

struct effect * ladspa_host_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
