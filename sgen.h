#ifndef _SGEN_H
#define _SGEN_H

#include "dsp.h"
#include "codec.h"

struct codec * sgen_codec_init(const char *, const char *, const char *, int, int, int, int);
void sgen_codec_print_encodings(const char *);

#endif
