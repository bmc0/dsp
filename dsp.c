#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <termios.h>
#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "util.h"

#define SELECT_FS(x) ((x == -1) ? (input_fs == -1) ? DEFAULT_FS : input_fs : x)
#define SELECT_CHANNELS(x) ((x == -1) ? (input_channels == -1) ? DEFAULT_CHANNELS : input_channels : x)
#define SHOULD_DITHER(in, out, has_effects) (force_dither != -1 && (force_dither == 1 || (out->prec < 24 && (has_effects || in->prec > out->prec))))
#define TIME_FMT "%.2zd:%.2zd:%05.2lf"
#define TIME_FMT_ARGS(frames, fs) (frames != -1) ? frames / fs / 3600 : 0, (frames != -1) ? (frames / fs / 60) % 60 : 0, (frames != -1) ? fmod((double) frames / fs, 60.0) : 0

static struct termios term_attrs;
static int input_fs = -1, input_channels = -1, interactive = -1, show_progress = 1, plot = 0, term_attrs_saved = 0, force_dither = 0, verbose_progress = 0;
static struct effects_chain chain = { NULL, NULL };
static struct codec_list in_codecs = { NULL, NULL };
static struct codec *out_codec = NULL;
static sample_t *buf1 = NULL, *buf2 = NULL, *obuf;

static const char usage[] =
	"Usage: dsp [options] path ... [:channel_selector] [@[~/]effects_file] [effect [args ...]] ...\n"
	"\n"
	"Global options:\n"
	"  -h         show this help\n"
	"  -b frames  set buffer size (must be specified before the first input)\n"
	"  -R ratio   set codec maximum buffer ratio (must be specified before the first input)\n"
	"  -I         disable interactive mode\n"
	"  -q         disable progress display\n"
	"  -s         silent mode\n"
	"  -v         verbose mode\n"
	"  -d         force dithering\n"
	"  -D         disable dithering\n"
	"  -p         plot effects chain instead of processing audio\n"
	"  -V         enable verbose progress display\n"
	"\n"
	"Input/output options:\n"
	"  -o               output\n"
	"  -t type          type\n"
	"  -e encoding      encoding\n"
	"  -B/L/N           big/little/native endian\n"
	"  -r frequency[k]  sample rate\n"
	"  -c channels      number of channels\n"
	"  -n               equivalent to '-t null null'\n";

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
	"  e : rebuild effects chain\n"
	"  v : toggle verbose progress display\n"
	"  q : quit\n";

struct dsp_globals dsp_globals = {
	0,                      /* clip_count */
	0,                      /* peak */
	LL_NORMAL,              /* loglevel */
	DEFAULT_BUF_FRAMES,     /* buf_frames */
	DEFAULT_MAX_BUF_RATIO,  /* max_buf_ratio */
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
	free(buf1);
	free(buf2);
	if (term_attrs_saved)
		tcsetattr(0, TCSANOW, &term_attrs);
	if (dsp_globals.clip_count > 0)
		LOG(LL_NORMAL, "dsp: warning: clipped %ld samples (%.2fdBFS peak)\n", dsp_globals.clip_count, log10(dsp_globals.peak) * 20);
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
	fprintf(stdout, "%s\n", usage);
	print_all_codecs();
	fputc('\n', stdout);
	print_all_effects();
}

