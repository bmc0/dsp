#ifndef _DECORRELATE_H
#define _DECORRELATE_H

#include "dsp.h"
#include "effect.h"

struct effect * decorrelate_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
