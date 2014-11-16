#ifndef _PULSE_H
#define _PULSE_H

#include "dsp.h"
#include "codec.h"

struct codec * pulse_codec_init(const char *, int, const char *, const char *, int, int, int);
void pulse_codec_print_encodings(const char *);

#endif
