#ifndef _AO_H
#define _AO_H

#include "dsp.h"
#include "codec.h"

struct codec * ao_codec_init(const char *, const char *, const char *, int, int, int, int);
void ao_codec_print_encodings(const char *);

#endif
