#ifndef _REMIX_H
#define _REMIX_H

#include "dsp.h"
#include "effect.h"

struct effect * remix_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
