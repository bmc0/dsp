#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <sys/termios.h>
#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "dither.h"
#include "util.h"

#define SHOULD_DITHER(in, out, has_effects) (force_dither != -1 && (force_dither == 1 || (out->prec < 24 && (has_effects || in->prec > out->prec))))
#define TIME_FMT "%.2zu:%.2zu:%05.2lf"
#define TIME_FMT_ARGS(frames, fs) frames / fs / 3600, (frames / fs / 60) % 60, fmod((double) frames / fs, 60.0)

static struct termios term_attrs;
static sample_t buf[BUF_SAMPLES];
static int interactive = -1, show_progress = 1, plot = 0, term_attrs_saved = 0, force_dither = 0;
static struct effects_chain chain = { NULL, NULL };
static struct codec_list in_codecs = { NULL, NULL };
static struct codec *out_codec = NULL;

static const char usage[] =
	"usage: dsp [[options] path ...] [[effect] [args ...] ...]\n"
	"\n"
	"global options:\n"
	"  -h  show this help\n"
	"  -I  disable interactive mode\n"
	"  -q  disable progress display\n"
	"  -s  silent mode\n"
	"  -v  verbose mode\n"
	"  -d  force dithering\n"
	"  -D  disable dithering\n"
	"  -p  plot effects chain instead of processing audio\n"
	"\n"
	"input/output options:\n"
	"  -o               output\n"
	"  -t type          type\n"
	"  -e encoding      encoding\n"
	"  -B/L/N           big/little/native endian\n"
	"  -r frequency[k]  sample rate\n"
	"  -c channels      number of channels\n"
	"  -n               equivalent to '-t null null'\n"
	"\n"
	"default output:\n"
	"  type: "DEFAULT_OUTPUT_TYPE"\n"
	"  path: "DEFAULT_OUTPUT_PATH"\n";

static const char interactive_help[] =
	"Keys:\n"
	"  h : display this help\n"
	"  , : seek backward 5 sec\n"
	"  . : seek forward 5 sec\n"
	"  < : seek backward 30 sec\n"
	"  > : seek forward 30 sec\n"
	"  r : restart current input\n"
	"  n : skip current input\n"
	"  c : pause\n"
	"  q : quit\n";

struct dsp_globals dsp_globals = {
	-1,         /* fs: set by first input; DEFAULT_FS if no input */
	-1,         /* channels: set by first input; DEFAULT_CHANNELS if no input */
	0,          /* clip_count */
	LL_NORMAL,  /* loglevel */
};

static sample_t clip(sample_t s)
{
	if (s > 1.0) {
		++dsp_globals.clip_count;
		return 1.0;
	}
	else if (s < -1.0) {
		++dsp_globals.clip_count;
		return -1.0;
	}
	else
		return s;
}

static void cleanup_and_exit(int s)
{
	destroy_codec_list(&in_codecs);
	if (out_codec != NULL)
		destroy_codec(out_codec);
	destroy_effects_chain(&chain);
	if (term_attrs_saved)
		tcsetattr(0, TCSANOW, &term_attrs);
	if (dsp_globals.clip_count > 0)
		LOG(LL_NORMAL, "dsp: warning: clipped %ld samples\n", dsp_globals.clip_count);
	exit(s);
}

static void setup_term(void)
{
	struct termios n;
	if (term_attrs_saved == 0) {
		tcgetattr(0, &term_attrs);
		term_attrs_saved = 1;
		n = term_attrs;
		n.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(0, TCSANOW, &n);
	}
}

static int input_pending(void)
{
	struct timeval t = { 0, 0 };
	fd_set f;
	FD_ZERO(&f);
	FD_SET(STDIN_FILENO, &f);
	select(1, &f, NULL, NULL, &t);
	return FD_ISSET(STDIN_FILENO, &f);
}

static void print_usage(void)
{
	fprintf(stderr, "%s\n", usage);
	print_all_codecs();
	fputc('\n', stderr);
	print_all_effects();
}

