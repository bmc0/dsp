#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "util.h"

#define CHOOSE_INPUT_FS(x) \
	(((x) == -1) ? (in_codecs.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_FS : in_codecs.head->fs : (x))
#define CHOOSE_INPUT_CHANNELS(x) \
	(((x) == -1) ? (in_codecs.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_CHANNELS : in_codecs.head->channels : (x))
#define SHOULD_DITHER(in, out, has_effects) \
	(force_dither != -1 && (out)->can_dither && (force_dither == 1 || ((out)->prec < 24 && ((has_effects) || (in)->prec > (out)->prec || !(in)->can_dither))))
#define TIME_FMT "%.2zd:%.2zd:%05.2lf"
#define TIME_FMT_ARGS(frames, fs) \
	((frames) != -1) ? (frames) / (fs) / 3600 : 0, \
	((frames) != -1) ? ((frames) / (fs) / 60) % 60 : 0, \
	((frames) != -1) ? fmod((double) (frames) / (fs), 60.0) : 0
#if _POSIX_TIMERS && defined(_POSIX_MONOTONIC_CLOCK)
#define HAVE_CLOCK_GETTIME
#else
#warning "clock_gettime() not available; Progress line throttling won't work."
#endif

struct codec_params {
	const char *path, *type, *enc;
	int fs, channels, endian, mode;
};

enum {
	INPUT_MODE_CONCAT,
	INPUT_MODE_SEQUENCE,
};

static struct termios term_attrs;
static int interactive = -1, show_progress = 1, plot = 0, input_mode = INPUT_MODE_CONCAT,
	term_attrs_saved = 0, force_dither = 0, drain_effects = 1, verbose_progress = 0;
static volatile sig_atomic_t term_sig = 0, tstp_sig = 0;
static struct effects_chain chain = { NULL, NULL };
static struct codec_list in_codecs = { NULL, NULL };
static struct codec *out_codec = NULL;
static sample_t *buf1 = NULL, *buf2 = NULL, *obuf;

static const char help_text[] =
	"Usage: %s [options] path ... [!] [:channel_selector] [@[~/]effects_file] [effect [args ...]] ...\n"
	"\n"
	"Global options:\n"
	"  -h         show this help\n"
	"  -b frames  set buffer size (must be given before the first input)\n"
	"  -R ratio   set codec maximum buffer ratio (must be given before the first input)\n"
	"  -i         force interactive mode\n"
	"  -I         disable interactive mode\n"
	"  -q         disable progress display\n"
	"  -s         silent mode\n"
	"  -v         verbose mode\n"
	"  -d         force dithering\n"
	"  -D         disable dithering\n"
	"  -E         don't drain effects chain before rebuilding\n"
	"  -p         plot effects chain instead of processing audio\n"
	"  -V         enable verbose progress display\n"
	"  -S         run in sequence mode\n"
	"\n"
	"Input/output options:\n"
	"  -o               output\n"
	"  -t type          type\n"
	"  -e encoding      encoding\n"
	"  -B/L/N           big/little/native endian\n"
	"  -r frequency[k]  sample rate\n"
	"  -c channels      number of channels\n"
	"  -n               equivalent to '-t null null'\n"
	"\n"
	"Selector syntax:\n"
	"  [[start][-[end]][,...]]\n";

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
	"dsp",                  /* prog_name */
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
		LOG_FMT(LL_NORMAL, "warning: clipped %ld samples (%.2fdBFS peak)",
			dsp_globals.clip_count, log10(dsp_globals.peak) * 20);
	exit(s);
}

static void setup_term(void)
{
	struct termios n;
	if (!term_attrs_saved) {
		tcgetattr(0, &term_attrs);
		term_attrs_saved = 1;
	}
	n = term_attrs;
	n.c_lflag &= ~(ICANON | ECHO | ISIG);
	n.c_cc[VMIN] = 1;
	n.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &n);
}

static int input_pending(void)
{
	struct timeval t = { 0, 0 };
	fd_set f;
	FD_ZERO(&f);
	FD_SET(STDIN_FILENO, &f);
	if (select(1, &f, NULL, NULL, &t) == -1) return 0;
	return FD_ISSET(STDIN_FILENO, &f);
}

