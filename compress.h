#ifndef _COMPRESS_H
#define _COMPRESS_H

#include "dsp.h"
#include "effect.h"

struct effect * compress_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