static int parse_io_params(int argc, char *argv[], int *mode, char **path, char **type, char **enc, int *endian, int *rate, int *channels)
{
	int opt;
	*path = *type = NULL;
	*enc = NULL;
	*endian = CODEC_ENDIAN_DEFAULT;
	*channels = *rate = -1;
	*mode = CODEC_MODE_READ;

	while ((opt = getopt(argc, argv, "+:hIqsvdDpot:e:BLNr:c:n")) != -1) {
		switch (opt) {
			case 'h':
				print_usage();
				cleanup_and_exit(0);
			case 'I':
				interactive = 0;
				break;
			case 'q':
				show_progress = 0;
				break;
			case 's':
				dsp_globals.loglevel = 0;
				break;
			case 'v':
				dsp_globals.loglevel = LL_VERBOSE;
				break;
			case 'd':
				force_dither = 1;
				break;
			case 'D':
				force_dither = -1;
				break;
			case 'p':
				plot = 1;
				break;
			case 'o':
				*mode = CODEC_MODE_WRITE;
				break;
			case 't':
				*type = optarg;
				break;
			case 'e':
				*enc = optarg;
				break;
			case 'B':
				*endian = CODEC_ENDIAN_BIG;
				break;
			case 'L':
				*endian = CODEC_ENDIAN_LITTLE;
				break;
			case 'N':
				*endian = CODEC_ENDIAN_NATIVE;
				break;
			case 'r':
				*rate = parse_freq(optarg);
				if (*rate <= 0) {
					LOG(LL_ERROR, "dsp: error: rate must be > 0\n");
					return 1;
				}
				break;
			case 'c':
				*channels = atoi(optarg);
				if (*channels <= 0) {
					LOG(LL_ERROR, "dsp: error: number of channels must be > 0\n");
					return 1;
				}
				break;
			case 'n':
				*path = *type = "null";
				return 0;
			default:
				if (opt == ':')
					LOG(LL_ERROR, "dsp: error: expected argument to option '%c'\n", optopt);
				else
					LOG(LL_ERROR, "dsp: error: illegal option '%c'\n", optopt);
				return 1;
		}
	}
	if (optind < argc)
		*path = argv[optind++];
	else {
		LOG(LL_ERROR, "dsp: error: expected path\n");
		return 1;
	}
	return 0;
}

static void print_io_info(struct codec *c, const char *n)
{
	fprintf(stderr, "dsp: %s: %s; type=%s enc=%s precision=%d channels=%d fs=%d frames=%zu ["TIME_FMT"]\n",
		n, c->path, c->type, c->enc, c->prec, c->channels, c->fs, c->frames, TIME_FMT_ARGS(c->frames, c->fs));
}

static struct codec * init_io(int mode, const char *path, const char *type, const char *enc, int endian, int rate, int channels, size_t frames)
{
	struct codec *c = NULL;

	c = init_codec(type, mode, path, enc, endian, rate, channels);
	if (c == NULL) {
		LOG(LL_ERROR, "dsp: error: failed to open %s: %s\n", (mode == CODEC_MODE_WRITE) ? "output" : "input", path);
		return NULL;
	}
	if (mode == CODEC_MODE_WRITE)
		c->frames = frames;
	if ((LOGLEVEL(LL_NORMAL) && mode == CODEC_MODE_WRITE) || LOGLEVEL(LL_VERBOSE))
		print_io_info(c, (mode == CODEC_MODE_WRITE) ? "output" : "input");
	if (dsp_globals.fs == -1)
		dsp_globals.fs = c->fs;
	else if (c->fs != dsp_globals.fs) {
		LOG(LL_ERROR, "dsp: error: all inputs and outputs must have the same sample rate\n");
		return NULL;
	}
	if (dsp_globals.channels == -1)
		dsp_globals.channels = c->channels;
	else if (c->channels != dsp_globals.channels) {
		LOG(LL_ERROR, "dsp: error: all inputs and outputs must have the same number of channels\n");
		return NULL;
	}
	if (interactive == -1 && c->interactive)
		interactive = 1;
	return c;
}

static void terminate(int s)
{
	LOG(LL_NORMAL, "\ndsp: info: signal %d: terminating...\n", s);
	if (out_codec != NULL)
		out_codec->reset(out_codec);  /* reset so we don't sit around waiting for alsa to drain */
	cleanup_and_exit(0);
}