static int parse_codec_params(int argc, char *argv[], int *mode, char **path, char **type, char **enc, int *endian, int *fs, int *channels)
{
	int opt;
	*path = *type = NULL;
	*enc = NULL;
	*endian = CODEC_ENDIAN_DEFAULT;
	*channels = *fs = -1;
	*mode = CODEC_MODE_READ;

	while ((opt = getopt(argc, argv, "+:hb:R:IqsvdDpVot:e:BLNr:c:n")) != -1) {
		switch (opt) {
		case 'h':
			print_usage();
			cleanup_and_exit(0);
		case 'b':
			if (in_codecs.head == NULL) {
				dsp_globals.buf_frames = atoi(optarg);
				if (dsp_globals.buf_frames <= 0) {
					LOG(LL_ERROR, "dsp: error: buffer size must be > 0\n");
					return 1;
				}
			}
			else
				LOG(LL_ERROR, "dsp: warning: buffer size must be specified before the first input\n");
			break;
		case 'R':
			if (in_codecs.head == NULL) {
				dsp_globals.max_buf_ratio = atoi(optarg);
				if (dsp_globals.max_buf_ratio <= 0) {
					LOG(LL_ERROR, "dsp: error: buffer ratio must be > 0\n");
					return 1;
				}
			}
			else
				LOG(LL_ERROR, "dsp: warning: buffer ratio must be specified before the first input\n");
			break;
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
		case 'V':
			verbose_progress = 1;
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
			*fs = parse_freq(optarg);
			if (*fs <= 0) {
				LOG(LL_ERROR, "dsp: error: sample rate must be > 0\n");
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
	fprintf(stderr, "dsp: %s: %s; type=%s enc=%s precision=%d channels=%d fs=%d frames=%zd ["TIME_FMT"]\n",
		n, c->path, c->type, c->enc, c->prec, c->channels, c->fs, c->frames, TIME_FMT_ARGS(c->frames, c->fs));
}

static void terminate(int s)
{
	LOG(LL_NORMAL, "\ndsp: info: signal %d: terminating...\n", s);
	if (out_codec != NULL)
		out_codec->reset(out_codec);  /* reset so we don't sit around waiting for alsa to drain */
	cleanup_and_exit(0);
}

static void print_progress(struct codec *in, struct codec *out, ssize_t pos, int pause)
{
	ssize_t delay = lround((double) out->delay(out) / out->fs * in->fs);
	ssize_t p = (pos > delay) ? pos - delay : 0;
	ssize_t rem = (in->frames > p) ? in->frames - p : 0;
	fprintf(stderr, "\r%c  %.1f%%  "TIME_FMT"  -"TIME_FMT"  ",
		(pause) ? '|' : '>', (in->frames != -1) ? (double) p / in->frames * 100.0 : 0,
		TIME_FMT_ARGS(p, in->fs), TIME_FMT_ARGS(rem, in->fs));
	if (verbose_progress)
		fprintf(stderr, "lat:%.2fms+%.2fms  ", (double) in->delay(in) / in->fs * 1000, (double) out->delay(out) / out->fs * 1000);
	if (verbose_progress || dsp_globals.clip_count != 0)
		fprintf(stderr, "peak:%.2fdBFS  clip:%ld  ", log10(dsp_globals.peak) * 20, dsp_globals.clip_count);
	fprintf(stderr, "\033[K");
}

static void write_to_output(ssize_t frames, sample_t *buf, int do_dither)
{
	ssize_t i;
	for (i = 0; i < frames * out_codec->channels; ++i) {
		if (do_dither)
			buf[i] = tpdf_dither_sample(buf[i], out_codec->prec);
		if (fabs(buf[i]) > dsp_globals.peak)
			dsp_globals.peak = fabs(buf[i]);
		buf[i] = clip(buf[i]);
	}
	if (frames != 0 && out_codec->write(out_codec, buf, frames) != frames) {
		LOG(LL_ERROR, "dsp: error: short write\n");
		cleanup_and_exit(1);
	}
}

int main(int argc, char *argv[])
{
	int k, pause = 0, do_dither = 0, effect_start, effect_argc;
	ssize_t r, w, delay, in_frames = 0, seek, pos = 0;
	struct codec *c = NULL;
	struct stream_info stream;

	struct {
		char *path, *type, *enc;
		int endian, fs, channels, mode;
	} params, out_params = { NULL, NULL, NULL, 0, -1, -1, CODEC_MODE_WRITE };

	signal(SIGINT, terminate);
	signal(SIGTERM, terminate);

	opterr = 0;
	if (!isatty(STDIN_FILENO))
		interactive = 0;
	while (optind < argc && get_effect_info(argv[optind]) == NULL && argv[optind][0] != ':' && argv[optind][0] != '@') {
		if (parse_codec_params(argc, argv, &params.mode, &params.path, &params.type, &params.enc, &params.endian, &params.fs, &params.channels))
			cleanup_and_exit(1);
		if (params.mode == CODEC_MODE_WRITE)
			out_params = params;
		else {
			c = init_codec(params.type, CODEC_MODE_READ, params.path, params.enc, params.endian, SELECT_FS(params.fs), SELECT_CHANNELS(params.channels));
			if (c == NULL) {
				LOG(LL_ERROR, "dsp: error: failed to open input: %s\n", params.path);
				cleanup_and_exit(1);
			}
			if (LOGLEVEL(LL_VERBOSE))
				print_io_info(c, "input");
			if (input_fs == -1)
				input_fs = c->fs;
			else if (c->fs != input_fs) {
				LOG(LL_ERROR, "dsp: error: all inputs must have the same sample rate\n");
				cleanup_and_exit(1);
			}
			if (input_channels == -1)
				input_channels = c->channels;
			else if (c->channels != input_channels) {
				LOG(LL_ERROR, "dsp: error: all inputs must have the same number of channels\n");
				cleanup_and_exit(1);
			}
			if (c->frames == -1 || in_frames == -1)
				in_frames = -1;
			else
				in_frames += c->frames;
			append_codec(&in_codecs, c);
		}
	}

	if (dsp_globals.loglevel == 0)	
		show_progress = 0;  /* disable progress display if in silent mode */
	if (in_codecs.head == NULL) {
		LOG(LL_ERROR, "dsp: error: no inputs\n");
		cleanup_and_exit(1);
	}

	effect_start = optind;
	effect_argc = argc - optind;
	stream.fs = input_fs;
	stream.channels = input_channels;
	if (build_effects_chain(effect_argc, &argv[effect_start], &chain, &stream, NULL, NULL))
		cleanup_and_exit(1);

	if (plot)
		plot_effects_chain(&chain, input_fs);
	else {
		out_codec = init_codec(out_params.type, out_params.mode, (out_params.path == NULL) ? "default" : out_params.path, out_params.enc, out_params.endian, (out_params.fs == -1) ? stream.fs : out_params.fs, (out_params.channels == -1) ? stream.channels : out_params.channels);
		if (out_codec == NULL) {
			LOG(LL_ERROR, "dsp: error: failed to open output\n");
			cleanup_and_exit(1);
		}
		if (out_codec->fs != stream.fs) {
			LOG(LL_ERROR, "dsp: error: sample rate mismatch: %s\n", out_codec->path);
			cleanup_and_exit(1);
		}
		if (out_codec->channels != stream.channels) {
			LOG(LL_ERROR, "dsp: error: channels mismatch: %s\n", out_codec->path);
			cleanup_and_exit(1);
		}
		if (in_frames == -1)
			out_codec->frames = -1;
		else
			out_codec->frames = in_frames * (get_effects_chain_total_ratio(&chain) * input_channels / stream.channels);
		if (LOGLEVEL(LL_NORMAL))
			print_io_info(out_codec, "output");

		if (interactive == -1 && out_codec->interactive)
			interactive = 1;
		else
			interactive = 0;

		buf1 = calloc(ceil(dsp_globals.buf_frames * input_channels * get_effects_chain_max_ratio(&chain)), sizeof(sample_t));
		buf2 = calloc(ceil(dsp_globals.buf_frames * input_channels * get_effects_chain_max_ratio(&chain)), sizeof(sample_t));

		if (interactive) {
			setup_term();
			LOG(LL_NORMAL, "dsp: info: running interactively; type 'h' for help\n");
		}
		while (in_codecs.head != NULL) {
			k = 0;
			do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
			LOG(LL_VERBOSE, "dsp: info: dither %s\n", (do_dither) ? "on" : "off" );
			if (LOGLEVEL(LL_NORMAL))
				print_io_info(in_codecs.head, "input");
			if (show_progress)
				print_progress(in_codecs.head, out_codec, pos, pause);
			do {
				while (interactive && (input_pending() || pause)) {
					delay = lround((double) out_codec->delay(out_codec) / out_codec->fs * in_codecs.head->fs);
					switch (getchar()) {
					case 'h':
						if (show_progress)
							fputs("\033[1K\r", stderr);
						fprintf(stderr, "\n%s\n", interactive_help);
						break;
					case ',':
						seek = in_codecs.head->seek(in_codecs.head, pos - input_fs * 5 - delay);
						if (seek >= 0) {
							pos = seek;
							out_codec->reset(out_codec);
							reset_effects_chain(&chain);
						}
						break;
					case '.':
						seek = in_codecs.head->seek(in_codecs.head, pos + input_fs * 5 - delay);
						if (seek >= 0) {
							pos = seek;
							out_codec->reset(out_codec);
							reset_effects_chain(&chain);
						}
						break;
					case '<':
						seek = in_codecs.head->seek(in_codecs.head, pos - input_fs * 30 - delay);
						if (seek >= 0) {
							pos = seek;
							out_codec->reset(out_codec);
							reset_effects_chain(&chain);
						}
						break;
					case '>':
						seek = in_codecs.head->seek(in_codecs.head, pos + input_fs * 30 - delay);
						if (seek >= 0) {
							pos = seek;
							out_codec->reset(out_codec);
							reset_effects_chain(&chain);
						}
						break;
					case 'r':
						seek = in_codecs.head->seek(in_codecs.head, 0);
						if (seek >= 0) {
							pos = seek;
							out_codec->reset(out_codec);
							reset_effects_chain(&chain);
						}
						break;
					case 'n':
						out_codec->reset(out_codec);
						reset_effects_chain(&chain);
						goto next_input;
					case 'c':
						out_codec->pause(out_codec, pause = (pause) ? 0 : 1);
						break;
					case 'e':
						if (show_progress)
							fputs("\033[1K\r", stderr);
						LOG(LL_NORMAL, "dsp: info: rebuilding effects chain\n");
						if (!pause) {
							do {
								w = dsp_globals.buf_frames;
								obuf = drain_effects_chain(&chain, &w, buf1, buf2);
								if (w > 0)
									write_to_output(w, obuf, do_dither);
							} while (w != -1);
						}
						destroy_effects_chain(&chain);
						stream.fs = input_fs;
						stream.channels = input_channels;
						if (build_effects_chain(effect_argc, &argv[effect_start], &chain, &stream, NULL, NULL))
							cleanup_and_exit(1);
						if (out_codec->fs != stream.fs) {
							LOG(LL_ERROR, "dsp: error: sample rate mismatch: %s\n", out_codec->path);
							cleanup_and_exit(1);
						}
						if (out_codec->channels != stream.channels) {
							LOG(LL_ERROR, "dsp: error: channels mismatch: %s\n", out_codec->path);
							cleanup_and_exit(1);
						}
						free(buf1);
						free(buf2);
						buf1 = calloc(ceil(dsp_globals.buf_frames * input_channels * get_effects_chain_max_ratio(&chain)), sizeof(sample_t));
						buf2 = calloc(ceil(dsp_globals.buf_frames * input_channels * get_effects_chain_max_ratio(&chain)), sizeof(sample_t));
						break;
					case 'v':
						verbose_progress = (verbose_progress) ? 0 : 1;
						break;
					case 'q':
						out_codec->reset(out_codec);
						if (show_progress)
							fputs("\033[1K\r", stderr);
						goto end_rw_loop;
					}
					if (show_progress)
						print_progress(in_codecs.head, out_codec, pos, pause);
				}
				w = r = in_codecs.head->read(in_codecs.head, buf1, dsp_globals.buf_frames);
				pos += r;
				obuf = run_effects_chain(&chain, &w, buf1, buf2);
				write_to_output(w, obuf, do_dither);
				k += w;
				if (show_progress && k >= out_codec->fs) {
					print_progress(in_codecs.head, out_codec, pos, pause);
					k = 0;
				}
			} while (r > 0);
			next_input:
			pos = 0;
			destroy_codec_list_head(&in_codecs);
			if (show_progress)
				fputs("\033[1K\r", stderr);
		}
		do {
			w = dsp_globals.buf_frames;
			obuf = drain_effects_chain(&chain, &w, buf1, buf2);
			if (w > 0)
				write_to_output(w, obuf, do_dither);
		} while (w != -1);
	}
	end_rw_loop:
	cleanup_and_exit(0);
	return 0;
}