static void print_help(void)
{
	fprintf(stdout, help_text, dsp_globals.prog_name);
	fputc('\n', stdout);
	print_all_codecs();
	fputc('\n', stdout);
	print_all_effects();
}

static int parse_codec_params(int argc, char *argv[], struct codec_params *p)
{
	int opt;
	char *endptr;
	/* reset codec_params */
	p->path = p->type = p->enc = NULL;  /* path will always be set if return value is zero */
	p->fs = p->channels = -1;
	p->endian = CODEC_ENDIAN_DEFAULT;
	p->mode = CODEC_MODE_READ;

	while ((opt = getopt(argc, argv, "+:hb:R:iIqsvdDEpVSot:e:BLNr:c:n")) != -1) {
		switch (opt) {
		case 'h':
			print_help();
			cleanup_and_exit(0);
		case 'b':
			if (in_codecs.head == NULL) {
				dsp_globals.buf_frames = strtol(optarg, &endptr, 10);
				if (check_endptr(NULL, optarg, endptr, "buffer size")) return 1;
				if (dsp_globals.buf_frames <= 0) {
					LOG_S(LL_ERROR, "error: buffer size must be > 0");
					return 1;
				}
			}
			else
				LOG_S(LL_ERROR, "warning: buffer size must be specified before the first input");
			break;
		case 'R':
			if (in_codecs.head == NULL) {
				dsp_globals.max_buf_ratio = strtol(optarg, &endptr, 10);
				if (check_endptr(NULL, optarg, endptr, "buffer ratio")) return 1;
				if (dsp_globals.max_buf_ratio <= 0) {
					LOG_S(LL_ERROR, "error: buffer ratio must be > 0");
					return 1;
				}
			}
			else
				LOG_S(LL_ERROR, "warning: buffer ratio must be specified before the first input");
			break;
		case 'i':
			interactive = 1;
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
		case 'E':
			drain_effects = 0;
			break;
		case 'p':
			plot = 1;
			break;
		case 'V':
			verbose_progress = 1;
			break;
		case 'S':
			input_mode = INPUT_MODE_SEQUENCE;
			break;
		case 'o':
			p->mode = CODEC_MODE_WRITE;
			break;
		case 't':
			p->type = optarg;
			break;
		case 'e':
			p->enc = optarg;
			break;
		case 'B':
			p->endian = CODEC_ENDIAN_BIG;
			break;
		case 'L':
			p->endian = CODEC_ENDIAN_LITTLE;
			break;
		case 'N':
			p->endian = CODEC_ENDIAN_NATIVE;
			break;
		case 'r':
			p->fs = parse_freq(optarg, &endptr);
			if (check_endptr(NULL, optarg, endptr, "sample rate")) return 1;
			if (p->fs <= 0) {
				LOG_S(LL_ERROR, "error: sample rate must be > 0");
				return 1;
			}
			break;
		case 'c':
			p->channels = strtol(optarg, &endptr, 10);
			if (check_endptr(NULL, optarg, endptr, "number of channels")) return 1;
			if (p->channels <= 0) {
				LOG_S(LL_ERROR, "error: number of channels must be > 0");
				return 1;
			}
			break;
		case 'n':
			p->path = p->type = "null";
			return 0;
		default:
			if (opt == ':')
				LOG_FMT(LL_ERROR, "error: expected argument to option '%c'", optopt);
			else
				LOG_FMT(LL_ERROR, "error: illegal option '%c'", optopt);
			return 1;
		}
	}
	if (optind < argc)
		p->path = argv[optind++];
	else {
		LOG_S(LL_ERROR, "error: expected path");
		return 1;
	}
	return 0;
}

static void print_io_info(struct codec *c, int ll, const char *n)
{
	LOG_FMT(ll, "%s: %s; type=%s enc=%s precision=%d channels=%d fs=%d frames=%zd ["TIME_FMT"]",
		n, c->path, c->type, c->enc, c->prec, c->channels, c->fs, c->frames, TIME_FMT_ARGS(c->frames, c->fs));
}