static void print_progress(struct codec *in, struct codec *out, ssize_t pos, int pause, sample_t peak)
{
	ssize_t delay = out->delay(out);
	ssize_t p = (pos > delay) ? pos - delay : 0;
	ssize_t rem = (in->frames > p) ? in->frames - p : 0;
	fprintf(stderr, "\033[1K\r%c  %.1f%%  "TIME_FMT"  -"TIME_FMT"  lat:%.2fms  peak:%.2fdBFS  clip:%ld  ",
		(pause) ? '|' : '>', (in->frames > 0) ? (double) p / in->frames * 100.0 : 0,
		TIME_FMT_ARGS(p, dsp_globals.fs), TIME_FMT_ARGS(rem, dsp_globals.fs),
		(double) delay / dsp_globals.fs * 1000, log10(peak) * 20, dsp_globals.clip_count);
}

int main(int argc, char *argv[])
{
	int i, k, j, effect_start = 1, pause = 0, do_dither = 0;
	ssize_t r, delay, out_frames = 0, seek, pos = 0;
	sample_t peak = 0;
	char **args = NULL;
	struct effect_info *ei = NULL;
	struct effect *e = NULL;
	struct codec *c = NULL;

	struct {
		char *path, *type, *enc;
		int endian, rate, channels, mode;
	} params, out_params = { NULL, NULL, NULL, 0, -1, -1, 0 };

	signal(SIGINT, terminate);
	signal(SIGTERM, terminate);

	opterr = 0;
	if (!isatty(STDIN_FILENO))
		interactive = 0;
	while (optind < argc && get_effect_info(argv[optind]) == NULL) {
		if (parse_io_params(argc, argv, &params.mode, &params.path, &params.type, &params.enc, &params.endian, &params.rate, &params.channels))
			cleanup_and_exit(1);
		if (params.mode == CODEC_MODE_WRITE)
			out_params = params;
		else {
			if ((c = init_io(params.mode, params.path, params.type, params.enc, params.endian, params.rate, params.channels, 0)) == NULL)
				cleanup_and_exit(1);
			out_frames += c->frames;
			append_codec(&in_codecs, c);
		}
	}

	if (dsp_globals.fs == -1)
		dsp_globals.fs = DEFAULT_FS;  /* set default if not set */
	if (dsp_globals.channels == -1)
		dsp_globals.channels = DEFAULT_CHANNELS;  /* set default if not set */
	if (dsp_globals.loglevel == 0)	
		show_progress = 0;  /* disable progress display if in silent mode */
	if (in_codecs.head == NULL) {
		LOG(LL_ERROR, "dsp: error: no inputs\n");
		cleanup_and_exit(1);
	}

	if (!plot) {
		if (out_params.path == NULL)
			out_codec = init_io(CODEC_MODE_WRITE, DEFAULT_OUTPUT_PATH, DEFAULT_OUTPUT_TYPE, NULL, CODEC_ENDIAN_DEFAULT, dsp_globals.fs, dsp_globals.channels, out_frames);
		else
			out_codec = init_io(out_params.mode, out_params.path, out_params.type, out_params.enc, out_params.endian, out_params.rate, out_params.channels, out_frames);
		if (out_codec == NULL)
			cleanup_and_exit(1);
	}
	if (interactive == -1)
		interactive = 0;  /* disable if not set */

	k = effect_start = optind;
	i = k + 1;
	while (k < argc) {
		ei = get_effect_info(argv[k]);
		if (ei == NULL) {
			if (k == effect_start) {
				LOG(LL_ERROR, "dsp: error: no such effect: %s\n", argv[k]);
				cleanup_and_exit(1);
			}
			else break;
		}
		while (i < argc && get_effect_info(argv[i]) == NULL)
			++i;
		args = calloc(i - k, sizeof(char *));
		LOG(LL_VERBOSE, "dsp: effect:");
		for (j = 0; j < i - k; ++j) {
			LOG(LL_VERBOSE, " %s", argv[k + j]);
			args[j] = strdup(argv[k + j]);
		}
		LOG(LL_VERBOSE, "\n");
		e = init_effect(ei, i - k, args);
		for (j = 0; j < i - k; ++j)
			free(args[j]);
		free(args);
		if (e == NULL) {
			LOG(LL_ERROR, "dsp: error: failed to initialize effect: %s\n", argv[k]);
			cleanup_and_exit(1);
		}
		append_effect(&chain, e);
		k = i;
		i = k + 1;
	}

	if (plot)
		plot_effects_chain(&chain);
	else {
		if (interactive) {
			setup_term();
			LOG(LL_NORMAL, "dsp: info: running interactively; type 'h' for help\n");
		}
		while (in_codecs.head != NULL) {
			k = 0;
			do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
			LOG(LL_VERBOSE, "dsp: info: dither %s\n", (do_dither) ? "on" : "off" );
			if (show_progress) {
				print_io_info(in_codecs.head, "input");
				print_progress(in_codecs.head, out_codec, pos, pause, peak);
			}
			do {
				while (interactive && (input_pending() || pause)) {
					delay = out_codec->delay(out_codec);
					switch (getchar()) {
						case 'h':
							if (show_progress)
								fputc('\n', stderr);
							fprintf(stderr, "\n%s\n", interactive_help);
							break;
						case ',':
							seek = in_codecs.head->seek(in_codecs.head, pos - dsp_globals.fs * 5 - delay);
							if (seek >= 0) {
								pos = seek;
								out_codec->reset(out_codec);
							}
							break;
						case '.':
							seek = in_codecs.head->seek(in_codecs.head, pos + dsp_globals.fs * 5 - delay);
							if (seek >= 0) {
								pos = seek;
								out_codec->reset(out_codec);
							}
							break;
						case '<':
							seek = in_codecs.head->seek(in_codecs.head, pos - dsp_globals.fs * 30 - delay);
							if (seek >= 0) {
								pos = seek;
								out_codec->reset(out_codec);
							}
							break;
						case '>':
							seek = in_codecs.head->seek(in_codecs.head, pos + dsp_globals.fs * 30 - delay);
							if (seek >= 0) {
								pos = seek;
								out_codec->reset(out_codec);
							}
							break;
						case 'r':
							seek = in_codecs.head->seek(in_codecs.head, 0);
							if (seek >= 0) {
								pos = seek;
								out_codec->reset(out_codec);
							}
							break;
						case 'n':
							out_codec->reset(out_codec);
							goto next_input;
						case 'c':
							out_codec->pause(out_codec, pause = (pause) ? 0 : 1);
							break;
						case 'q':
							out_codec->reset(out_codec);
							if (show_progress)
								fputc('\n', stderr);
							goto end_rw_loop;
					}
					if (show_progress)
						print_progress(in_codecs.head, out_codec, pos, pause, peak);
				}
				pos += r = in_codecs.head->read(in_codecs.head, buf, BUF_SAMPLES / dsp_globals.channels);
				k += r;
				for (i = 0; i < r * dsp_globals.channels; i += dsp_globals.channels) {
					run_effects_chain(&chain, &buf[i]);
					for (j = 0; j < dsp_globals.channels; ++j) {
						if (do_dither)
							buf[i + j] = tpdf_dither_sample(buf[i + j], out_codec->prec);
						if (fabs(buf[i + j]) > peak)
							peak = fabs(buf[i + j]);
						buf[i + j] = clip(buf[i + j]);
					}
				}
				if (r != 0 && out_codec->write(out_codec, buf, r) != r) {
					LOG(LL_ERROR, "dsp: error: short write\n");
					cleanup_and_exit(1);
				}
				if (show_progress && k >= dsp_globals.fs) {
					print_progress(in_codecs.head, out_codec, pos, pause, peak);
					k = 0;
				}
			} while (r > 0);
			next_input:
			pos = 0;
			destroy_codec_list_head(&in_codecs);
			if (show_progress)
				fputc('\n', stderr);
		}
	}
	end_rw_loop:
	cleanup_and_exit(0);
	return 0;
}
