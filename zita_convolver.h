#ifndef _DSP_ZITA_CONVOLVER_H
#define _DSP_ZITA_CONVOLVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dsp.h"
#include "effect.h"

struct effect * zita_convolver_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#ifdef __cplusplus
}
#endif

#endif