#ifdef HAVE_CLOCK_GETTIME
static int has_elapsed(struct timespec *then, double s)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int r = ((now.tv_sec - then->tv_sec) + (now.tv_nsec - then->tv_nsec) / 1e9) >= s;
	if (r) *then = now;
	return r;
}
#endif

static void print_progress(struct codec *in, struct codec *out, ssize_t pos, int is_paused, int force)
{
#ifdef HAVE_CLOCK_GETTIME
	static struct timespec then;
	if (has_elapsed(&then, 0.1) || force) {
#endif
		double in_delay_s = (double) in->delay(in) / in->fs;
		double out_delay_s = (double) out->delay(out) / out->fs;
		double effects_chain_delay_s = get_effects_chain_delay(&chain);
		ssize_t delay = lround((out_delay_s + effects_chain_delay_s) * in->fs);
		ssize_t p = (pos > delay) ? pos - delay : 0;
		ssize_t rem = (in->frames > p) ? in->frames - p : 0;
		fprintf(stderr, "\r%c  %.1f%%  "TIME_FMT"  -"TIME_FMT"  ",
			(is_paused) ? '|' : '>', (in->frames != -1) ? (double) p / in->frames * 100.0 : 0,
			TIME_FMT_ARGS(p, in->fs), TIME_FMT_ARGS(rem, in->fs));
		if (verbose_progress)
			fprintf(stderr, "lat:%.2fms+%.2fms+%.2fms=%.2fms  ",
				in_delay_s * 1000.0, effects_chain_delay_s * 1000.0, out_delay_s * 1000.0, (in_delay_s + effects_chain_delay_s + out_delay_s) * 1000.0);
		if (verbose_progress || dsp_globals.clip_count != 0)
			fprintf(stderr, "peak:%.2fdBFS  clip:%ld  ", log10(dsp_globals.peak) * 20, dsp_globals.clip_count);
		fprintf(stderr, "\033[K");
#ifdef HAVE_CLOCK_GETTIME
	}
#endif
}

static void write_out(ssize_t frames, sample_t *buf, int do_dither)
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
		LOG_S(LL_ERROR, "error: short write");
		cleanup_and_exit(1);
	}
}

static ssize_t do_seek(struct codec *in, struct codec *out, ssize_t pos, ssize_t delay, ssize_t offset, int whence)
{
	ssize_t s;
	if (whence == SEEK_SET)
		s = offset;
	else if (whence == SEEK_END)
		s = in->frames + offset;
	else
		s = pos + offset - delay;
	if ((s = in->seek(in, s)) >= 0) {
		out->drop(out);
		reset_effects_chain(&chain);
		return s;
	}
	return pos;
}

static void do_pause(struct codec *in, struct codec *out, int pause_state)
{
	if (in != NULL)  in->pause(in, pause_state);
	if (out != NULL) out->pause(out, pause_state);
}

static struct codec * init_out_codec(struct codec_params *p, struct stream_info *stream, ssize_t frames)
{
	struct codec *c;
	c = init_codec((p->path == NULL) ? "default" : p->path, p->type, p->enc,
		(p->fs == -1) ? stream->fs : p->fs, (p->channels == -1) ? stream->channels : p->channels, p->endian, p->mode);
	if (c == NULL) {
		LOG_S(LL_ERROR, "error: failed to open output");
		return NULL;
	}
	if (c->fs != stream->fs) {
		LOG_FMT(LL_ERROR, "error: sample rate mismatch: %s", c->path);
		return NULL;
	}
	if (c->channels != stream->channels) {
		LOG_FMT(LL_ERROR, "error: channels mismatch: %s", c->path);
		return NULL;
	}
	c->frames = frames;
	return c;
}

static void sig_handler_term(int s)
{
	term_sig = s;
}

static void sig_handler_tstp(int s)
{
	tstp_sig = 1;
}

