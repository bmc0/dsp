#ifndef _HILBERT_H
#define _HILBERT_H

#include "dsp.h"
#include "effect.h"

struct effect * hilbert_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
