#ifndef DSP_FFMPEG_H
#define DSP_FFMPEG_H

#include "dsp.h"
#include "codec.h"

struct codec * ffmpeg_codec_init(const char *, const char *, const char *, int, int, int, int);
void ffmpeg_codec_print_encodings(const char *);

#endif
