#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "crossfeed_hrtf.h"
#include "../codec.h"

struct crossfeed_hrtf_state {
	fftw_complex *filter_fr_left[2];
	fftw_complex *filter_fr_right[2];
	fftw_complex *tmp_fr[2];
	sample_t *impulse[2];
	sample_t *input[2];
	sample_t *output_left_input[2];
	sample_t *output_right_input[2];
	sample_t *overlap_left_input[2];
	sample_t *overlap_right_input[2];
	fftw_plan plan_r2c_left_c0, plan_r2c_left_c1, plan_c2r_left_c0, plan_c2r_left_c1;
	fftw_plan plan_r2c_right_c0, plan_r2c_right_c1, plan_c2r_right_c0, plan_c2r_right_c1;
	int input_frames, fr_frames, buf_pos, frames_read;
};

void crossfeed_hrtf_effect_run(struct effect *e, sample_t s[2])
{
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	int i;

	state->input[0][state->buf_pos] = s[0];
	state->input[1][state->buf_pos] = s[1];

	/* sum left ear output */
	s[0] = state->output_left_input[0][state->buf_pos] + state->output_right_input[0][state->buf_pos];
	/* sum right ear output */
	s[1] = state->output_left_input[1][state->buf_pos] + state->output_right_input[1][state->buf_pos];

	if (state->buf_pos == state->input_frames / 2 - 1) {
		/* left input */
		fftw_execute(state->plan_r2c_left_c0);
		fftw_execute(state->plan_r2c_left_c1);
		for (i = 0; i < state->fr_frames; ++i) {
			/* channel 0 */
			state->tmp_fr[0][i] *= state->filter_fr_left[0][i];
			/* channel 1 */
			state->tmp_fr[1][i] *= state->filter_fr_left[1][i];
		}
		fftw_execute(state->plan_c2r_left_c0);
		fftw_execute(state->plan_c2r_left_c1);
		/* normalize */
		for (i = 0; i < state->input_frames; ++i) {
			state->output_left_input[0][i] /= state->input_frames;
			state->output_left_input[1][i] /= state->input_frames;
		}
		/* handle overlap */
		for (i = 0; i < state->input_frames / 2; ++i) {
			state->output_left_input[0][i] += state->overlap_left_input[0][i];
			state->output_left_input[1][i] += state->overlap_left_input[1][i];
			state->overlap_left_input[0][i] = state->output_left_input[0][i + state->input_frames / 2];
			state->overlap_left_input[1][i] = state->output_left_input[1][i + state->input_frames / 2];
		}

		/* right input */
		fftw_execute(state->plan_r2c_right_c0);
		fftw_execute(state->plan_r2c_right_c1);
		for (i = 0; i < state->fr_frames; ++i) {
			/* channel 0 */
			state->tmp_fr[0][i] *= state->filter_fr_right[0][i];
			/* channel 1 */
			state->tmp_fr[1][i] *= state->filter_fr_right[1][i];
		}
		fftw_execute(state->plan_c2r_right_c0);
		fftw_execute(state->plan_c2r_right_c1);
		/* normalize */
		for (i = 0; i < state->input_frames; ++i) {
			state->output_right_input[0][i] /= state->input_frames;
			state->output_right_input[1][i] /= state->input_frames;
		}
		/* handle overlap */
		for (i = 0; i < state->input_frames / 2; ++i) {
			state->output_right_input[0][i] += state->overlap_right_input[0][i];
			state->output_right_input[1][i] += state->overlap_right_input[1][i];
			state->overlap_right_input[0][i] = state->output_right_input[0][i + state->input_frames / 2];
			state->overlap_right_input[1][i] = state->output_right_input[1][i + state->input_frames / 2];
		}
	}

	++state->frames_read;
	++state->buf_pos;
	state->buf_pos %= state->input_frames / 2;
}

void crossfeed_hrtf_effect_plot(struct effect *e, int i)
{
	printf("H%d(f)=0\n", i); /* fixme */
}

