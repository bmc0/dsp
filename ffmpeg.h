#ifndef _FFMPEG_H
#define _FFMPEG_H

#include "dsp.h"
#include "codec.h"

struct codec * ffmpeg_codec_init(const char *, int, const char *, const char *, int, int, int);
void ffmpeg_codec_print_encodings(const char *);

#endif
