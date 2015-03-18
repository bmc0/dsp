#ifndef _SNDFILE_H
#define _SNDFILE_H

#include "dsp.h"
#include "codec.h"

struct codec * sndfile_codec_init(const char *, const char *, const char *, int, int, int, int);
void sndfile_codec_print_encodings(const char *);

#endif
