#ifndef _DSP_H
#define _DSP_H

#include <sys/types.h>

enum {
	LL_ERROR = 1,
	LL_NORMAL,
	LL_VERBOSE,
};
#define LOG(l, ...) do { if (dsp_globals.loglevel >= l) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOGLEVEL(l) dsp_globals.loglevel >= l
#define SELECT_FS(x) ((x == -1) ? (dsp_globals.fs == -1) ? DEFAULT_FS : dsp_globals.fs : x)
#define SELECT_CHANNELS(x) ((x == -1) ? (dsp_globals.channels == -1) ? DEFAULT_CHANNELS : dsp_globals.channels : x)

#define DEFAULT_FS           44100
#define DEFAULT_CHANNELS     1
#define DEFAULT_BUF_FRAMES   2048
#define DEFAULT_OUTPUT_TYPE  "alsa"
#define DEFAULT_OUTPUT_PATH  "default"

#define BIT_PERFECT 1

typedef double sample_t;

struct dsp_globals {
	int fs, channels;
	long clip_count;
	int loglevel;
	size_t buf_frames;
};

extern struct dsp_globals dsp_globals;

#endif
