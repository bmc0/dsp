#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "resample.h"
#include "util.h"

/* Tunables */
static const double default_bw = 0.95;  /* default bandwidth */
static const double m_fact = 8;         /* controls window size; 6 for Blackman window, 8 for Nuttall and Blackman-Nuttall window */

struct resample_state {
	struct {
		int n, d;
	} ratio;
	ssize_t m, sinc_len, sinc_fr_len, tmp_fr_len, in_len, out_len, in_buf_pos, out_buf_pos, drain_pos, drain_frames, out_delay;
	fftw_complex *sinc_fr;
	fftw_complex *tmp_fr;
	sample_t **input, **output, **overlap;
	fftw_plan *r2c_plan, *c2r_plan;
	int has_output, is_draining;
};

void resample_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct resample_state *state = (struct resample_state *) e->data;
	ssize_t i, k, iframes = 0, oframes = 0, max_oframes = (double) *frames * e->worst_case_ratio;

	while (iframes < *frames) {
		while (state->in_buf_pos < state->in_len && iframes < *frames) {
			for (i = 0; i < e->ostream.channels; ++i)
				state->input[i][state->in_buf_pos] = (ibuf) ? ibuf[iframes * e->ostream.channels + i] : 0;
			++iframes;
			++state->in_buf_pos;
		}

		while (state->out_buf_pos < state->out_len && oframes < max_oframes && state->has_output) {
			for (i = 0; i < e->ostream.channels; ++i)
				obuf[oframes * e->ostream.channels + i] = state->output[i][state->out_buf_pos];
			++oframes;
			++state->out_buf_pos;
		}

		if (state->in_buf_pos == state->in_len && (!state->has_output || state->out_buf_pos == state->out_len)) {
			for (i = 0; i < e->ostream.channels; ++i) {
				/* FFT(state->input[i]) -> state->tmp_fr */
				fftw_execute(state->r2c_plan[i]);
				/* convolve input with sinc filter */
				for (k = 0; k < state->sinc_fr_len; ++k)
					state->tmp_fr[k] *= state->sinc_fr[k];
				/* IFFT(state->tmp_fr) -> state->output[i] */
				fftw_execute(state->c2r_plan[i]);
				/* normalize */
				for (k = 0; k < state->out_len * 2; ++k)
					state->output[i][k] /= state->in_len * 2;
				/* handle overlap */
				for (k = 0; k < state->out_len; ++k) {
					state->output[i][k] += state->overlap[i][k];
					state->overlap[i][k] = state->output[i][k + state->out_len];
				}
			}
			state->in_buf_pos = state->out_buf_pos = 0;
			if (state->has_output == 0) {
				state->out_buf_pos = state->out_delay;
				state->has_output = 1;
			}
		}
	}
	*frames = oframes;
}

void resample_copy_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	memcpy(obuf, ibuf, *frames * e->ostream.channels * sizeof(sample_t));
}

ssize_t resample_effect_delay(struct effect *e)
{
	ssize_t frames = 0;
	struct resample_state *state = (struct resample_state *) e->data;
	if (state->has_output) {
		frames += state->out_delay;  /* filter delay */
		frames += state->out_len - state->out_buf_pos;  /* pending output frames */
	}
	frames += state->in_buf_pos * state->ratio.n / state->ratio.d;  /* pending input frames */
	return frames;
}

void resample_effect_reset(struct effect *e)
{
	int i;
	struct resample_state *state = (struct resample_state *) e->data;
	state->in_buf_pos = state->out_buf_pos = 0;
	state->has_output = 0;
	for (i = 0; i < e->ostream.channels; ++i)
		memset(state->overlap[i], 0, state->out_len * sizeof(sample_t));
}

