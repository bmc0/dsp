#ifndef _NULL_H
#define _NULL_H

#include "dsp.h"
#include "codec.h"

struct codec * null_codec_init(const char *, int, const char *, const char *, int, int, int);
void null_codec_print_encodings(const char *);

#endif
