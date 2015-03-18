#ifndef _MP3_H
#define _MP3_H

#include "dsp.h"
#include "codec.h"

struct codec * mp3_codec_init(const char *, const char *, const char *, int, int, int, int);
void mp3_codec_print_encodings(const char *);

#endif