void resample_effect_drain(struct effect *e, ssize_t *frames, sample_t *obuf)
{
	struct resample_state *state = (struct resample_state *) e->data;
	if (!state->has_output && state->in_buf_pos == 0)
		*frames = -1;
	else {
		if (!state->is_draining) {
			if (state->has_output) {
				state->drain_frames += state->out_delay;  /* filter delay */
				state->drain_frames += state->out_len - state->out_buf_pos;  /* pending output frames */
			}
			state->drain_frames += state->in_buf_pos * state->ratio.n / state->ratio.d;  /* pending input frames */
			state->is_draining = 1;
		}
		if (state->drain_pos < state->drain_frames) {
			resample_effect_run(e, frames, NULL, obuf);
			state->drain_pos += *frames;
			*frames -= (state->drain_pos > state->drain_frames) ? state->drain_pos - state->drain_frames : 0;
		}
		else
			*frames = -1;
	}
}

void resample_effect_destroy(struct effect *e)
{
	int i;
	struct resample_state *state = (struct resample_state *) e->data;
	fftw_free(state->sinc_fr);
	fftw_free(state->tmp_fr);
	for (i = 0; i < e->ostream.channels; ++i) {
		fftw_free(state->input[i]);
		fftw_free(state->output[i]);
		fftw_free(state->overlap[i]);
		fftw_destroy_plan(state->r2c_plan[i]);
		fftw_destroy_plan(state->c2r_plan[i]);
	}
	free(state->input);
	free(state->output);
	free(state->overlap);
	free(state->r2c_plan);
	free(state->c2r_plan);
	free(state);
}

