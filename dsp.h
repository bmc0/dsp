#ifndef _DSP_H
#define _DSP_H

#include <stdio.h>
#include <sys/types.h>

enum {
	LL_SILENT = 0,
	LL_ERROR,
	LL_OPEN_ERROR,
	LL_NORMAL,
	LL_VERBOSE,
};
#define LOGLEVEL(l) (dsp_globals.loglevel >= (l))
#define LOG_FMT(l, fmt, ...) do { if (LOGLEVEL(l)) fprintf(stderr, "%s: " fmt "\n", dsp_globals.prog_name, __VA_ARGS__); } while (0)
#define LOG_S(l, s) do { if (LOGLEVEL(l)) fprintf(stderr, "%s: %s\n", dsp_globals.prog_name, s); } while (0)

#define DEFAULT_FS            44100
#define DEFAULT_CHANNELS      1
#define DEFAULT_BUF_FRAMES    2048
#define DEFAULT_MAX_BUF_RATIO 32
#define BIT_PERFECT 1

typedef double sample_t;

struct dsp_globals {
	long clip_count;
	sample_t peak;
	int loglevel;
	ssize_t buf_frames;
	ssize_t max_buf_ratio;
	const char *prog_name;
};

struct stream_info {
	int fs, channels;
};

extern struct dsp_globals dsp_globals;

#endif
