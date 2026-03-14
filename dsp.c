/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
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
#include "list_util.h"

#define CHOOSE_INPUT_FS(x) \
	(((x) == 0) ? (input_list.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_FS : input_list.head->codec->fs : (x))
#define CHOOSE_INPUT_CHANNELS(x) \
	(((x) == 0) ? (input_list.head == NULL || input_mode == INPUT_MODE_SEQUENCE) ? DEFAULT_CHANNELS : input_list.head->codec->channels : (x))
#define SHOULD_DITHER(in, out, chain_needs_dither) \
	(force_dither != -1 && ((out)->hints & CODEC_HINT_CAN_DITHER) && \
		(force_dither == 1 || ((out)->prec < 24 && ((chain_needs_dither) || (in)->prec > (out)->prec || !((in)->hints & CODEC_HINT_CAN_DITHER)))))
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
	INPUT_MODE_ABX,
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

static struct termios term_attrs;
static int term_fd = STDIN_FILENO, interactive = -1, show_progress = 1, plot = 0,
	term_attrs_saved = 0, force_dither = 0, drain_effects = 1, verbose_progress = 0,
	status_cleared = -1, status_redraw = 1, block_frames = DEFAULT_BLOCK_FRAMES,
	input_buf_ratio = DEFAULT_INPUT_BUF_RATIO, output_buf_ratio = DEFAULT_OUTPUT_BUF_RATIO;
enum input_mode input_mode = INPUT_MODE_CONCAT;
static ssize_t clip_count = 0;
static sample_t peak = 0.0, dither_mult = 0.0;
static struct effects_chain chain = EFFECTS_CHAIN_INITIALIZER;
static struct effects_chain_xfade_state xfade_state = EFFECTS_CHAIN_XFADE_STATE_INITIALIZER;
static struct read_buf_input_list input_list = READ_BUF_INPUT_LIST_INITIALIZER;
static struct codec_read_buf *in_codec_buf = NULL;
static struct codec *out_codec = NULL;
static struct codec_write_buf *out_codec_buf = NULL;
static sample_t *buf1 = NULL, *buf2 = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sig_thread, key_thread;
static int have_sig_thread = 0, have_key_thread = 0;
static struct {
	pthread_mutex_t lock;
	struct event ev[8];
	int front, back, init;
	sem_t slots, items;
} ev_queue = { .lock = PTHREAD_MUTEX_INITIALIZER };
static char progress_line[DSP_STATUSLINE_MAX_LEN] = {0};
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	struct statusline_state *head, *tail;
	int len;
} status_list = {0};
static struct {
	int rows, cols;
} term_size = {0};

#define ABX_TRIALS_DEFAULT 10
#define ABX_FADE_DURATION  50  /* milliseconds */
static int n_trials = ABX_TRIALS_DEFAULT;
static struct read_buf_input_list abx_inputs[2] = {
	READ_BUF_INPUT_LIST_INITIALIZER,
	READ_BUF_INPUT_LIST_INITIALIZER,
};
static struct codec_read_buf *abx_codec_bufs[2] = { NULL, NULL };

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
	"  -X[n]      run in ABX comparator mode\n"
	"\n"
	"Input/output options:\n"
	"  -o               output\n"
	"  -t type          type\n"
	"  -e encoding      encoding\n"
	"  -B/L/N           big/little/native endian\n"
	"  -r frequency[k]  sample rate\n"
	"  -c channels      number of channels\n"
	"  -R ratio         buffer ratio\n"
	"  -T time_range    set start and end positions (input only)\n"
	"  -l[n]            repeat n times or indefinitely (input only)\n"
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

static const char abx_interactive_help[] =
	"Keys:\n"
	"  h     : display this help\n"
	"  a|1   : play A\n"
	"  b|3   : play B\n"
	"  x|2   : play X\n"
	"  A     : X is A\n"
	"  B     : X is B\n"
	"  Enter : accept current choice\n"
	"  q     : terminate test and quit\n";


struct dsp_globals dsp_globals = {
	LL_NORMAL,              /* loglevel */
	"dsp",                  /* prog_name */
};

static void statuslines_clear(void)
{
	pthread_mutex_lock(&status_lock);
	if (show_progress || status_list.head) {
		dsp_log_puts("\033[1K\r");
		if (status_list.head) {
			for (int i = 0; i < status_list.len; ++i) dsp_log_puts("\n\033[2K");
			dsp_log_printf("\033[%dA", status_list.len);
		}
	}
	pthread_mutex_unlock(&status_lock);
}

static const char * trunc_line(const char *s, int w)
{
	static char buf[DSP_STATUSLINE_MAX_LEN];
	if (w < 1) return s;
	const size_t len = strlen(s);
	if (w >= LENGTH(buf)) w = LENGTH(buf)-1;
	if (len > w) {
		const int trunc_len = MAXIMUM(w-3, 0);
		memcpy(buf, s, trunc_len);
		memcpy(buf+trunc_len, "...", 4);
		return buf;
	}
	return s;
}

static void statuslines_draw(int cr, int force)
{
	pthread_mutex_lock(&status_lock);
	if ((show_progress || status_list.head) && (status_redraw || force)) {
		const int w = term_size.cols - 1;
		if (!cr && show_progress)
			dsp_log_printf("\r%s\033[K\033[2C", trunc_line(progress_line, w));
		LIST_FOREACH(&status_list, line)
			dsp_log_printf("\n%s\033[K", trunc_line(line->s, w));
		dsp_log_putc((cr) ? '\r' : '\n');
		if (cr) {
			if (status_list.head)
				dsp_log_printf("\033[%dA", status_list.len);
			if (show_progress)
				dsp_log_printf("%s\033[K\033[2C", trunc_line(progress_line, w));
		}
		status_redraw = 0;
	}
	pthread_mutex_unlock(&status_lock);
}

void dsp_log_acquire(void)
{
	pthread_mutex_lock(&log_lock);
	if (!status_cleared)
		statuslines_clear();
}

void dsp_log_release(void)
{
	if (!status_cleared)
		statuslines_draw(1, 1);
	pthread_mutex_unlock(&log_lock);
}