struct effect * resample_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct resample_state *state;
	int rate, max_rate, min_rate, max_factor, gcd, rem, i;
	double bw = default_bw;
	sample_t *sinc, width, fc, m;
	fftw_plan sinc_plan;

	if (argc < 2 || argc > 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (argc == 3) {
		bw = atof(argv[1]);
		rate = lround(parse_freq(argv[2]));
	}
	else
		rate = lround(parse_freq(argv[1]));
	CHECK_RANGE(bw > 0 && bw < 1, "bandwidth", return NULL);
	CHECK_RANGE(rate > 0, "rate", return NULL);

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = istream->fs;
	e->ostream.fs = rate;
	e->istream.channels = e->ostream.channels = istream->channels;

	if (rate == istream->fs) {
		e->worst_case_ratio = e->ratio = 1.0;
		e->run = resample_copy_effect_run;
		LOG(LL_VERBOSE, "dsp: %s: info: sample rates match; no proccessing will be done\n", argv[0]);
		return e;
	}

	e->ratio = (double) rate / istream->fs;
	e->worst_case_ratio = ceil((double) rate / istream->fs);
	e->run = resample_effect_run;
	e->delay = resample_effect_delay;
	e->reset = resample_effect_reset;
	e->drain = resample_effect_drain;
	e->destroy = resample_effect_destroy;

	state = calloc(1, sizeof(struct resample_state));
	e->data = state;

	/* find greatest common divisor using Euclid's algorithm */
	gcd = rate;
	rem = istream->fs;
	do {
		i = rem;
		rem = gcd % rem;
		gcd = i;
	} while (rem != 0);

	max_rate = MAXIMUM(rate, istream->fs);
	min_rate = MINIMUM(rate, istream->fs);
	state->ratio.n = rate / gcd;
	state->ratio.d = istream->fs / gcd;
	max_factor = MAXIMUM(state->ratio.n, state->ratio.d);

	/* calulate params for windowed sinc function */
	width = (min_rate - min_rate * bw) / 2;
	fc = (min_rate - width) / max_rate;
	m = round(m_fact / (width / max_rate));

	/* determine array lengths */
	state->m = (size_t) (m + 1) * 2 - 1;  /* final impulse length after convolving sinc function with itself */
	i = ceil((double) state->m / max_factor);  /* calculate multiplier */
	state->sinc_len = max_factor * i;
	state->in_len = state->ratio.d * i;
	state->out_len = state->ratio.n * i;
	state->tmp_fr_len = state->sinc_fr_len = state->sinc_len + 1;

	/* calculate output delay */
	if (rate == max_rate)
		state->out_delay = state->m / 2;
	else
		state->out_delay = lround((double) state->m / 2 * state->ratio.n / state->ratio.d);

	/* allocate arrays, construct fftw plans */
	sinc = fftw_malloc(state->sinc_len * 2 * sizeof(sample_t));
	memset(sinc, 0, state->sinc_len * 2 * sizeof(sample_t));
	state->sinc_fr = fftw_malloc(state->sinc_fr_len * sizeof(fftw_complex));
	memset(state->sinc_fr, 0, state->sinc_fr_len * sizeof(fftw_complex));
	sinc_plan = fftw_plan_dft_r2c_1d(state->sinc_len * 2, sinc, state->sinc_fr, FFTW_ESTIMATE);

	state->tmp_fr = fftw_malloc(state->tmp_fr_len * sizeof(fftw_complex));
	memset(state->tmp_fr, 0, state->tmp_fr_len * sizeof(fftw_complex));
	state->input = calloc(e->ostream.channels, sizeof(sample_t *));
	state->output = calloc(e->ostream.channels, sizeof(sample_t *));
	state->overlap = calloc(e->ostream.channels, sizeof(sample_t *));
	state->r2c_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
	state->c2r_plan = calloc(e->ostream.channels, sizeof(fftw_plan));
	for (i = 0; i < e->ostream.channels; ++i) {
		state->input[i] = fftw_malloc(state->in_len * 2 * sizeof(sample_t));
		memset(state->input[i], 0, state->in_len * 2 * sizeof(sample_t));
		state->output[i] = fftw_malloc(state->out_len * 2 * sizeof(sample_t));
		memset(state->output[i], 0, state->out_len * 2 * sizeof(sample_t));
		state->overlap[i] = fftw_malloc(state->out_len * sizeof(sample_t));
		memset(state->overlap[i], 0, state->out_len * sizeof(sample_t));
		state->r2c_plan[i] = fftw_plan_dft_r2c_1d(state->in_len * 2, state->input[i], state->tmp_fr, FFTW_ESTIMATE);
		state->c2r_plan[i] = fftw_plan_dft_c2r_1d(state->out_len * 2, state->tmp_fr, state->output[i], FFTW_ESTIMATE);
	}

	/* generate windowed sinc function */
	for (i = 0; i < (int) m + 1; ++i) {
		/* calculate scaled sinc() value */
		if ((double) i == m / 2)
			sinc[i] = fc;
		else
			sinc[i] = sin(M_PI * fc * (i - m / 2)) / (M_PI * (i - m / 2));
		/* apply Blackman window (~75dB stopband attenuation) */
		/* sinc[i] *= 0.42 - 0.5 * cos(2 * M_PI * i / m) + 0.08 * cos(4 * M_PI * i / m); */
		/* apply Nuttall window (continuous first derivative) (~112dB stopband attenuation) */
		sinc[i] *= 0.355768 - 0.487396 * cos(2 * M_PI * i / m) + 0.144232 * cos(4 * M_PI * i / m) - 0.012604 * cos(6 * M_PI * i / m);
		/* apply Blackman-Nuttall window (~114dB stopband attenuation) */
		/* sinc[i] *= 0.3635819 - 0.4891775 * cos(2 * M_PI * i / m) + 0.1365995 * cos(4 * M_PI * i / m) - 0.0106411 * cos(6 * M_PI * i / m); */
	}

	fftw_execute(sinc_plan);
	fftw_destroy_plan(sinc_plan);
	fftw_free(sinc);

	/* convolve sinc function with itself (doubles stopband attenuation) */
	for (i = 0; i < state->sinc_fr_len; ++i)
		state->sinc_fr[i] *= state->sinc_fr[i];

	LOG(LL_VERBOSE, "dsp: %s: info: gcd=%d ratio=%d/%d width=%fHz fc=%f filter_len=%zd in_len=%zd out_len=%zd\n",
		argv[0], gcd, state->ratio.n, state->ratio.d, width, fc, state->m, state->in_len, state->out_len);

	return e;
}
