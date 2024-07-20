#include <stdlib.h>
#include <string.h>
#include <math.h>
#define _MATRIX4_COMMON_H_NO_STATIC_FUNCTIONS
#include "matrix4_common.h"
#include "dsp.h"

int get_args_and_channels(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, int argc, const char *const *argv, struct matrix4_config *config)
{
	double surr_level = -6.0206;
	char *endptr;
	if (argc > 3) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return 1;
	}
	config->opt_str_idx = -1;
	if (argc == 2) {
		const double val = strtod(argv[1], &endptr);
		if (endptr == argv[1] || *endptr != '\0')
			config->opt_str_idx = 1;
		else
			surr_level = val;
	}
	else if (argc == 3) {
		config->opt_str_idx = 1;
		surr_level = strtod(argv[2], &endptr);
		CHECK_ENDPTR(argv[2], endptr, "surround_level", return 1);
	}
	config->surr_mult = pow(10.0, surr_level / 20.0);
	if (config->surr_mult > 1.0)
		LOG_FMT(LL_ERROR, "%s: warning: surround_level probably shouldn't be greater than 0dB", argv[0]);

	if (istream->fs < 32000) {
		LOG_FMT(LL_ERROR, "%s: error: sample rate out of range", argv[0]);
		return 1;
	}
	for (int i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i))
			++config->n_channels;
	}
	if (config->n_channels != 2) {
		LOG_FMT(LL_ERROR, "%s: error: number of input channels must be 2", argv[0]);
		return 1;
	}
	config->c0 = config->c1 = -1;
	for (int i = 0; i < istream->channels; ++i) {
		if (GET_BIT(channel_selector, i)) {
			if (config->c0 == -1)
				config->c0 = i;
			else
				config->c1 = i;
		}
	}
	return 0;
}

static int is_opt(const char *opt, const char *opt_name)
{
	size_t len = strlen(opt_name);
	if (len > 1 && opt_name[len-1] == '=')
		return strncmp(opt, opt_name, len-1) == 0 && (strlen(opt) == len-1 || opt[len-1] == '=');
	return strcmp(opt, opt_name) == 0;
}

int parse_effect_opts(const char *const *argv, const struct stream_info *istream, struct matrix4_config *config)
{
	char *opt_str = NULL;
	config->do_dir_boost = 1;
	if (config->opt_str_idx > 0) {
		opt_str = strdup(argv[config->opt_str_idx]);
		char *opt = opt_str, *next_opt, *opt_arg, *endptr;
		while (*opt != '\0') {
			next_opt = isolate(opt, ',');
			if (*opt == '\0') /* do nothing */;
			else if (is_opt(opt, "show_status"))   config->show_status = 1;
			else if (is_opt(opt, "no_dir_boost"))  config->do_dir_boost = 0;
			else if (is_opt(opt, "signal"))        config->enable_signal = 1;
			else if (is_opt(opt, "linear_phase"))  config->do_phase_lin = 1;
			else if (is_opt(opt, "surround_delay=")) {
				opt_arg = isolate(opt, '=');
				if (*opt_arg == '\0') { LOG_FMT(LL_ERROR, "%s: error: option requires argument: %s", argv[0], opt); goto fail; }
				config->surr_delay_frames = parse_len(opt_arg, istream->fs, &endptr);
				CHECK_ENDPTR(opt_arg, endptr, "surround_delay", goto fail);
			}
			else {
				LOG_FMT(LL_ERROR, "%s: error: unrecognized option: %s", argv[0], opt);
				goto fail;
			}
			opt = next_opt;
		}
		free(opt_str);
	}
	return 0;

	fail:
	free(opt_str);
	return 1;
}

struct matrix4_delay_state {
	char has_output, is_draining;
	int n_ch;
	sample_t *buf;
	ssize_t len, p, drain_frames;
};

sample_t * matrix4_delay_surr_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	sample_t *ibuf_p = &ibuf[e->istream.channels-2];
	for (ssize_t i = 0; i < *frames; ++i) {
		sample_t *b = &state->buf[state->p*2];
		const sample_t s0 = ibuf_p[0], s1 = ibuf_p[1];
		ibuf_p[0] = b[0];
		ibuf_p[1] = b[1];
		b[0] = s0;
		b[1] = s1;
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
		ibuf_p += e->istream.channels;
	}
	return ibuf;
}

sample_t * matrix4_delay_front_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t oframes = 0;
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	for (ssize_t i = 0; i < *frames; ++i) {
		int k;
		sample_t *b = &state->buf[state->p*state->n_ch];
		sample_t *ibuf_p = &ibuf[i*e->istream.channels];
		sample_t *obuf_p = &obuf[oframes*e->istream.channels];
		if (state->has_output) {
			for (k = 0; k < state->n_ch; ++k) {
				obuf_p[k] = b[k];
				b[k] = (ibuf) ? ibuf_p[k] : 0.0;
			}
			obuf_p[k+0] = (ibuf) ? ibuf_p[k+0] : 0.0;
			obuf_p[k+1] = (ibuf) ? ibuf_p[k+1] : 0.0;
			++oframes;
		}
		else {
			for (k = 0; k < state->n_ch; ++k) {
				b[k] = (ibuf) ? ibuf_p[k] : 0.0;
			#ifdef SYMMETRIC_IO
				obuf_p[k] = 0.0;
			#endif
			}
		#ifdef SYMMETRIC_IO
			obuf_p[k+0] = 0.0;
			obuf_p[k+1] = 0.0;
		#endif
		}
		state->p = (state->p + 1 >= state->len) ? 0 : state->p + 1;
		if (state->p == 0)
			state->has_output = 1;
	}
	*frames = oframes;
	return obuf;
}

ssize_t matrix4_delay_front_effect_delay(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	return (state->has_output) ? state->len : state->p;
}

void matrix4_delay_front_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	if (!state->has_output && state->p == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			state->drain_frames = state->len;
			state->is_draining = 1;
		}
		if (state->drain_frames > 0) {
			*frames = MINIMUM(*frames, state->drain_frames);
			state->drain_frames -= *frames;
			e->run(e, frames, NULL, obuf);
		}
		else
			*frames = -1;
	}
}

void matrix4_delay_effect_reset(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	state->p = 0;
	state->has_output = 0;
	memset(state->buf, 0, state->len * state->n_ch * sizeof(sample_t));
}

void matrix4_delay_effect_destroy(struct effect *e)
{
	struct matrix4_delay_state *state = (struct matrix4_delay_state *) e->data;
	free(state->buf);
	free(state);
}

struct effect * matrix4_delay_effect_init(const struct effect_info *ei, const struct stream_info *istream, ssize_t frames)
{
	if (frames == 0)
		return NULL;

	LOG_FMT(LL_VERBOSE, "%s: info: net surround delay is %gms (%zd sample%s)", ei->name, frames*1000.0/istream->fs, frames, (frames == 1) ? "" : "s");
	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	if (frames > 0) {
		e->run = matrix4_delay_surr_effect_run;
	}
	else {
		e->run = matrix4_delay_front_effect_run;
		e->delay = matrix4_delay_front_effect_delay;
		e->drain = matrix4_delay_front_effect_drain;
	}
	e->reset = matrix4_delay_effect_reset;
	e->destroy = matrix4_delay_effect_destroy;

	struct matrix4_delay_state *state = calloc(1, sizeof(struct matrix4_delay_state));
	state->len = (frames < 0) ? -frames : frames;
	state->n_ch = (frames > 0) ? 2 : e->istream.channels - 2;
	state->buf = calloc(state->len * state->n_ch, sizeof(sample_t));

	e->data = state;
	return e;
}