void dsp_statuslines_acquire(void)
{
	pthread_mutex_lock(&status_lock);
}

void dsp_statuslines_release(void)
{
	status_redraw = 1;
	pthread_mutex_unlock(&status_lock);
}

void dsp_statusline_register(struct statusline_state *line)
{
	LIST_APPEND(&status_list, line);
	++status_list.len;
}

void dsp_statusline_unregister(struct statusline_state *line)
{
	LIST_REMOVE(&status_list, line);
	pthread_mutex_lock(&log_lock);
	if (!status_cleared)  /* clear last line */
		dsp_log_printf("\033[%dB\033[2K\033[%dA", status_list.len, status_list.len);
	--status_list.len;
	pthread_mutex_unlock(&log_lock);
}

void dsp_get_term_size(int *rows, int *cols)
{
	if (rows) *rows = term_size.rows;
	if (cols) *cols = term_size.cols;
}

static void ev_queue_push(enum event_type type, int val)
{
	while (sem_wait(&ev_queue.slots) != 0);
	pthread_mutex_lock(&ev_queue.lock);
	ev_queue.ev[ev_queue.back].type = type;
	ev_queue.ev[ev_queue.back].val = val;
	ev_queue.back = (ev_queue.back+1) % LENGTH(ev_queue.ev);
	pthread_mutex_unlock(&ev_queue.lock);
	sem_post(&ev_queue.items);
}

static int ev_queue_pop(int blocking, struct event *ev)
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
			LOG_FMT(LL_ERROR, "%s: error: sigwait() failed", __func__);
			ev_queue_push(EVENT_TYPE_SIGNAL, SIGTERM);
			break;
		}
		ev_queue_push(EVENT_TYPE_SIGNAL, sig);
	}
	return NULL;
}

static void * key_worker(void *arg)
{
	const int fd = *((int *) arg);
	for (;;) {
		ssize_t r;
		char ch = 0;
		while ((r = read(fd, &ch, 1)) < 0 && errno == EINTR);
		if (r == 1) ev_queue_push(EVENT_TYPE_KEY, ch);
		else if (r < 0) {
			dsp_perror(DSP_EREAD, NULL, strerror(errno));
			return NULL;
		}
	}
	return NULL;
}

enum status_ctrl_action {
	STATUS_CTRL_DRAW = 1,
	STATUS_CTRL_CLEAR,
	STATUS_CTRL_KEEP,
};

static void status_ctrl(enum status_ctrl_action action)
{
	pthread_mutex_lock(&log_lock);
	switch (action) {
	case STATUS_CTRL_DRAW:
		statuslines_draw(1, 0);
		status_cleared = 0;
		break;
	case STATUS_CTRL_CLEAR:
		if (!status_cleared) {
			statuslines_clear();
			status_cleared = 1;
		}
		break;
	case STATUS_CTRL_KEEP:
		if (status_cleared >= 0) {
			statuslines_draw(0, 1);
			status_cleared = -1;
		}
		break;
	}
	pthread_mutex_unlock(&log_lock);
}

static void __attribute__((noreturn)) cleanup_and_exit(int s)
{
	status_ctrl((s) ? STATUS_CTRL_KEEP : STATUS_CTRL_CLEAR);
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
	codec_read_buf_destroy(in_codec_buf);
	read_buf_input_list_destroy(&input_list);
	for (int i = 0; i < 2; ++i) {
		codec_read_buf_destroy(abx_codec_bufs[i]);
		read_buf_input_list_destroy(&abx_inputs[i]);
	}
	codec_write_buf_destroy(out_codec_buf);
	destroy_codec(out_codec);
	destroy_effects_chain(&chain);
	destroy_effects_chain(&xfade_state.chain[1]);
#ifdef HAVE_FFTW3
	dsp_fftw_save_wisdom();
#endif
	free(buf1);
	free(buf2);
	if (term_attrs_saved)
		tcsetattr(term_fd, TCSANOW, &term_attrs);
	if (clip_count > 0)
		LOG_FMT(LL_NORMAL, "warning: clipped %zd sample%s (%.2fdBFS peak)",
			clip_count, (clip_count == 1) ? "" : "s", 20.0*log10(peak));
	exit(s);
}

static void term_setup(void)
{
	struct termios n;
	if (!term_attrs_saved) {
		tcgetattr(term_fd, &term_attrs);
		term_attrs_saved = 1;
	}
	n = term_attrs;
	n.c_lflag &= ~(ICANON | ECHO);
	n.c_cc[VMIN] = 1;
	n.c_cc[VTIME] = 0;
	tcsetattr(term_fd, TCSANOW, &n);
}

static void print_help(void)
{
	fprintf(stdout, help_text, dsp_globals.prog_name);
	fputc('\n', stdout);
	print_all_codecs();
	fputc('\n', stdout);
	print_all_effects();
}

