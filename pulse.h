#ifndef DSP_PULSE_H
#define DSP_PULSE_H

#include "dsp.h"
#include "codec.h"

struct codec * pulse_codec_init(const char *, const char *, const char *, int, int, int, int);
void pulse_codec_print_encodings(const char *);

#endif