void crossfeed_hrtf_effect_destroy(struct effect *e)
{
	struct crossfeed_hrtf_state *state = (struct crossfeed_hrtf_state *) e->data;
	fftw_free(state->filter_fr_left[0]);
	fftw_free(state->filter_fr_left[1]);
	fftw_free(state->filter_fr_right[0]);
	fftw_free(state->filter_fr_right[1]);
	fftw_free(state->tmp_fr[0]);
	fftw_free(state->tmp_fr[1]);
	free(state->input[0]);
	free(state->input[1]);
	free(state->output_left_input[0]);
	free(state->output_left_input[1]);
	free(state->output_right_input[0]);
	free(state->output_right_input[1]);
	free(state->overlap_left_input[0]);
	free(state->overlap_left_input[1]);
	free(state->overlap_right_input[0]);
	free(state->overlap_right_input[1]);
	fftw_destroy_plan(state->plan_r2c_left_c0);
	fftw_destroy_plan(state->plan_r2c_left_c1);
	fftw_destroy_plan(state->plan_c2r_left_c0);
	fftw_destroy_plan(state->plan_c2r_left_c1);
	fftw_destroy_plan(state->plan_r2c_right_c0);
	fftw_destroy_plan(state->plan_r2c_right_c1);
	fftw_destroy_plan(state->plan_c2r_right_c0);
	fftw_destroy_plan(state->plan_c2r_right_c1);
	free(state);
}

struct effect * crossfeed_hrtf_effect_init(struct effect_info *ei, int argc, char **argv)
{
	struct effect *e;
	struct crossfeed_hrtf_state *state;
	struct codec *c_left, *c_right;
	sample_t *tmp_buf;
	int i;
	size_t frames;
	fftw_plan impulse_plan0, impulse_plan1;
	sample_t *impulse_left[2];
	sample_t *impulse_right[2];

