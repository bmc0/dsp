#ifndef _CODECS_PCM_H
#define _CODECS_PCM_H

#include "../dsp.h"
#include "../codec.h"

struct codec * pcm_codec_init(const char *, int, const char *, const char *, int, int, int);
void pcm_codec_print_encodings(const char *);

#endif
