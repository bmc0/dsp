#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "stats.h"
#include "util.h"

struct stats_state {
	ssize_t samples, peak_count, peak_frame;
	sample_t sum, sum_sq, min, max, ref;
};

void stats_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t i, k, samples = *frames * e->ostream.channels;
	struct stats_state *state = (struct stats_state *) e->data;
	for (i = 0; i < samples; i += e->ostream.channels) {
		for (k = 0; k < e->ostream.channels; ++k) {
			++state[k].samples;
			state[k].sum += ibuf[i + k];
			state[k].sum_sq += ibuf[i + k] * ibuf[i + k];
			if (ibuf[i + k] < state[k].min) {
				state[k].min = ibuf[i + k];
				state[k].peak_count = 0;
			}
			if (ibuf[i + k] > state[k].max) {
				state[k].max = ibuf[i + k];
				state[k].peak_count = 0;
			}
			if (ibuf[i + k] == state[k].min || ibuf[i + k] == state[k].max)
				++state[k].peak_count;
			if (fabs(ibuf[i + k]) >= MAXIMUM(fabs(state[k].max), fabs(state[k].min)))
				state[k].peak_frame = state[k].samples;
			obuf[i + k] = ibuf[i + k];
		}
	}
}

void stats_effect_plot(struct effect *e, int i)
{
	int k;
	for (k = 0; k < e->ostream.channels; ++k)
		printf("H%d_%d(f)=0\n", k, i);
}

void stats_effect_destroy(struct effect *e)
{
	ssize_t i;
	struct stats_state *state = (struct stats_state *) e->data;
	fprintf(stderr, "\n%-18s", "Channel");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12zd", i);
	fprintf(stderr, "\n%-18s", "DC offset");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.8f", state[i].sum / state[i].samples);
	fprintf(stderr, "\n%-18s", "Minimum");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.8f", state[i].min);
	fprintf(stderr, "\n%-18s", "Maximum");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.8f", state[i].max);
	fprintf(stderr, "\n%-18s", "Peak level (dBFS)");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.4f", 20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max))));
	if (state->ref != -HUGE_VAL) {
		fprintf(stderr, "\n%-18s", "Peak level (dBr)");
		for (i = 0; i < e->ostream.channels; ++i)
			fprintf(stderr, " %12.4f", state->ref + (20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max)))));
	}
	fprintf(stderr, "\n%-18s", "RMS level (dBFS)");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.4f", 20 * log10(sqrt(state[i].sum_sq / state[i].samples)));
	if (state->ref != -HUGE_VAL) {
		fprintf(stderr, "\n%-18s", "RMS level (dBr)");
		for (i = 0; i < e->ostream.channels; ++i)
			fprintf(stderr, " %12.4f", state->ref + (20 * log10(sqrt(state[i].sum_sq / state[i].samples))));
	}
	fprintf(stderr, "\n%-18s", "Crest factor (dB)");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.4f", 20 * log10(MAXIMUM(fabs(state[i].min), fabs(state[i].max)) / sqrt(state[i].sum_sq / state[i].samples)));
	fprintf(stderr, "\n%-18s", "Peak count");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12zd", state[i].peak_count);
	fprintf(stderr, "\n%-18s", "Peak sample");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12zd", state[i].peak_frame);
	fprintf(stderr, "\n%-18s", "Samples");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12zd", state[i].samples);
	fprintf(stderr, "\n%-18s", "Length (s)");
	for (i = 0; i < e->ostream.channels; ++i)
		fprintf(stderr, " %12.2f", (double) state[i].samples / e->ostream.fs);
	fputc('\n', stderr);
	free(state);
}

struct effect * stats_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	struct effect *e;
	struct stats_state *state;
	sample_t ref = -HUGE_VAL;

	if (argc == 2)
		ref = atof(argv[1]);
	else if (argc != 1) {
		LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
		return NULL;
	}

	e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->worst_case_ratio = e->ratio = 1.0;
	e->run = stats_effect_run;
	e->plot = stats_effect_plot;
	e->destroy = stats_effect_destroy;
	state = calloc(istream->channels, sizeof(struct stats_state));
	state->ref = ref;
	e->data = state;
	return e;
}
