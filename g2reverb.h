#ifndef _G2REVERB_H
#define _G2REVERB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dsp.h"
#include "effect.h"

struct effect * g2reverb_effect_init(struct effect_info *, struct stream_info *, char *, const char *, int, char **);

#ifdef __cplusplus
}
#endif

#endif
