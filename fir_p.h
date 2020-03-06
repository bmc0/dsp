#ifndef _FIR_P_H
#define _FIR_P_H

#include "dsp.h"
#include "effect.h"

struct effect * fir_p_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#endif