static int parse_codec_params(struct dsp_getopt_state *g, int argc, const char *const *argv, struct codec_params *p, const char **r_timespan, ssize_t *r_repeats)
{
	int opt;
	char *endptr;
	/* reset codec_params */
	p->path = p->type = p->enc = NULL;  /* path will always be set if return value is zero */
	p->fs = p->channels = 0;
	p->endian = CODEC_ENDIAN_DEFAULT;
	p->mode = CODEC_MODE_READ;
	p->buf_ratio = 0;
	*r_timespan = NULL;
	*r_repeats = 0;

	while ((opt = dsp_getopt(g, argc, argv, "hb:iIqsvdDEpPVSX::ot:e:BLNr:c:R:T:l::n")) != -1) {
		switch (opt) {
		case 'h':
			print_help();
			cleanup_and_exit(0);
		case 'b':
			if (input_list.head == NULL) {
				block_frames = strtol(g->arg, &endptr, 10);
				if (check_endptr(NULL, g->arg, endptr, "block size")) return 1;
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
		case 'X':
			input_mode = INPUT_MODE_ABX;
			if (g->arg) {
				n_trials = strtol(g->arg, &endptr, 10);
				if (check_endptr(NULL, g->arg, endptr, "trials")) return 1;
				if (n_trials < 2) {
					LOG_S(LL_ERROR, "error: minimum number of trials is 2");
					return 1;
				}
			}
			else n_trials = ABX_TRIALS_DEFAULT;
			break;
		case 'o':
			p->mode = CODEC_MODE_WRITE;
			break;
		case 't':
			p->type = g->arg;
			break;
		case 'e':
			p->enc = g->arg;
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
			p->fs = lround(parse_freq(g->arg, &endptr));
			if (check_endptr(NULL, g->arg, endptr, "sample rate")) return 1;
			if (p->fs <= 0) {
				LOG_S(LL_ERROR, "error: sample rate must be > 0");
				return 1;
			}
			break;
		case 'c':
			p->channels = strtol(g->arg, &endptr, 10);
			if (check_endptr(NULL, g->arg, endptr, "number of channels")) return 1;
			if (p->channels <= 0) {
				LOG_S(LL_ERROR, "error: number of channels must be > 0");
				return 1;
			}
			break;
		case 'R':
			p->buf_ratio = strtol(g->arg, &endptr, 10);
			if (check_endptr(NULL, g->arg, endptr, "buffer ratio")) return 1;
			if (p->buf_ratio <= 0) {
				LOG_S(LL_ERROR, "error: buffer ratio must be > 0");
				return 1;
			}
			break;
		case 'n':
			p->path = p->type = "null";
			return 0;
		case 'T':
			*r_timespan = g->arg;
			break;
		case 'l':
			if (g->arg) {
				*r_repeats = strtol(g->arg, &endptr, 10);
				if (check_endptr(NULL, g->arg, endptr, "number of repeats")) return 1;
			}
			else *r_repeats = READ_BUF_INPUT_REPEAT_INF;
			break;
		default:
			dsp_getopt_print_error(g, opt, NULL);
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
	if (g->ind < argc)
		p->path = argv[g->ind++];
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
	*out_delay = (double) codec_write_buf_delay(out_codec_buf) / out_codec->fs;
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

static void update_progress(ssize_t pos, ssize_t repeats, int is_paused, int force)
{
	if (!show_progress)
		return;
#ifdef HAVE_CLOCK_GETTIME
	static struct timespec then;
	if (has_elapsed(&then, 0.1) || force) {
#endif
		struct codec *in = input_list.head->codec;
		double in_delay_s = 0.0, chain_delay_s, out_delay_s;
		get_delay_sec(&chain_delay_s, &out_delay_s);
		ssize_t delay = get_delay_frames(in->fs, chain_delay_s, out_delay_s);
		ssize_t p = MAXIMUM(pos - delay, input_list.head->start);
		ssize_t rem = MAXIMUM(in->frames - p, 0);
		if (verbose_progress)
			in_delay_s = (double) codec_read_buf_delay(in_codec_buf) / in->fs;
		dsp_statuslines_acquire();
		int pl = snprintf(progress_line, LENGTH(progress_line), "%c  %.1f%%  "TIME_FMT"  -"TIME_FMT,
			(is_paused) ? '|' : '>', (in->frames != -1) ? (double) p / in->frames * 100.0 : 0,
			TIME_FMT_ARGS(p, in->fs), TIME_FMT_ARGS(rem, in->fs));
		if (pl < LENGTH(progress_line)-1 && repeats) {
			if (repeats < 0) pl += snprintf(progress_line + pl, LENGTH(progress_line) - pl, "  rep:inf");
			else pl += snprintf(progress_line + pl, LENGTH(progress_line) - pl, "  rep:%zd", repeats);
		}
		if (pl < LENGTH(progress_line)-1 && verbose_progress) {
			pl += snprintf(progress_line + pl, LENGTH(progress_line) - pl, "  lat:%.2fms+%.2fms+%.2fms=%.2fms",
				in_delay_s*1000.0, chain_delay_s*1000.0, out_delay_s*1000.0, (in_delay_s+chain_delay_s+out_delay_s)*1000.0);
		}
		if (pl < LENGTH(progress_line)-1 && (verbose_progress || clip_count != 0)) {
			pl += snprintf(progress_line + pl, LENGTH(progress_line) - pl, "  peak:%.2fdBFS  clip:%zd",
				20.0*log10(peak), clip_count);
		}
		dsp_statuslines_release();
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
	if (a > 1.0) {
		++clip_count;
		return (signbit(s)) ? -1.0 : 1.0;
	}
	return s;
}

static void write_out(ssize_t frames, sample_t *buf, int add_dither)
{
	const ssize_t samples = frames * out_codec->channels;
	if (add_dither) {
		for (ssize_t i = 0; i < samples; ++i)
			buf[i] = clip(buf[i] + tpdf_noise(dither_mult));
	}
	else {
		for (ssize_t i = 0; i < samples; ++i)
			buf[i] = clip(buf[i]);
	}
	codec_write_buf_write(out_codec_buf, buf, frames);
}

static void finish_xfade(void)
{
	destroy_effects_chain(&chain);
	chain = xfade_state.chain[1];
	effects_chain_xfade_reset(&xfade_state);
}

static ssize_t do_seek(ssize_t pos, ssize_t offset, int whence, int pause_state)
{
	ssize_t s;
	struct codec *in = input_list.head->codec;
	if (whence == SEEK_SET)
		s = offset;
	else if (whence == SEEK_END)
		s = in->frames + offset;
	else {
		double chain_delay_s, out_delay_s;
		get_delay_sec(&chain_delay_s, &out_delay_s);
		s = pos + offset - get_delay_frames(in->fs, chain_delay_s, out_delay_s);
	}
	if ((s = codec_read_buf_seek(in_codec_buf, s)) >= 0) {
		if (xfade_state.pos > 0) finish_xfade();
		reset_effects_chain(&chain);
		codec_write_buf_drop(out_codec_buf, pause_state || (in->hints & CODEC_HINT_REALTIME), pause_state);
		return s;
	}
	return pos;
}

static void do_pause(int pause_state, int sync)
{
	codec_read_buf_pause(in_codec_buf, pause_state, sync);
	codec_write_buf_pause(out_codec_buf, pause_state, sync);
}

static struct codec_write_buf * init_out_codec(struct codec_params *out_p, struct stream_info *stream, ssize_t frames, int write_buf_blocks)
{
	struct codec_params p = *out_p;
	if (p.path == NULL)  p.path = CODEC_DEFAULT_DEVICE;
	if (p.fs == 0)       p.fs = stream->fs;
	if (p.channels == 0) p.channels = stream->channels;

	/* FIXME: Not ideal in sequence mode if the resample effect is used and
	   the input sample rate changes after the first input. */
	const ssize_t chain_max_frames = get_effects_chain_max_out_frames(&chain, p.block_frames);
	if (p.block_frames < chain_max_frames) p.block_frames = chain_max_frames;

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

	out_codec_buf = codec_write_buf_init(out_codec, p.block_frames, write_buf_blocks - out_codec->buf_ratio, write_buf_error_cb);
	return out_codec_buf;
}

static void query_term_size(void)
{
	if (term_fd < 0) return;
	dsp_log_acquire();
#if defined(TIOCGWINSZ)
	struct winsize ws;
	if (ioctl(term_fd, TIOCGWINSZ, &ws) == 0) {
		term_size.rows = ws.ws_row;
		term_size.cols = ws.ws_col;
#elif defined(TIOCGSIZE)
	struct ttysize ts;
	if (ioctl(term_fd, TIOCGSIZE, &ts) == 0) {
		term_size.rows = ts.ts_row;
		term_size.cols = ws.ts_col;
#endif
		/* if (LOGLEVEL(LL_VERBOSE))
			dsp_log_printf("%s: info: terminal size: %dx%d\n", dsp_globals.prog_name, term_size.cols, term_size.rows); */
	}
	/* else if (LOGLEVEL(LL_ERROR))
		dsp_log_printf("%s: error: ioctl(): %s\n", dsp_globals.prog_name, strerror(errno)); */
	dsp_log_release();
}

static void handle_tstp(int is_paused)
{
	sigset_t set;
	if (interactive && term_attrs_saved) tcsetattr(term_fd, TCSANOW, &term_attrs);
	if (!is_paused) do_pause(1, 1);
	sigemptyset(&set);
	sigaddset(&set, SIGTSTP);
	if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
		LOG_S(LL_ERROR, "error: pthread_sigmask() failed");
		cleanup_and_exit(1);
	}
	status_ctrl(STATUS_CTRL_KEEP);
	raise(SIGTSTP);
	if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
		LOG_S(LL_ERROR, "error: pthread_sigmask() failed");
		cleanup_and_exit(1);
	}
	if (interactive) term_setup();
	if (!is_paused) do_pause(0, 0);
	query_term_size();
}

static double abx_p_value(int n, int k)
{
	/* Based on https://stackoverflow.com/a/45869209 */
	const double log1_2 = -6.931471805599453094e-01;
	double cdf = exp(n*log1_2), b = 0.0;
	for (int x = 1; x <= n-k; ++x) {
		b += log(n-x+1)-log(x);
		cdf += exp(b + n*log1_2);
	}
	return cdf;
}

static inline double abx_fade_mult(int pos, int n)
{
	const double fade = (double) pos / n;
	return (fade <= 0.5) ? 4.0*(fade*fade*fade) : 1.0-4.0*((1.0-fade)*(1.0-fade)*(1.0-fade));
}

enum { ABX_IBUF_A = 0, ABX_IBUF_B, ABX_IBUF_X };
static inline int abx_ibuf(int id)
{
	switch (id) {
	case 'A': return ABX_IBUF_A;
	case 'B': return ABX_IBUF_B;
	case 'X': return ABX_IBUF_X;
	default: LOG_FMT(LL_ERROR, "%s(): BUG: invalid id: %d", __func__, id);
	}
	return ABX_IBUF_X;
}

static inline int abx_key_to_id(int key)
{
	switch (key) {
	case 'a': case '1': return 'A';
	case 'b': case '3': return 'B';
	case 'x': case '2': return 'X';
	default: LOG_FMT(LL_ERROR, "%s(): BUG: invalid key: %d", __func__, key);
	}
	return 'X';
}

static void update_abx_status(int trial, int n_trials, int cur_input, int last_sel)
{
	dsp_statuslines_acquire();
	int pl = snprintf(progress_line, LENGTH(progress_line), "ABX trial %d of %d / playing: %c", trial+1, n_trials, cur_input);
	if (pl < LENGTH(progress_line)-1 && last_sel)
		pl += snprintf(progress_line+pl, LENGTH(progress_line)-pl, " / current choice: X is %c", last_sel);
	dsp_statuslines_release();
}

#define SET_DITHER(chain, in_codec) \
	do { \
		const int chain_needs_dither = effects_chain_needs_dither(chain); \
		const int do_dither = SHOULD_DITHER(in_codec, out_codec, chain_needs_dither); \
		add_dither = effects_chain_set_dither_params(chain, out_codec->prec, do_dither); \
		LOG_FMT(LL_VERBOSE, "info: auto dither %s%s", (do_dither) ? "on" : "off", \
			(do_dither && !add_dither) ? " (effect)" : ""); \
	} while (0)

static void run_abx_loop(void)
{
	const int in_channels = abx_inputs[0].head->codec->channels;
	const int fade_frames = lrint((ABX_FADE_DURATION/1000.0) * abx_inputs[0].head->codec->fs);
	const ssize_t buf_len = get_effects_chain_buffer_len(&chain, block_frames, in_channels);
	int ret = 0, add_dither = 0, term_sig, fade_pos = 0;
	int trial = 0, n_correct = 0, cur_input = 'X', next_input = 0, last_sel = 0;
	sample_t *ibufs[3] = {0}, *obuf;
	buf1 = calloc(buf_len, sizeof(sample_t));
	buf2 = calloc(buf_len, sizeof(sample_t));
	ibufs[ABX_IBUF_A] = calloc(block_frames, sizeof(sample_t)*in_channels);
	ibufs[ABX_IBUF_B] = calloc(block_frames, sizeof(sample_t)*in_channels);
	char *seq = calloc(n_trials, sizeof(char));
	if (!buf1 || !buf2 || !ibufs[ABX_IBUF_A] || !ibufs[ABX_IBUF_B] || !seq) {
		dsp_perror(DSP_ENOMEM, __func__, NULL);
		goto fail;
	}

	uint32_t seed = ((uint32_t) time(NULL)) & PM_RAND_MAX;
	pm_rand2_r(&seed);
	const int na = n_trials/2+(seed&(n_trials&1));
	memset(seq, 'A', na);
	memset(seq+na, 'B', n_trials-na);
	for (int i = n_trials-1; i > 0; --i) {
		const int k = pm_rand1_r(&seed)/(PM_RAND_MAX/(i+1)+1);
		char tmp = seq[i]; seq[i] = seq[k]; seq[k] = tmp;
	}

	SET_DITHER(&chain, abx_inputs[0].head->codec);
	while (trial < n_trials) {
		ibufs[ABX_IBUF_X] = ibufs[abx_ibuf(seq[trial])];
		LOG_FMT(LL_NORMAL, "info: starting ABX trial %d of %d", trial+1, n_trials);
		if (!show_progress && !next_input) LOG_FMT(LL_NORMAL, "info: playing %c", cur_input);
		update_abx_status(trial, n_trials, (next_input) ? next_input : cur_input, last_sel);
		status_ctrl(STATUS_CTRL_DRAW);
		for (;;) {
			struct event ev;
			while (ev_queue_pop(0, &ev) == 0) {
				int id = 0;
				switch (ev.type) {
				case EVENT_TYPE_SIGNAL:
					switch (ev.val) {
					case SIGINT:
					case SIGTERM:
						term_sig = ev.val;
						goto got_term_sig;
					case SIGTSTP:
					case SIGUSR1:
					case SIGUSR2:
						LOG_FMT(LL_NORMAL, "warning: ignoring signal %d", ev.val);
						break;
					case SIGWINCH:
						query_term_size();
						break;
					default:
						LOG_FMT(LL_ERROR, "%s: BUG: unhandled signal: %d", __func__, ev.val);
					}
					break;
				case EVENT_TYPE_KEY:
					switch (ev.val) {
					case 'h':
						dsp_log_acquire();
						dsp_log_printf("\n%s\n", abx_interactive_help);
						dsp_log_release();
						break;
					case 'a': case '1':
					case 'b': case '3':
					case 'x': case '2':
						id = abx_key_to_id(ev.val);
						if (id != 'X') last_sel = id;
						if (next_input || cur_input != id) next_input = id;
						break;
					case 'A': case 'B':
						last_sel = ev.val;
					case '\n':
						if (last_sel) goto end_trial;
						break;
					case 'q':
						codec_write_buf_drop(out_codec_buf, 1, 0);
						goto done;
					}
					break;
				case EVENT_TYPE_CODEC_ERROR:
					goto fail;
				default:
					LOG_FMT(LL_ERROR, "%s: BUG: unhandled event type: %d", __func__, (int) ev.type);
				}
			}
			ssize_t r_a = codec_read_buf_read(abx_codec_bufs[0], ibufs[ABX_IBUF_A], block_frames);
			ssize_t r_b = codec_read_buf_read(abx_codec_bufs[1], ibufs[ABX_IBUF_B], block_frames);
			if (r_a != r_b) {
				LOG_FMT(LL_ERROR, "error: unequal reads: %zd != %zd", r_a, r_b);
				goto fail;
			}
			if (next_input || fade_pos > 0) {
				/*
				 * Do a brief, non-overlapping fade every time the input is switched.
				 * This is required in order to prevent pops/clicks or other cues that
				 * may be audible when switching between, e.g. B and X, but not A and X
				 * when X is A (or vice versa). An overlapping fade is problematic when
				 * there are phase differences between inputs.
				*/
				if (fade_pos <= 0) fade_pos = fade_frames*2;
				const ssize_t buf_end = r_a*in_channels;
				ssize_t buf_pos = 0;
				sample_t *ibuf = ibufs[abx_ibuf(cur_input)];
				while (--fade_pos > 0 && buf_pos < buf_end) {
					const double fade = (fade_pos > fade_frames)
						? abx_fade_mult(fade_pos-fade_frames, fade_frames)
						: abx_fade_mult(fade_frames-fade_pos, fade_frames);
					if (fade_pos == fade_frames) {
						cur_input = next_input;
						next_input = 0;
						ibuf = ibufs[abx_ibuf(cur_input)];
						update_abx_status(trial, n_trials, cur_input, last_sel);
						if (!show_progress) LOG_FMT(LL_NORMAL, "info: playing %c", cur_input);
					}
					for (int k = 0; k < in_channels; ++k, ++buf_pos)
						buf1[buf_pos] = ibuf[buf_pos] * fade;
				}
				if (buf_pos < buf_end) memcpy(buf1+buf_pos, ibuf+buf_pos, sizeof(sample_t)*(buf_end-buf_pos));
			}
			else memcpy(buf1, ibufs[abx_ibuf(cur_input)], sizeof(sample_t)*r_a*in_channels);
			ssize_t w = r_a;
			obuf = run_effects_chain(&chain, &w, buf1, buf2);
			write_out(w, obuf, add_dither);
			status_ctrl(STATUS_CTRL_DRAW);
		}
		end_trial:
		LOG_FMT(LL_NORMAL, "info: ABX trial %d: choice: X is %c", trial+1, last_sel);
		if (last_sel == seq[trial]) ++n_correct;
		if (cur_input == 'X') cur_input = seq[trial];
		next_input = 'X';
		last_sel = 0;
		++trial;
	}

	done:
	if (trial > 0) {
		LOG_FMT(LL_ERROR, "info: ABX result: %d correct out of %d (p=%g)",
			n_correct, trial, abx_p_value(trial, n_correct));
	}
	free(ibufs[ABX_IBUF_A]);
	free(ibufs[ABX_IBUF_B]);
	free(seq);
	cleanup_and_exit(ret);

	got_term_sig:
	LOG_FMT(LL_NORMAL, "info: signal %d: terminating...", term_sig);
	codec_write_buf_drop(out_codec_buf, 1, 0);
	goto done;

	fail:
	ret = 1;
	goto done;
}

#define DRAIN_EFFECTS_CHAIN \
	do { \
		ssize_t w = block_frames; \
		obuf = drain_effects_chain(&chain, &w, buf1, buf2); \
		if (w < 0) break; \
		write_out(w, obuf, add_dither); \
	} while (1)

#define REBUILD_EFFECTS_CHAIN \
	do { \
		destroy_effects_chain(&chain); \
		stream.fs = input_list.head->codec->fs; \
		stream.channels = input_list.head->codec->channels; \
		if (build_effects_chain_from_argv(chain_argc, (const char *const *) &argv[chain_start], &chain, &stream, NULL, NULL)) \
			cleanup_and_exit(1); \
	} while (0)

#define REOPEN_OUTPUT \
	do { \
		if (out_codec->fs != stream.fs || out_codec->channels != stream.channels) { \
			LOG_S(LL_NORMAL, "info: output sample rate and/or channels changed; reopening output"); \
			codec_write_buf_destroy(out_codec_buf); \
			out_codec_buf = NULL; \
			destroy_codec(out_codec); \
			if (init_out_codec(&out_p, &stream, -1, write_buf_blocks) == NULL) \
				cleanup_and_exit(1); \
		} \
	} while (0)

#define REALLOC_BUFS(chain) \
	do { \
		const ssize_t new_buf_len = get_effects_chain_buffer_len(chain, block_frames, input_list.head->codec->channels); \
		if (new_buf_len > buf_len) { \
			buf_len = new_buf_len; \
			free(buf1); free(buf2); free(xfade_state.buf); \
			buf1 = calloc(buf_len, sizeof(sample_t)); \
			buf2 = calloc(buf_len, sizeof(sample_t)); \
			xfade_state.buf = (!drain_effects) ? calloc(buf_len, sizeof(sample_t)) : NULL; \
			if (!buf1 || !buf2 || (!drain_effects && !xfade_state.buf)) { \
				dsp_perror(DSP_ENOMEM, __func__, NULL); \
				cleanup_and_exit(1); \
			} \
		} \
	} while (0)

int main(int argc, char *argv[])
{
	int is_paused = 0, add_dither = 0, read_buf_blocks = 0, term_sig, err;
	double in_time = 0.0;
	struct dsp_getopt_state g = DSP_GETOPT_STATE_INITIALIZER;
	struct codec_params p, out_p = CODEC_PARAMS_AUTO(NULL, CODEC_MODE_WRITE);
	sample_t *obuf;

	dsp_globals.prog_name = argv[0];

	if (!isatty(term_fd)) {
		term_fd = open(ctermid(NULL), O_RDWR);
		if (term_fd < 0 || !isatty(term_fd))
			interactive = 0;
	}
	while (g.ind < argc && !IS_EFFECTS_CHAIN_START(argv[g.ind])) {
		const char *start_timespec;
		ssize_t repeats;
		if (parse_codec_params(&g, argc, (const char *const *) argv, &p, &start_timespec, &repeats))
			cleanup_and_exit(1);
		if (p.mode == CODEC_MODE_WRITE) {
			if (start_timespec) LOG_FMT(LL_ERROR, "warning: ignoring '-T' option for output: %s", p.path);
			if (repeats) LOG_FMT(LL_ERROR, "warning: ignoring '-l' option for output: %s", p.path);
			out_p = p;
		}
		else {
			p.fs = CHOOSE_INPUT_FS(p.fs);
			p.channels = CHOOSE_INPUT_CHANNELS(p.channels);
			const int req_blocks = p.buf_ratio;
			if (p.buf_ratio - CODEC_BUF_MIN_BLOCKS >= 2)
				p.buf_ratio = 2;
			struct codec *c = init_codec(&p);
			if (c == NULL) {
				LOG_FMT(LL_ERROR, "error: failed to open input: %s", p.path);
				cleanup_and_exit(1);
			}
			read_buf_blocks = MAXIMUM(read_buf_blocks, req_blocks - c->buf_ratio);
			print_io_info(c, LL_VERBOSE, "input");
			ssize_t c_frames = c->frames;
			ssize_t start_pos = 0, end_pos = READ_BUF_INPUT_END_UNSPECIFIED;
			if (start_timespec) {
				char *endptr;
				start_pos = parse_timespec(start_timespec, c->fs, &endptr);
				int end_is_rel = (*endptr == '+');
				if (endptr != start_timespec && (end_is_rel || *endptr == '-')) {
					char *end_timespec = endptr+1;
					end_pos = parse_timespec(end_timespec, c->fs, &endptr);
					if (check_endptr(NULL, end_timespec, endptr, "end timespec"))
						cleanup_and_exit(1);
					if (end_pos < 0) {
						if (end_is_rel) {
							LOG_FMT(LL_ERROR, "error: %s: end timespec must be positive when relative to start timespec", c->path);
							cleanup_and_exit(1);
						}
						end_pos = MAXIMUM(c_frames+end_pos, 0);
					}
				}
				else if (check_endptr(NULL, start_timespec, endptr, "start timespec"))
					cleanup_and_exit(1);
				if (start_pos < 0) start_pos = MAXIMUM(c_frames+start_pos, 0);
				if (start_pos > 0) {
					start_pos = c->seek(c, start_pos);
					if (start_pos < 0) {
						dsp_perror(DSP_ESEEK, NULL, c->path);
						cleanup_and_exit(1);
					}
				}
				if (end_pos >= 0) {
					end_pos = (end_is_rel) ? start_pos+end_pos : end_pos;
					if (end_pos < start_pos) LOG_FMT(LL_ERROR, "warning: %s: end timespec precedes start timespec", c->path);
					c_frames = MINIMUM(c_frames, MAXIMUM(end_pos-start_pos, 0));
				}
				else if (c_frames >= start_pos) c_frames -= start_pos;
			}
			if (c_frames > 0 && repeats > 0)
				c_frames *= repeats+1;
			else if (repeats < 0)
				c_frames = -1;
			if (c_frames == -1 || in_time < 0.0)
				in_time = -1.0;
			else in_time += (double) c_frames / c->fs;
			read_buf_input_list_add(&input_list, c, start_pos, end_pos, repeats);
		}
	}
	if (input_mode != INPUT_MODE_SEQUENCE) {
		LIST_FOREACH(&input_list, input) {
			if (input_list.head != NULL && input->codec->fs != input_list.head->codec->fs) {
				LOG_S(LL_ERROR, "error: all inputs must have the same sample rate");
				cleanup_and_exit(1);
			}
			if (input_list.head != NULL && input->codec->channels != input_list.head->codec->channels) {
				LOG_S(LL_ERROR, "error: all inputs must have the same number of channels");
				cleanup_and_exit(1);
			}
		}
	}

	if (dsp_globals.loglevel == 0)
		show_progress = 0;  /* disable progress display if in silent mode */
	if (input_list.head == NULL) {
		LOG_S(LL_ERROR, "error: no inputs");
		cleanup_and_exit(1);
	}

	const int chain_start = g.ind, chain_argc = argc-g.ind;
	struct stream_info stream = {
		.fs = input_list.head->codec->fs,
		.channels = input_list.head->codec->channels,
	};

	if (plot) {
		if (build_effects_chain_from_argv(chain_argc, (const char *const *) &argv[chain_start], &chain, &stream, NULL, NULL))
			cleanup_and_exit(1);
		plot_effects_chain(&chain, input_list.head->codec->fs, input_list.head->codec->channels, (plot > 1));
	}
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
		sigaddset(&set, SIGUSR1);
		sigaddset(&set, SIGUSR2);
		if (term_fd >= 0) sigaddset(&set, SIGWINCH);
		if ((err = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0) {
			LOG_FMT(LL_ERROR, "error: pthread_sigmask() failed: %s", strerror(err));
			cleanup_and_exit(1);
		}
		if ((err = pthread_create(&sig_thread, NULL, sig_worker, &set)) != 0) {
			LOG_FMT(LL_ERROR, "error: could not create signal handling thread: %s", strerror(err));
			cleanup_and_exit(1);
		}
		have_sig_thread = 1;
		query_term_size();

		if (build_effects_chain_from_argv(chain_argc, (const char *const *) &argv[chain_start], &chain, &stream, NULL, NULL))
			cleanup_and_exit(1);

		if (input_mode == INPUT_MODE_ABX) {
			int n_inputs = 0;
			LIST_FOREACH(&input_list, input) ++n_inputs;
			if (n_inputs != 2) {
				LOG_FMT(LL_ERROR, "error: expected 2 inputs; got %d", n_inputs);
				cleanup_and_exit(1);
			}
			ssize_t abx_frames[2];
			for (int i = 0; i < 2; ++i) {
				struct read_buf_input *input = input_list.head;
				if (input->end != READ_BUF_INPUT_END_UNSPECIFIED)
					abx_frames[i] = input->end - input->start;
				else abx_frames[i] = input->codec->frames;
				if (abx_frames[i] < 0) {
					LOG_S(LL_ERROR, "error: inputs must have a known length");
					cleanup_and_exit(1);
				}
				LIST_REMOVE(&input_list, input);
				input->repeats = READ_BUF_INPUT_REPEAT_INF;
				LIST_APPEND(&abx_inputs[i], input);
			}
			if (abx_frames[0] != abx_frames[1]) {
				LOG_S(LL_ERROR, "error: inputs must be of identical length");
				cleanup_and_exit(1);
			}
			in_time = -1.0;
			for (int i = 0; i < 2; ++i) {
				if ((abx_codec_bufs[i] = codec_read_buf_init(&abx_inputs[i], block_frames, read_buf_blocks, NULL)) == NULL)
					cleanup_and_exit(1);
			}
		}
		else if ((in_codec_buf = codec_read_buf_init(&input_list, block_frames, read_buf_blocks, NULL)) == NULL)
			cleanup_and_exit(1);

		ssize_t out_frames = (in_time < 0.0) ? -1 : (ssize_t) llround(in_time * stream.fs);
		const int write_buf_blocks = out_p.buf_ratio;
		if (out_p.buf_ratio - CODEC_BUF_MIN_BLOCKS >= 2)
			out_p.buf_ratio = 2;
		if (init_out_codec(&out_p, &stream, out_frames, write_buf_blocks) == NULL)
			cleanup_and_exit(1);
		dither_mult = tpdf_dither_get_mult(out_codec->prec);

		if (interactive == -1)
			interactive = (out_codec->hints & CODEC_HINT_INTERACTIVE) ? 1 : 0;
		if (interactive) {
			term_setup();
			if ((err = pthread_create(&key_thread, NULL, key_worker, &term_fd)) != 0) {
				LOG_FMT(LL_ERROR, "error: could not create key handling thread: %s", strerror(err));
				cleanup_and_exit(1);
			}
			have_key_thread = 1;
			LOG_S(LL_NORMAL, "info: running interactively; type 'h' for help");
		}
		else if (input_mode == INPUT_MODE_ABX) {
			LOG_S(LL_ERROR, "error: ABX mode must be interactive");
			cleanup_and_exit(1);
		}
		if (input_mode == INPUT_MODE_ABX) run_abx_loop();  /* does not return */

		ssize_t buf_len = 0;
		REALLOC_BUFS(&chain);

		while (input_list.head != NULL) {
			ssize_t r, pos = input_list.head->start;
			int k = 0, repeats = input_list.head->repeats;
			SET_DITHER(&chain, input_list.head->codec);
			print_io_info(input_list.head->codec, LL_NORMAL, "input");
			update_progress(pos, repeats, is_paused, 1);
			status_ctrl(STATUS_CTRL_DRAW);
			do {
				struct event ev;
				while (ev_queue_pop(is_paused, &ev) == 0) {
					switch (ev.type) {
					case EVENT_TYPE_SIGNAL:
						switch (ev.val) {
						case SIGINT:
						case SIGTERM:
							term_sig = ev.val;
							update_progress(pos, repeats, is_paused, 1);
							goto got_term_sig;
						case SIGTSTP:
							handle_tstp(is_paused);
							print_io_info(input_list.head->codec, LL_NORMAL, "input");
							break;
						case SIGUSR1:
							goto handle_effects_chain_rebuild_request;
						case SIGUSR2:
							signal_effects_chain(&chain);
							break;
						case SIGWINCH:
							query_term_size();
							break;
						default:
							LOG_FMT(LL_ERROR, "%s: BUG: unhandled signal: %d", __func__, ev.val);
						}
						break;
					case EVENT_TYPE_KEY:
						switch (ev.val) {
						case 'h':
							dsp_log_acquire();
							dsp_log_printf("\n%s\n", interactive_help);
							dsp_log_release();
							break;
						case ',':
							pos = do_seek(pos, (ssize_t) input_list.head->codec->fs * -5, SEEK_CUR, is_paused);
							break;
						case '.':
							pos = do_seek(pos, (ssize_t) input_list.head->codec->fs * 5, SEEK_CUR, is_paused);
							break;
						case '<':
							pos = do_seek(pos, (ssize_t) input_list.head->codec->fs * -30, SEEK_CUR, is_paused);
							break;
						case '>':
							pos = do_seek(pos, (ssize_t) input_list.head->codec->fs * 30, SEEK_CUR, is_paused);
							break;
						case 'r':
							pos = do_seek(pos, 0, SEEK_SET, is_paused);
							break;
						case 'n':
							codec_write_buf_drop(out_codec_buf, 1, 0);
							if (xfade_state.pos > 0) finish_xfade();
							reset_effects_chain(&chain);
							goto next_input;
						case 'c':
							is_paused = !is_paused;
							do_pause(is_paused, 0);
							break;
						case 'e': handle_effects_chain_rebuild_request:
							status_ctrl(STATUS_CTRL_CLEAR);
							LOG_S(LL_NORMAL, "info: rebuilding effects chain");
							if (xfade_state.pos > 0) finish_xfade();
							if (!is_paused && !drain_effects) {  /* attempt crossfade */
								stream.fs = input_list.head->codec->fs;
								stream.channels = input_list.head->codec->channels;
								xfade_state.istream = stream;
								xfade_state.chain[0] = chain;
								if (build_effects_chain_from_argv(chain_argc, (const char *const *) &argv[chain_start], &xfade_state.chain[1], &stream, NULL, NULL))
									cleanup_and_exit(1);
								xfade_state.ostream = stream;
								xfade_state.frames = lround((EFFECTS_CHAIN_XFADE_TIME)/1000.0 * stream.fs);
								xfade_state.pos = xfade_state.frames;
								if (xfade_state.pos == 0 || stream.fs != out_codec->fs || stream.channels != out_codec->channels)
									finish_xfade();  /* no crossfade */
							}
							else {
								if (!is_paused) DRAIN_EFFECTS_CHAIN;
								REBUILD_EFFECTS_CHAIN;
							}
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
							if (xfade_state.pos > 0) {
								REALLOC_BUFS(&xfade_state.chain[1]);
								SET_DITHER(&xfade_state.chain[1], input_list.head->codec);
							}
							else {
								REALLOC_BUFS(&chain);
								SET_DITHER(&chain, input_list.head->codec);
							}
							break;
						case 'v':
							verbose_progress = !verbose_progress;
							break;
						case 's':
							signal_effects_chain(&chain);
							break;
						case 'q':
							codec_write_buf_drop(out_codec_buf, 1, 0);
							goto end_rw_loop;
						case '\f':  /* full redraw */
							dsp_log_acquire();
							dsp_log_printf("\033[2J\033[H");
							dsp_log_release();
							print_io_info(input_list.head->codec, LL_NORMAL, "input");
							break;
						}
						break;
					case EVENT_TYPE_CODEC_ERROR:
						cleanup_and_exit(1);
						break;
					default:
						LOG_FMT(LL_ERROR, "%s: BUG: unhandled event type: %d", __func__, (int) ev.type);
					}
					update_progress(pos, repeats, is_paused, 1);
					status_ctrl(STATUS_CTRL_DRAW);
				}
				ssize_t w = r = codec_read_buf_read(in_codec_buf, buf1, block_frames);
				pos = codec_read_buf_get_pos(in_codec_buf);
				const int prev_repeats = repeats;
				repeats = codec_read_buf_get_repeats(in_codec_buf);
				const int did_repeat = (prev_repeats != repeats);
				if (xfade_state.pos > 0) {
					obuf = effects_chain_xfade_run(&xfade_state, &w, buf1, buf2);
					if (xfade_state.pos == 0) {
						finish_xfade();
						LOG_S(LL_VERBOSE, "info: end of crossfade");
					}
				}
				else obuf = run_effects_chain(&chain, &w, buf1, buf2);
				write_out(w, obuf, add_dither);
				k += w;
				if (k >= out_codec->fs || did_repeat) {
					update_progress(pos, repeats, is_paused, did_repeat);
					k -= out_codec->fs;
				}
				status_ctrl(STATUS_CTRL_DRAW);
			} while (r > 0);
			next_input:
			stream.fs = input_list.head->codec->fs;
			stream.channels = input_list.head->codec->channels;
			codec_read_buf_next(in_codec_buf);
			read_buf_input_list_destroy_head(&input_list);
			if (input_list.head != NULL && (input_list.head->codec->fs != stream.fs || input_list.head->codec->channels != stream.channels)) {
				status_ctrl(STATUS_CTRL_CLEAR);
				LOG_S(LL_NORMAL, "info: input sample rate and/or channels changed; rebuilding effects chain");
				if (!is_paused)
					DRAIN_EFFECTS_CHAIN;
				REBUILD_EFFECTS_CHAIN;
				REOPEN_OUTPUT;
				REALLOC_BUFS(&chain);
			}
		}
		DRAIN_EFFECTS_CHAIN;
	}
	end_rw_loop:
	cleanup_and_exit(0);

	got_term_sig:
	status_ctrl(STATUS_CTRL_KEEP);
	LOG_FMT(LL_NORMAL, "info: signal %d: terminating...", term_sig);
	codec_write_buf_drop(out_codec_buf, 1, 0);
	goto end_rw_loop;
}
