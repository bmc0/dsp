/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2024 Michael Barbour <barbour.michael.0@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include "dsp.h"
#include "effect.h"
#include "codec.h"
#include "codec_buf.h"
#include "util.h"

#define CHOOSE_INPUT_FS(x) \
	(((x) == 0) ? (in_codecs.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_FS : in_codecs.head->fs : (x))
#define CHOOSE_INPUT_CHANNELS(x) \
	(((x) == 0) ? (in_codecs.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_CHANNELS : in_codecs.head->channels : (x))
#define SHOULD_DITHER(in, out, has_effects) \
	(force_dither != -1 && ((out)->hints & CODEC_HINT_CAN_DITHER) && \
		(force_dither == 1 || ((out)->prec < 24 && ((has_effects) || (in)->prec > (out)->prec || !((in)->hints & CODEC_HINT_CAN_DITHER)))))
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

enum input_mode {
	INPUT_MODE_CONCAT,
	INPUT_MODE_SEQUENCE,
};

enum event_type {
	EVENT_TYPE_SIGNAL = 1,
	EVENT_TYPE_KEY,
	EVENT_TYPE_CODEC_ERROR,
};

struct event {
	enum event_type type;
	int val;
};

#define PROGRESS_MAX_LEN 1024
#define CLEAR_PROGRESS   "\033[1K\r"

static struct termios term_attrs;
static int interactive = -1, show_progress = 1, plot = 0, term_attrs_saved = 0,
	force_dither = 0, drain_effects = 1, verbose_progress = 0, progress_cleared = 0,
	block_frames = DEFAULT_BLOCK_FRAMES, input_buf_ratio = DEFAULT_BUF_RATIO,
	output_buf_ratio = DEFAULT_BUF_RATIO;
enum input_mode input_mode = INPUT_MODE_CONCAT;
static ssize_t clip_count = 0;
static sample_t peak = 0.0, dither_mult = 0.0;
static struct effects_chain chain = EFFECTS_CHAIN_INITIALIZER;
static struct codec_list in_codecs = CODEC_LIST_INITIALIZER;
static struct codec *out_codec = NULL;
static struct codec_write_buf *out_codec_buf = NULL;
static sample_t *buf1 = NULL, *buf2 = NULL;
static char *progress_line = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sig_thread, key_thread;
static int have_sig_thread = 0, have_key_thread = 0;
static struct {
	pthread_mutex_t lock;
	struct event ev[8];
	int front, back, init;
	sem_t slots, items;
} ev_queue = { .lock = PTHREAD_MUTEX_INITIALIZER };

static const char help_text[] =
	"Usage: %s [options] path ... [effect [args]] ...\n"
	"\n"
	"Global options:\n"
	"  -h         show this help\n"
	"  -b frames  block size (must be given before the first input)\n"
	"  -i         force interactive mode\n"
	"  -I         disable interactive mode\n"
	"  -q         disable progress display\n"
	"  -s         silent mode\n"
	"  -v         verbose mode\n"
	"  -d         force dithering\n"
	"  -D         disable dithering\n"
	"  -E         don't drain effects chain before rebuilding\n"
	"  -p         plot effects chain magnitude response instead of processing audio\n"
	"  -P         same as '-p', but also plot phase response\n"
	"  -V         verbose progress display\n"
	"  -S         use \"sequence\" input combining mode\n"
	"\n"
	"Input/output options:\n"
	"  -o               output\n"
	"  -t type          type\n"
	"  -e encoding      encoding\n"
	"  -B/L/N           big/little/native endian\n"
	"  -r frequency[k]  sample rate\n"
	"  -c channels      number of channels\n"
	"  -R ratio         buffer ratio\n"
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
	"  s : send signal to effects chain\n"
	"  q : quit\n";

struct dsp_globals dsp_globals = {
	LL_NORMAL,              /* loglevel */
	"dsp",                  /* prog_name */
};

int dsp_log_printf(const char *fmt, ...)
{
	int r;
	pthread_mutex_lock(&log_lock);
	if (progress_line && !progress_cleared)
		fputs(CLEAR_PROGRESS, stderr);
	va_list v;
	va_start(v, fmt);
	r = vfprintf(stderr, fmt, v);
	va_end(v);
	if (progress_line && !progress_cleared)
		fputs(progress_line, stderr);
	pthread_mutex_unlock(&log_lock);
	return r;
}

void ev_queue_push(enum event_type type, int val)
{
	while (sem_wait(&ev_queue.slots) != 0);
	pthread_mutex_lock(&ev_queue.lock);
	ev_queue.ev[ev_queue.back].type = type;
	ev_queue.ev[ev_queue.back].val = val;
	ev_queue.back = (ev_queue.back+1) % LENGTH(ev_queue.ev);
	pthread_mutex_unlock(&ev_queue.lock);
	sem_post(&ev_queue.items);
}

int ev_queue_pop(int blocking, struct event *ev)
{
	if (blocking) while(sem_wait(&ev_queue.items) != 0);
	else {
		int err;
		while ((err = sem_trywait(&ev_queue.items)) < 0 && errno == EINTR);
		if (err != 0) return -1;
	}
	pthread_mutex_lock(&ev_queue.lock);
	*ev = ev_queue.ev[ev_queue.front];
	ev_queue.front = (ev_queue.front+1) % LENGTH(ev_queue.ev);
	pthread_mutex_unlock(&ev_queue.lock);
	sem_post(&ev_queue.slots);
	return 0;
}

static void * sig_worker(void *arg)
{
	sigset_t *set = (sigset_t *) arg;
	int sig;
	for (;;) {
		if (sigwait(set, &sig) != 0) {
			LOG_S(LL_ERROR, "sig_worker: error: sigwait() failed");
			ev_queue_push(EVENT_TYPE_SIGNAL, SIGTERM);
			break;
		}
		ev_queue_push(EVENT_TYPE_SIGNAL, sig);
	}
	return NULL;
}

static void * key_worker(void *arg)
{
	for (;;) {
		ssize_t r;
		char ch = 0;
		while ((r = read(STDIN_FILENO, &ch, 1)) < 0 && errno == EINTR);
		if (r == 1) ev_queue_push(EVENT_TYPE_KEY, ch);
		else if (r < 0) LOG_FMT(LL_ERROR, "key_worker: read error: %s", strerror(errno));
	}
	return NULL;
}

static void clear_progress(int n)
{
	pthread_mutex_lock(&log_lock);
	if (progress_line && !progress_cleared) {
		if (n) fputc('\n', stderr);
		else fputs(CLEAR_PROGRESS, stderr);
	}
	progress_cleared = 1;
	pthread_mutex_unlock(&log_lock);
}

static void cleanup_and_exit(int s)
{
	clear_progress(s);
	if (have_key_thread) {
		pthread_cancel(key_thread);
		pthread_join(key_thread, NULL);
	}
	if (have_sig_thread) {
		pthread_cancel(sig_thread);
		pthread_join(sig_thread, NULL);
	}
	if (ev_queue.init) {
		sem_destroy(&ev_queue.slots);
		sem_destroy(&ev_queue.items);
	}
	destroy_codec_list(&in_codecs);
	if (out_codec_buf) out_codec_buf->destroy(out_codec_buf);
	if (out_codec) destroy_codec(out_codec);
	destroy_effects_chain(&chain);
	free(buf1);
	free(buf2);
	free(progress_line);
	progress_line = NULL;
	if (term_attrs_saved)
		tcsetattr(0, TCSANOW, &term_attrs);
	if (clip_count > 0)
		LOG_FMT(LL_NORMAL, "warning: clipped %ld samples (%.2fdBFS peak)",
			clip_count, 20.0*log10(peak));
	pthread_mutex_destroy(&log_lock);
	exit(s);
}

static void term_setup(void)
{
	struct termios n;
	if (!term_attrs_saved) {
		tcgetattr(0, &term_attrs);
		term_attrs_saved = 1;
	}
	n = term_attrs;
	n.c_lflag &= ~(ICANON | ECHO);
	n.c_cc[VMIN] = 1;
	n.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &n);
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
	p->fs = p->channels = 0;
	p->endian = CODEC_ENDIAN_DEFAULT;
	p->mode = CODEC_MODE_READ;
	p->buf_ratio = 0;

	while ((opt = getopt(argc, argv, "+:hb:iIqsvdDEpPVSot:e:BLNr:c:R:n")) != -1) {
		switch (opt) {
		case 'h':
			print_help();
			cleanup_and_exit(0);
		case 'b':
			if (in_codecs.head == NULL) {
				block_frames = strtol(optarg, &endptr, 10);
				if (check_endptr(NULL, optarg, endptr, "block size")) return 1;
				if (block_frames <= 1) {
					LOG_S(LL_ERROR, "error: block size must be > 1");
					return 1;
				}
			}
			else
				LOG_S(LL_ERROR, "warning: block size must be specified before the first input");
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
		case 'P':
			plot = 2;
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
			p->fs = lround(parse_freq(optarg, &endptr));
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
		case 'R':
			p->buf_ratio = strtol(optarg, &endptr, 10);
			if (check_endptr(NULL, optarg, endptr, "buffer ratio")) return 1;
			if (p->buf_ratio <= 0) {
				LOG_S(LL_ERROR, "error: buffer ratio must be > 0");
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
	if (p->buf_ratio == 0) {
		if (p->mode == CODEC_MODE_WRITE)
			p->buf_ratio = output_buf_ratio;
		else
			p->buf_ratio = input_buf_ratio;
	}
	else {
		if (p->mode == CODEC_MODE_WRITE)
			output_buf_ratio = p->buf_ratio;
		if (p->mode == CODEC_MODE_READ)
			input_buf_ratio = p->buf_ratio;
	}
	p->block_frames = block_frames;
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

static void get_delay_sec(double *chain_delay, double *out_delay)
{
	*chain_delay = get_effects_chain_delay(&chain);
	if (out_codec_buf)
		*out_delay = (double) out_codec_buf->delay(out_codec_buf) / out_codec->fs;
	else
		*out_delay = (double) out_codec->delay(out_codec) / out_codec->fs;
}

static ssize_t get_delay_frames(double fs, double chain_delay_s, double out_delay_s)
{
	return lround((chain_delay_s+out_delay_s)*fs);
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

static void print_progress(struct codec *in, ssize_t pos, int is_paused, int force)
{
	if (!show_progress)
		return;
#ifdef HAVE_CLOCK_GETTIME
	static struct timespec then;
	if (has_elapsed(&then, 0.1) || force) {
#endif
		double in_delay_s, chain_delay_s, out_delay_s;
		in_delay_s = (double) in->delay(in) / in->fs;
		get_delay_sec(&chain_delay_s, &out_delay_s);
		ssize_t delay = get_delay_frames(in->fs, chain_delay_s, out_delay_s);
		ssize_t p = (pos > delay) ? pos - delay : 0;
		ssize_t rem = (in->frames > p) ? in->frames - p : 0;
		if (progress_line == NULL)
			progress_line = calloc(PROGRESS_MAX_LEN, sizeof(char));
		int pl = snprintf(progress_line, PROGRESS_MAX_LEN, "%c  %.1f%%  "TIME_FMT"  -"TIME_FMT"  ",
			(is_paused) ? '|' : '>', (in->frames != -1) ? (double) p / in->frames * 100.0 : 0,
			TIME_FMT_ARGS(p, in->fs), TIME_FMT_ARGS(rem, in->fs));
		if (pl < PROGRESS_MAX_LEN - 1 && verbose_progress)
			pl += snprintf(progress_line + pl, PROGRESS_MAX_LEN - pl, "lat:%.2fms+%.2fms+%.2fms=%.2fms  ",
				in_delay_s*1000.0, chain_delay_s*1000.0, out_delay_s*1000.0, (in_delay_s+chain_delay_s+out_delay_s)*1000.0);
		if (pl < PROGRESS_MAX_LEN - 1 && (verbose_progress || clip_count != 0))
			pl += snprintf(progress_line + pl, PROGRESS_MAX_LEN - pl, "peak:%.2fdBFS  clip:%ld  ",
				20.0*log10(peak), clip_count);
		pthread_mutex_lock(&log_lock);
		fprintf(stderr, "\r%s\033[K", progress_line);
		progress_cleared = 0;
		pthread_mutex_unlock(&log_lock);
#ifdef HAVE_CLOCK_GETTIME
	}
#endif
}

static void write_buf_error_cb(int error)
{
	switch (error) {
	case CODEC_BUF_ERROR_SHORT_WRITE:
		LOG_S(LL_ERROR, "error: short write");
		break;
	default:
		LOG_S(LL_ERROR, "error: unknown write error");
	}
	ev_queue_push(EVENT_TYPE_CODEC_ERROR, error);
}

static inline sample_t clip(sample_t s)
{
	const sample_t a = fabs(s);
	peak = MAXIMUM(a, peak);
	if (s > 1.0) {
		++clip_count;
		return 1.0;
	}
	else if (s < -1.0) {
		++clip_count;
		return -1.0;
	}
	return s;
}

static void write_out(ssize_t frames, sample_t *buf, int do_dither)
{
	const ssize_t samples = frames * out_codec->channels;
	if (do_dither) {
		for (ssize_t i = 0; i < samples; ++i)
			buf[i] = clip(buf[i] + tpdf_noise(dither_mult));
	}
	else {
		for (ssize_t i = 0; i < samples; ++i)
			buf[i] = clip(buf[i]);
	}
	if (out_codec_buf)
		out_codec_buf->write(out_codec_buf, buf, frames);
	else {
		if (out_codec->write(out_codec, buf, frames) != frames)
			write_buf_error_cb(CODEC_BUF_ERROR_SHORT_WRITE);
	}
}

static ssize_t do_seek(struct codec *in, ssize_t pos, ssize_t offset, int whence, int pause_state)
{
	ssize_t s;
	if (whence == SEEK_SET)
		s = offset;
	else if (whence == SEEK_END)
		s = in->frames + offset;
	else {
		double chain_delay_s, out_delay_s;
		get_delay_sec(&chain_delay_s, &out_delay_s);
		ssize_t delay = in->delay(in) + get_delay_frames(in->fs, chain_delay_s, out_delay_s);
		s = pos + offset - delay;
	}
	if ((s = in->seek(in, s)) >= 0) {
		reset_effects_chain(&chain);
		if (out_codec_buf) {
			out_codec_buf->drop(out_codec_buf, pause_state);
			if (pause_state) out_codec_buf->sync(out_codec_buf);
		}
		else out_codec->drop(out_codec);
		return s;
	}
	return pos;
}

static void do_pause(struct codec *in, int pause_state, int sync)
{
	if (in) in->pause(in, pause_state);
	if (out_codec_buf) {
		out_codec_buf->pause(out_codec_buf, pause_state);
		if (sync) out_codec_buf->sync(out_codec_buf);
	}
	else out_codec->pause(out_codec, pause_state);
}

static struct codec * init_out_codec(struct codec_params *out_p, struct stream_info *stream, ssize_t frames, int write_buf_blocks)
{
	struct codec_params p = *out_p;
	if (p.path == NULL)  p.path = CODEC_DEFAULT_DEVICE;
	if (p.fs == 0)       p.fs = stream->fs;
	if (p.channels == 0) p.channels = stream->channels;
	p.block_frames = get_effects_chain_max_out_frames(&chain, block_frames);

	if ((out_codec = init_codec(&p)) == NULL) {
		LOG_S(LL_ERROR, "error: failed to open output");
		return NULL;
	}
	if (out_codec->fs != stream->fs) {
		LOG_FMT(LL_ERROR, "error: sample rate mismatch: %s", out_codec->path);
		return NULL;
	}
	if (out_codec->channels != stream->channels) {
		LOG_FMT(LL_ERROR, "error: channels mismatch: %s", out_codec->path);
		return NULL;
	}
	out_codec->frames = frames;
	print_io_info(out_codec, LL_NORMAL, "output");

	if (write_buf_blocks > 1 && !(out_codec->hints & CODEC_HINT_NO_OUT_BUF))
		out_codec_buf = codec_write_buf_init(out_codec, p.block_frames, write_buf_blocks, write_buf_error_cb);
	if (out_codec_buf)
		LOG_S(LL_VERBOSE, "info: write buffer enabled");

	return out_codec;
}

static void handle_tstp(int is_paused)
{
	sigset_t set;
	if (interactive && term_attrs_saved) tcsetattr(0, TCSANOW, &term_attrs);
	if (!is_paused) do_pause(in_codecs.head, 1, 1);
	sigemptyset(&set);
	sigaddset(&set, SIGTSTP);
	if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
		LOG_S(LL_ERROR, "error: pthread_sigmask() failed");
		cleanup_and_exit(1);
	}
	raise(SIGTSTP);
	if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
		LOG_S(LL_ERROR, "error: pthread_sigmask() failed");
		cleanup_and_exit(1);
	}
	if (interactive) term_setup();
	if (!is_paused) do_pause(in_codecs.head, 0, 0);
}

#define DRAIN_EFFECTS_CHAIN \
	do { \
		ssize_t w = block_frames; \
		obuf = drain_effects_chain(&chain, &w, buf1, buf2); \
		if (w < 0) break; \
		write_out(w, obuf, do_dither); \
	} while (1)

#define REBUILD_EFFECTS_CHAIN \
	do { \
		destroy_effects_chain(&chain); \
		stream.fs = in_codecs.head->fs; \
		stream.channels = in_codecs.head->channels; \
		if (build_effects_chain(chain_argc, (const char *const *) &argv[chain_start], &chain, &stream, NULL, NULL)) \
			cleanup_and_exit(1); \
	} while (0)

#define REOPEN_OUTPUT \
	do { \
		if (out_codec->fs != stream.fs || out_codec->channels != stream.channels) { \
			LOG_S(LL_NORMAL, "info: output sample rate and/or channels changed; reopening output"); \
			if (out_codec_buf) out_codec_buf->destroy(out_codec_buf); \
			out_codec_buf = NULL; \
			destroy_codec(out_codec); \
			if (init_out_codec(&out_p, &stream, -1, write_buf_blocks) == NULL) \
				cleanup_and_exit(1); \
		} \
	} while (0)

#define REALLOC_BUFS \
	do { \
		const int new_buf_len = get_effects_chain_buffer_len(&chain, block_frames, in_codecs.head->channels); \
		if (new_buf_len > buf_len) { \
			buf_len = new_buf_len; \
			buf1 = realloc(buf1, buf_len*sizeof(sample_t)); \
			buf2 = realloc(buf2, buf_len*sizeof(sample_t)); \
		} \
	} while (0)

int main(int argc, char *argv[])
{
	int is_paused = 0, do_dither = 0, chain_start, chain_argc, term_sig, err;
	double in_time = 0.0;
	struct codec *c = NULL;
	struct stream_info stream;
	struct codec_params p, out_p = CODEC_PARAMS_AUTO(NULL, CODEC_MODE_WRITE);
	sample_t *obuf;

	dsp_globals.prog_name = argv[0];

	opterr = 0;
	if (!isatty(STDIN_FILENO))
		interactive = 0;
	while (optind < argc && !IS_EFFECTS_CHAIN_START(argv[optind])) {
		if (parse_codec_params(argc, argv, &p))
			cleanup_and_exit(1);
		if (p.mode == CODEC_MODE_WRITE)
			out_p = p;
		else {
			p.fs = CHOOSE_INPUT_FS(p.fs);
			p.channels = CHOOSE_INPUT_CHANNELS(p.channels);
			c = init_codec(&p);
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
			if (c->frames == -1 || in_time < 0.0)
				in_time = -1.0;
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

	chain_start = optind;
	chain_argc = argc - optind;
	stream.fs = in_codecs.head->fs;
	stream.channels = in_codecs.head->channels;
	if (build_effects_chain(chain_argc, (const char *const *) &argv[chain_start], &chain, &stream, NULL, NULL))
		cleanup_and_exit(1);

	if (plot)
		plot_effects_chain(&chain, in_codecs.head->fs, in_codecs.head->channels, (plot > 1));
	else {
		sem_init(&ev_queue.slots, 0, LENGTH(ev_queue.ev));
		sem_init(&ev_queue.items, 0, 0);
		ev_queue.init = 1;

		/* sigmask must be set up before creating any threads */
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGINT);
		sigaddset(&set, SIGTERM);
		sigaddset(&set, SIGTSTP);
		if ((err = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0) {
			LOG_FMT(LL_ERROR, "error: pthread_sigmask() failed: %s", strerror(err));
			cleanup_and_exit(1);
		}
		if ((err = pthread_create(&sig_thread, NULL, sig_worker, &set)) != 0) {
			LOG_FMT(LL_ERROR, "error: could not create signal handling thread: %s", strerror(err));
			cleanup_and_exit(1);
		}
		have_sig_thread = 1;

		ssize_t out_frames = (in_time < 0.0) ? -1 : (ssize_t) llround(in_time * stream.fs);
		const int write_buf_blocks = out_p.buf_ratio - 1;
		if (write_buf_blocks > 1)
			out_p.buf_ratio = 1;
		if (init_out_codec(&out_p, &stream, out_frames, write_buf_blocks) == NULL)
			cleanup_and_exit(1);

		if (interactive == -1)
			interactive = (out_codec->hints & CODEC_HINT_INTERACTIVE) ? 1 : 0;
		if (interactive) {
			term_setup();
			if ((err = pthread_create(&key_thread, NULL, key_worker, NULL)) != 0) {
				LOG_FMT(LL_ERROR, "error: could not create key handling thread: %s", strerror(err));
				cleanup_and_exit(1);
			}
			have_key_thread = 1;
			LOG_S(LL_NORMAL, "info: running interactively; type 'h' for help");
		}

		int buf_len = 0;
		REALLOC_BUFS;
		dither_mult = tpdf_dither_get_mult(out_codec->prec);

		while (in_codecs.head != NULL) {
			ssize_t r, pos = 0;
			int k = 0;
			do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
			LOG_FMT(LL_VERBOSE, "info: dither %s", (do_dither) ? "on" : "off" );
			print_io_info(in_codecs.head, LL_NORMAL, "input");
			print_progress(in_codecs.head, pos, is_paused, 1);
			do {
				struct event ev;
				while (ev_queue_pop(is_paused, &ev) == 0) {
					switch (ev.type) {
					case EVENT_TYPE_SIGNAL:
						switch (ev.val) {
						case SIGINT:
						case SIGTERM:
							term_sig = ev.val;
							goto got_term_sig;
						case SIGTSTP:
							handle_tstp(is_paused);
							break;
						default:
							LOG_FMT(LL_ERROR, "%s: BUG: unhandled signal: %d", __func__, ev.val);
						}
						break;
					case EVENT_TYPE_KEY:
						switch (ev.val) {
						case 'h':
							dsp_log_printf("\n%s\n", interactive_help);
							break;
						case ',':
							pos = do_seek(in_codecs.head, pos, (ssize_t) in_codecs.head->fs * -5, SEEK_CUR, is_paused);
							break;
						case '.':
							pos = do_seek(in_codecs.head, pos, (ssize_t) in_codecs.head->fs * 5, SEEK_CUR, is_paused);
							break;
						case '<':
							pos = do_seek(in_codecs.head, pos, (ssize_t) in_codecs.head->fs * -30, SEEK_CUR, is_paused);
							break;
						case '>':
							pos = do_seek(in_codecs.head, pos, (ssize_t) in_codecs.head->fs * 30, SEEK_CUR, is_paused);
							break;
						case 'r':
							pos = do_seek(in_codecs.head, pos, 0, SEEK_SET, is_paused);
							break;
						case 'n':
							if (out_codec_buf) out_codec_buf->drop(out_codec_buf, is_paused);
							else out_codec->drop(out_codec);
							reset_effects_chain(&chain);
							goto next_input;
						case 'c':
							is_paused = !is_paused;
							do_pause(in_codecs.head, is_paused, 0);
							break;
						case 'e':
							clear_progress(0);
							LOG_S(LL_NORMAL, "info: rebuilding effects chain");
							if (!is_paused && drain_effects)
								DRAIN_EFFECTS_CHAIN;
							REBUILD_EFFECTS_CHAIN;
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
							else REOPEN_OUTPUT;
							REALLOC_BUFS;
							do_dither = SHOULD_DITHER(in_codecs.head, out_codec, chain.head != NULL);
							LOG_FMT(LL_VERBOSE, "info: dither %s", (do_dither) ? "on" : "off" );
							break;
						case 'v':
							verbose_progress = !verbose_progress;
							break;
						case 's':
							signal_effects_chain(&chain);
							break;
						case 'q':
							if (out_codec_buf) out_codec_buf->drop(out_codec_buf, 1);
							else out_codec->drop(out_codec);
							goto end_rw_loop;
						}
						break;
					case EVENT_TYPE_CODEC_ERROR:
						cleanup_and_exit(1);
						break;
					default:
						LOG_FMT(LL_ERROR, "%s: BUG: unhandled event type: %d", __func__, (int) ev.type);
					}
					print_progress(in_codecs.head, pos, is_paused, 1);
				}
				ssize_t w = r = in_codecs.head->read(in_codecs.head, buf1, block_frames);
				pos += r;
				obuf = run_effects_chain(chain.head, &w, buf1, buf2);
				write_out(w, obuf, do_dither);
				k += w;
				if (k >= out_codec->fs) {
					print_progress(in_codecs.head, pos, is_paused, 0);
					k -= out_codec->fs;
				}
			} while (r > 0);
			next_input:
			stream.fs = in_codecs.head->fs;
			stream.channels = in_codecs.head->channels;
			destroy_codec_list_head(&in_codecs);
			if (in_codecs.head != NULL && (in_codecs.head->fs != stream.fs || in_codecs.head->channels != stream.channels)) {
				clear_progress(0);
				LOG_S(LL_NORMAL, "info: input sample rate and/or channels changed; rebuilding effects chain");
				if (!is_paused)
					DRAIN_EFFECTS_CHAIN;
				REBUILD_EFFECTS_CHAIN;
				REOPEN_OUTPUT;
				REALLOC_BUFS;
			}
		}
		DRAIN_EFFECTS_CHAIN;
	}
	end_rw_loop:
	cleanup_and_exit(0);

	got_term_sig:
	clear_progress(1);
	LOG_FMT(LL_NORMAL, "info: signal %d: terminating...", term_sig);
	if (out_codec_buf) out_codec_buf->drop(out_codec_buf, 1);
	else if (out_codec) out_codec->drop(out_codec);
	goto end_rw_loop;
}
