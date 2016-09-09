#ifndef _FIR_H
#define _FIR_H

#include "dsp.h"
#include "effect.h"

struct effect * fir_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