static void handle_tstp(const struct sigaction *old_sa, const struct sigaction *new_sa, int is_paused)
{
	if (interactive && term_attrs_saved) tcsetattr(0, TCSANOW, &term_attrs);
	if (!is_paused) do_pause(in_codecs.head, out_codec, 1);
	sigaction(SIGTSTP, old_sa, NULL);
	kill(0, SIGTSTP);
	tstp_sig = 0;
	sigaction(SIGTSTP, new_sa, NULL);
	if (interactive) setup_term();
	if (!is_paused) do_pause(in_codecs.head, out_codec, 0);
}

int main(int argc, char *argv[])
{
	int k, is_paused = 0, do_dither = 0, effect_start, effect_argc, ch;
	ssize_t r, w, delay, pos = 0, out_frames, buf_len;
	double in_time = 0;
	struct codec *c = NULL;
	struct stream_info stream;
	struct codec_params p,
		out_p = { NULL, NULL, NULL, -1, -1, CODEC_ENDIAN_DEFAULT, CODEC_MODE_WRITE };
	struct sigaction sa, old_sigtstp_sa, new_sigtstp_sa;

	dsp_globals.prog_name = argv[0];

	sa.sa_handler = sig_handler_term;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	new_sigtstp_sa.sa_handler = sig_handler_tstp;
	sigemptyset(&new_sigtstp_sa.sa_mask);
	new_sigtstp_sa.sa_flags = 0;
	sigaction(SIGTSTP, &new_sigtstp_sa, &old_sigtstp_sa);

	opterr = 0;
	if (!isatty(STDIN_FILENO))
		interactive = 0;
	while (optind < argc && get_effect_info(argv[optind]) == NULL && argv[optind][0] != ':' && argv[optind][0] != '@' && !(argv[optind][0] == '!' && argv[optind][1] == '\0')) {
		if (parse_codec_params(argc, argv, &p))
			cleanup_and_exit(1);
		if (p.mode == CODEC_MODE_WRITE)
			out_p = p;
		else {
			c = init_codec(p.path, p.type, p.enc, CHOOSE_INPUT_FS(p.fs),
				CHOOSE_INPUT_CHANNELS(p.channels), p.endian, p.mode);
			if (c == NULL) {
				LOG_FMT(LL_ERROR, "error: failed to open input: %s", p.path);
				cleanup_and_exit(1);
			}
			print_io_info(c, LL_VERBOSE, "input");
			if (input_mode != INPUT_MODE_SEQUENCE) {
				if (in_codecs.head != NULL && c->fs != in_codecs.head->fs) {
					LOG_S(LL_ERROR, "error: all inputs must have the same sample rate in concatenate mode");
					cleanup_and_exit(1);
				}
				if (in_codecs.head != NULL && c->channels != in_codecs.head->channels) {
					LOG_S(LL_ERROR, "error: all inputs must have the same number of channels in concatenate mode");
					cleanup_and_exit(1);
				}
			}
			if (c->frames == -1 || in_time == -1)
				in_time = -1;
			else
				in_time += (double) c->frames / c->fs;
			append_codec(&in_codecs, c);
		}
	}

	if (dsp_globals.loglevel == 0)
		show_progress = 0;  /* disable progress display if in silent mode */
	if (in_codecs.head == NULL) {
		LOG_S(LL_ERROR, "error: no inputs");
		cleanup_and_exit(1);
	}

	effect_start = optind;
	effect_argc = argc - optind;
	stream.fs = in_codecs.head->fs;
	stream.channels = in_codecs.head->channels;
	if (build_effects_chain(effect_argc, &argv[effect_start], &chain, &stream, NULL, NULL))
		cleanup_and_exit(1);

	if (plot)
		plot_effects_chain(&chain, in_codecs.head->fs);
	else {
		if (in_time == -1)
			out_frames = -1;
		else
			out_frames = (ssize_t) llround(in_time * stream.fs);
		if ((out_codec = init_out_codec(&out_p, &stream, out_frames)) == NULL)
			cleanup_and_exit(1);
		print_io_info(out_codec, LL_NORMAL, "output");

		if (interactive == -1) {
			if (out_codec->interactive)
				interactive = 1;
			else
				interactive = 0;
		}

		buf_len = get_effects_chain_buffer_len(&chain, dsp_globals.buf_frames, in_codecs.head->channels);
		buf1 = calloc(buf_len, sizeof(sample_t));
		buf2 = calloc(buf_len, sizeof(sample_t));
		/* LOG_FMT(LL_VERBOSE, "info: buffer length: %zd samples", (size_t) buf_len); */

		if (interactive) {
			setup_term();
			LOG_S(LL_NORMAL, "info: running interactively; type 'h' for help");
		}
		while (in_codecs.head != NULL) {
			k = 0;
			do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
			LOG_FMT(LL_VERBOSE, "info: dither %s", (do_dither) ? "on" : "off" );
			print_io_info(in_codecs.head, LL_NORMAL, "input");
			if (show_progress)
				print_progress(in_codecs.head, out_codec, pos, is_paused, 1);
			do {
				if (term_sig) goto got_term_sig;
				if (tstp_sig) {
					handle_tstp(&old_sigtstp_sa, &new_sigtstp_sa, is_paused);
					if (show_progress)
						print_progress(in_codecs.head, out_codec, pos, is_paused, 1);
				}
				while (interactive && (input_pending() || is_paused)) {
					delay = lround(((double) out_codec->delay(out_codec) / out_codec->fs + get_effects_chain_delay(&chain)) * in_codecs.head->fs);
					ch = getchar();
					switch (ch) {
					case 'h':
						if (show_progress)
							fputs("\033[1K\r", stderr);
						fprintf(stderr, "\n%s\n", interactive_help);
						break;
					case ',':
						pos = do_seek(in_codecs.head, out_codec, pos, delay, lround(in_codecs.head->fs * -5), SEEK_CUR);
						break;
					case '.':
						pos = do_seek(in_codecs.head, out_codec, pos, delay, lround(in_codecs.head->fs * 5), SEEK_CUR);
						break;
					case '<':
						pos = do_seek(in_codecs.head, out_codec, pos, delay, lround(in_codecs.head->fs * -30), SEEK_CUR);
						break;
					case '>':
						pos = do_seek(in_codecs.head, out_codec, pos, delay, lround(in_codecs.head->fs * 30), SEEK_CUR);
						break;
					case 'r':
						pos = do_seek(in_codecs.head, out_codec, pos, delay, 0, SEEK_SET);
						break;
					case 'n':
						out_codec->drop(out_codec);
						reset_effects_chain(&chain);
						goto next_input;
					case 'c':
						is_paused = !is_paused;
						do_pause(in_codecs.head, out_codec, is_paused);
						break;
					case 'e':
						if (show_progress)
							fputs("\033[1K\r", stderr);
						LOG_S(LL_NORMAL, "info: rebuilding effects chain");
						if (!is_paused && drain_effects) {
							do {
								w = dsp_globals.buf_frames;
								obuf = drain_effects_chain(&chain, &w, buf1, buf2);
								if (w > 0)
									write_out(w, obuf, do_dither);
							} while (w != -1);
						}
						destroy_effects_chain(&chain);
						stream.fs = in_codecs.head->fs;
						stream.channels = in_codecs.head->channels;
						if (build_effects_chain(effect_argc, &argv[effect_start], &chain, &stream, NULL, NULL))
							cleanup_and_exit(1);
						if (input_mode != INPUT_MODE_SEQUENCE) {
							if (out_codec->fs != stream.fs) {
								LOG_FMT(LL_ERROR, "error: sample rate mismatch: %s", out_codec->path);
								cleanup_and_exit(1);
							}
							if (out_codec->channels != stream.channels) {
								LOG_FMT(LL_ERROR, "error: channels mismatch: %s", out_codec->path);
								cleanup_and_exit(1);
							}
						}
						else if (out_codec->fs != stream.fs || out_codec->channels != stream.channels) {
							LOG_S(LL_NORMAL, "info: output sample rate and/or channels changed; reopening output");
							destroy_codec(out_codec);
							if ((out_codec = init_out_codec(&out_p, &stream, -1)) == NULL)
								cleanup_and_exit(1);
							print_io_info(out_codec, LL_NORMAL, "output");
						}
						buf_len = get_effects_chain_buffer_len(&chain, dsp_globals.buf_frames, in_codecs.head->channels);
						buf1 = realloc(buf1, buf_len * sizeof(sample_t));
						buf2 = realloc(buf2, buf_len * sizeof(sample_t));
						do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
						LOG_FMT(LL_VERBOSE, "info: dither %s", (do_dither) ? "on" : "off" );
						break;
					case 'v':
						verbose_progress = !verbose_progress;
						break;
					case 'q':
						out_codec->drop(out_codec);
						if (show_progress)
							fputs("\033[1K\r", stderr);
						goto end_rw_loop;
					default:
						if (term_attrs_saved) {
							if (ch == term_attrs.c_cc[VINTR])      kill(0, SIGINT);
							else if (ch == term_attrs.c_cc[VQUIT]) kill(0, SIGQUIT);
							else if (ch == term_attrs.c_cc[VSUSP]) tstp_sig = 1;
						}
					}
					if (term_sig) goto got_term_sig;
					if (tstp_sig) handle_tstp(&old_sigtstp_sa, &new_sigtstp_sa, is_paused);
					if (show_progress)
						print_progress(in_codecs.head, out_codec, pos, is_paused, 1);
				}
				w = r = in_codecs.head->read(in_codecs.head, buf1, dsp_globals.buf_frames);
				pos += r;
				obuf = run_effects_chain(chain.head, &w, buf1, buf2);
				write_out(w, obuf, do_dither);
				k += w;
				if (show_progress && k >= out_codec->fs) {
					print_progress(in_codecs.head, out_codec, pos, is_paused, 0);
					k -= out_codec->fs;
				}
			} while (r > 0);
			next_input:
			pos = 0;
			stream.fs = in_codecs.head->fs;
			stream.channels = in_codecs.head->channels;
			destroy_codec_list_head(&in_codecs);
			if (show_progress)
				fputs("\033[1K\r", stderr);
			if (in_codecs.head != NULL && (in_codecs.head->fs != stream.fs || in_codecs.head->channels != stream.channels)) {
				LOG_S(LL_NORMAL, "info: input sample rate and/or channels changed; rebuilding effects chain");
				if (!is_paused && drain_effects) {
					do {
						w = dsp_globals.buf_frames;
						obuf = drain_effects_chain(&chain, &w, buf1, buf2);
						if (w > 0)
							write_out(w, obuf, do_dither);
					} while (w != -1);
				}
				destroy_effects_chain(&chain);
				stream.fs = in_codecs.head->fs;
				stream.channels = in_codecs.head->channels;
				if (build_effects_chain(effect_argc, &argv[effect_start], &chain, &stream, NULL, NULL))
					cleanup_and_exit(1);
				if (out_codec->fs != stream.fs || out_codec->channels != stream.channels) {
					LOG_S(LL_NORMAL, "info: output sample rate and/or channels changed; reopening output");
					destroy_codec(out_codec);
					if ((out_codec = init_out_codec(&out_p, &stream, -1)) == NULL)
						cleanup_and_exit(1);
					print_io_info(out_codec, LL_NORMAL, "output");
				}
				buf_len = get_effects_chain_buffer_len(&chain, dsp_globals.buf_frames, in_codecs.head->channels);
				buf1 = realloc(buf1, buf_len * sizeof(sample_t));
				buf2 = realloc(buf2, buf_len * sizeof(sample_t));
			}
		}
		do {
			w = dsp_globals.buf_frames;
			obuf = drain_effects_chain(&chain, &w, buf1, buf2);
			if (w > 0)
				write_out(w, obuf, do_dither);
		} while (w != -1);
	}
	end_rw_loop:
	cleanup_and_exit(0);

	got_term_sig:
	fputc('\n', stderr);
	LOG_FMT(LL_NORMAL, "info: signal %d: terminating...", term_sig);
	if (out_codec != NULL) out_codec->drop(out_codec);
	goto end_rw_loop;

	return 0;
}
