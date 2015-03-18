#ifndef _PCM_H
#define _PCM_H

#include "dsp.h"
#include "codec.h"

struct codec * pcm_codec_init(const char *, const char *, const char *, int, int, int, int);
void pcm_codec_print_encodings(const char *);

#endif
