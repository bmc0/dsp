#ifndef _DSP_ZITA_CONVOLVER_H
#define _DSP_ZITA_CONVOLVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dsp.h"
#include "effect.h"

struct effect * zita_convolver_effect_init(const struct effect_info *, const struct stream_info *, const char *, const char *, int, const char *const *);

#ifdef __cplusplus
}
#endif

#endif