	if (argc != 3) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}
	if (dsp_globals.channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: channels != 2\n", argv[0]);
		return NULL;
	}
	c_left = init_codec(NULL, CODEC_MODE_READ, argv[1], NULL, CODEC_ENDIAN_DEFAULT, 0, 0);
	c_right = init_codec(NULL, CODEC_MODE_READ, argv[2], NULL, CODEC_ENDIAN_DEFAULT, 0, 0);
	if (c_left == NULL || c_right == NULL) {
		LOG(LL_ERROR, "dsp: %s: error: failed to open impulse file: %s\n", argv[0], argv[1]);
		return NULL;
	}
	if (c_left->channels != 2 || c_right->channels != 2) {
		LOG(LL_ERROR, "dsp: %s: error: impulse channels != 2\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	if (c_left->fs != dsp_globals.fs || c_right->fs != dsp_globals.fs) {
		LOG(LL_ERROR, "dsp: %s: error: sample rate mismatch\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	if (c_left->frames <= 1 || c_right->frames <= 1) {
		LOG(LL_ERROR, "dsp: %s: error: impulse length must > 1 sample\n", argv[0]);
		destroy_codec(c_left);
		destroy_codec(c_right);
		return NULL;
	}
	frames = (c_left->frames > c_right->frames) ? c_left->frames : c_right->frames;

	e = calloc(1, sizeof(struct effect));
	e->run = crossfeed_hrtf_effect_run;
	e->plot = crossfeed_hrtf_effect_plot;
	e->destroy = crossfeed_hrtf_effect_destroy;

	state = calloc(1, sizeof(struct crossfeed_hrtf_state));
	state->input_frames = (frames - 1) * 2;
	state->fr_frames = frames;
	state->filter_fr_left[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_left[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_right[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->filter_fr_right[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->tmp_fr[0] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->tmp_fr[1] = fftw_malloc(state->fr_frames * sizeof(fftw_complex));
	state->input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->output_left_input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->output_left_input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->output_right_input[0] = calloc(state->input_frames, sizeof(sample_t));
	state->output_right_input[1] = calloc(state->input_frames, sizeof(sample_t));
	state->overlap_left_input[0] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_left_input[1] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_right_input[0] = calloc(state->input_frames / 2, sizeof(sample_t));
	state->overlap_right_input[1] = calloc(state->input_frames / 2, sizeof(sample_t));

	impulse_left[0] = calloc(state->input_frames, sizeof(sample_t));
	impulse_left[1] = calloc(state->input_frames, sizeof(sample_t));
	impulse_right[0] = calloc(state->input_frames, sizeof(sample_t));
	impulse_right[1] = calloc(state->input_frames, sizeof(sample_t));

	tmp_buf = calloc(frames * 2, sizeof(sample_t));
	if (c_left->read(c_left, tmp_buf, c_left->frames) != c_left->frames)
		LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	for (i = 0; i < c_left->frames; ++i) {
		impulse_left[0][i] = tmp_buf[i * 2];
		impulse_left[1][i] = tmp_buf[i * 2 + 1];
	}
	memset(tmp_buf, 0, frames * 2 * sizeof(sample_t));
	if (c_right->read(c_right, tmp_buf, c_right->frames) != c_right->frames)
		LOG(LL_ERROR, "dsp: %s: warning: short read\n", argv[0]);
	for (i = 0; i < c_right->frames; ++i) {
		impulse_right[0][i] = tmp_buf[i * 2];
		impulse_right[1][i] = tmp_buf[i * 2 + 1];
	}
	free(tmp_buf);

	impulse_plan0 = fftw_plan_dft_r2c_1d(state->input_frames, impulse_left[0], state->filter_fr_left[0], FFTW_ESTIMATE);
	impulse_plan1 = fftw_plan_dft_r2c_1d(state->input_frames, impulse_left[1], state->filter_fr_left[1], FFTW_ESTIMATE);
	fftw_execute(impulse_plan0);
	fftw_execute(impulse_plan1);
	fftw_destroy_plan(impulse_plan0);
	fftw_destroy_plan(impulse_plan1);

	impulse_plan0 = fftw_plan_dft_r2c_1d(state->input_frames, impulse_right[0], state->filter_fr_right[0], FFTW_ESTIMATE);
	impulse_plan1 = fftw_plan_dft_r2c_1d(state->input_frames, impulse_right[1], state->filter_fr_right[1], FFTW_ESTIMATE);
	fftw_execute(impulse_plan0);
	fftw_execute(impulse_plan1);
	fftw_destroy_plan(impulse_plan0);
	fftw_destroy_plan(impulse_plan1);

	/* init left input plans */
	state->plan_r2c_left_c0 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[0], state->tmp_fr[0], FFTW_ESTIMATE);
	state->plan_r2c_left_c1 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[0], state->tmp_fr[1], FFTW_ESTIMATE);
	state->plan_c2r_left_c0 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[0], state->output_left_input[0], FFTW_ESTIMATE);
	state->plan_c2r_left_c1 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[1], state->output_left_input[1], FFTW_ESTIMATE);

	/* init right input plans */
	state->plan_r2c_right_c0 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[1], state->tmp_fr[0], FFTW_ESTIMATE);
	state->plan_r2c_right_c1 = fftw_plan_dft_r2c_1d(state->input_frames, state->input[1], state->tmp_fr[1], FFTW_ESTIMATE);
	state->plan_c2r_right_c0 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[0], state->output_right_input[0], FFTW_ESTIMATE);
	state->plan_c2r_right_c1 = fftw_plan_dft_c2r_1d(state->input_frames, state->tmp_fr[1], state->output_right_input[1], FFTW_ESTIMATE);

	LOG(LL_VERBOSE, "dsp: %s: impulse frames=%zu\n", argv[0], frames);
	destroy_codec(c_left);
	destroy_codec(c_right);
	free(impulse_left[0]);
	free(impulse_left[1]);
	free(impulse_right[0]);
	free(impulse_right[1]);

	e->data = state;
	return e;
}
