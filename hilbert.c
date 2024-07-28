#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "hilbert.h"
#include "fir.h"
#include "fir_p.h"
#include "util.h"

struct effect * hilbert_effect_init(struct effect_info *ei, struct stream_info *istream, char *channel_selector, const char *dir, int argc, char **argv)
{
	ssize_t taps, i = 1, k;
	sample_t *h;
	char *endptr;
	int use_fir_p = 0;
	struct effect *e;

	if (argc > 3 || argc < 2) {
		LOG_FMT(LL_ERROR, "%s: usage: %s", argv[0], ei->usage);
		return NULL;
	}
	if (strcmp(argv[i], "-p") == 0) {
		use_fir_p = 1;
		++i;
	}
	taps = strtol(argv[i], &endptr, 10);
	CHECK_ENDPTR(argv[i], endptr, "taps", return NULL);
	if (taps < 3) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be > 3", argv[0]);
		return NULL;
	}
	if (taps%2 == 0) {
		LOG_FMT(LL_ERROR, "%s: error: taps must be odd", argv[0]);
		return NULL;
	}
	h = calloc(taps, sizeof(sample_t));
	for (i = 0, k = -taps/2; i < taps; ++i, ++k) {
		if (k%2 == 0)
			h[i] = 0;
		else {
			double x = 2.0*M_PI*i/(taps-1);
			h[i] = 2.0/(M_PI*k) * (0.42 - 0.5*cos(x) + 0.08*cos(2.0*x));
		}
	}
	e = (use_fir_p) ?
		fir_p_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0) :
		fir_effect_init_with_filter(ei, istream, channel_selector, h, 1, taps, 0);
	free(h);
	return e;
}
