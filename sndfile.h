#ifndef _CODEC_SNDFILE_H
#define _CODEC_SNDFILE_H

#include "dsp.h"
#include "codec.h"

struct codec * sndfile_codec_init(const char *, int, const char *, const char *, int, int, int);
void sndfile_codec_print_encodings(const char *);

#endif
