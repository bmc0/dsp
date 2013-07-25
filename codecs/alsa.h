#ifndef _CODECS_ALSA_H
#define _CODECS_ALSA_H

#include "../dsp.h"
#include "../codec.h"

struct codec * alsa_codec_init(const char *, int, const char *, const char *, int, int, int);
void alsa_codec_print_encodings(const char *);

#endif
