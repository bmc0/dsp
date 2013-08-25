#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "biquad.h"
#include "../util.h"

#define CHECK_RANGE(cond, name) \
	if (!(cond)) { \
		LOG(LL_ERROR, "dsp: %s: error: %s out of range\n", argv[0], name); \
		return NULL; \
	}

void biquad_init(struct biquad_state *state, double b0, double b1, double b2, double a0, double a1, double a2)
{
	state->c0 = b0 / a0;
	state->c1 = b1 / a0;
	state->c2 = b2 / a0;
	state->c3 = a1 / a0;
	state->c4 = a2 / a0;

	state->x[0] = state->x[1] = 0.0;
	state->y[0] = state->y[1] = 0.0;
}

void biquad_init_using_type(struct biquad_state *b, int type, double fs, double arg0, double arg1, double arg2, double arg3)
{
	double b0, b1, b2, a0, a1, a2;
	double f0, q, gain, a, w0, sin_w0, cos_w0, alpha, c;
	double fz, qz, fp, qp, fc, d0i, d1i, d2i, c0i, c1i, c2i, gn, cci;

	if (type == BIQUAD_LINKWITZ_TRANSFORM) {
		fz = arg0;
		qz = arg1;
		fp = arg2;
		qp = arg3;
		
		fc = (fz + fp) / 2.0;

		d0i = pow(2.0 * M_PI * fz, 2.0);
		d1i = (2.0 * M_PI * fz) / qz;
		d2i = 1;

		c0i = pow(2.0 * M_PI * fp, 2.0);
		c1i = (2.0 * M_PI * fp) / qp;
		c2i = 1;

		gn = (2.0 * M_PI * fc) / tan(M_PI * fc / dsp_globals.fs);
		cci = c0i + gn * c1i + pow(gn, 2.0) * c2i;
		
		b0 = (d0i + gn * d1i + pow(gn, 2.0) * d2i) / cci;
		b1 = 2 * (d0i - pow(gn, 2.0) * d2i) / cci;
		b2 = (d0i - gn * d1i + pow(gn, 2.0) * d2i) / cci;
		a0 = 1;
		a1 = (2.0 * (c0i - pow(gn, 2.0) * c2i) / cci);
		a2 = ((c0i - gn * c1i + pow(gn, 2.0) * c2i) / cci);
	}
	else {
		f0 = arg0;
		q = arg1;
		gain = arg2;

		a = pow(10.0, gain / 40.0);
		w0 = 2 * M_PI * f0 / fs;
		sin_w0 = sin(w0);
		cos_w0 = cos(w0);
		alpha = sin_w0 / (2.0 * q);

		switch (type) {
			case BIQUAD_LOWPASS_1:
				a0 = 1.0;
				a1 = -exp(-w0);
				a2 = 0.0;
				b0 = 1.0 + a1;
				b1 = b2 = 0.0;
				break;
			case BIQUAD_HIGHPASS_1:
				a0 = 1.0;
				a1 = -exp(-w0);
				a2 = 0.0;
				b0 = (1.0 - a1) / 2.0;
				b1 = -b0;
				b2 = 0.0;
				break;
			case BIQUAD_LOWPASS:
				b0 = (1.0 - cos_w0) / 2.0;
				b1 = 1.0 - cos_w0;
				b2 = b0;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cos_w0;
				a2 = 1.0 - alpha;
				break;
			case BIQUAD_HIGHPASS:
				b0 = (1.0 + cos_w0) / 2.0;
				b1 = -(1.0 + cos_w0);
				b2 = b0;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cos_w0;
				a2 = 1.0 - alpha;
				break;
			case BIQUAD_BANDPASS_SKIRT:
				b0 = sin_w0 / 2.0;
				b1 = 0.0;
				b2 = -b0;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cos_w0;
				a2 = 1.0 - alpha;
				break;
			case BIQUAD_BANDPASS_PEAK:
				b0 = alpha;
				b1 = 0.0;
				b2 = -alpha;
				a0 = 1.0 + alpha;
				a1 = -2.0 * cos_w0;
				a2 = 1.0 - alpha;
				break;
			case BIQUAD_NOTCH:
				b0 = 1.0;
				b1 = -2.0 * cos_w0;
				b2 = 1.0;
				a0 = 1.0 + alpha;
				a1 = b1;
				a2 = 1.0 - alpha;
				break;
			case BIQUAD_ALLPASS:
				b0 = 1.0 - alpha;
				b1 = -2.0 * cos_w0;
				b2 = 1.0 + alpha;
				a0 = b2;
				a1 = b1;
				a2 = b0;
				break;
			case BIQUAD_PEAK:
				b0 = 1.0 + alpha * a;
				b1 = -2.0 * cos_w0;
				b2 = 1.0 - alpha * a;
				a0 = 1.0 + alpha / a;
				a1 = b1;
				a2 = 1.0 - alpha / a;
				break;
			case BIQUAD_LOWSHELF:
				c = 2.0 * sqrt(a) * alpha;
				b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + c);
				b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0);
				b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - c);
				a0 = (a + 1.0) + (a - 1.0) * cos_w0 + c;
				a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0);
				a2 = (a + 1.0) + (a - 1.0) * cos_w0 - c;
				break;
			case BIQUAD_HIGHSHELF:
				c = 2.0 * sqrt(a) * alpha;
				b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + c);
				b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0);
				b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - c);
				a0 = (a + 1.0) - (a - 1.0) * cos_w0 + c;
				a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0);
				a2 = (a + 1.0) - (a - 1.0) * cos_w0 - c;
				break;
			default:
				/* do nothing */
				b0 = 1.0;
				b1 = 0.0;
				b2 = 0.0;
				a0 = 1.0;
				a1 = 0.0;
				a2 = 0.0;
		}
	}
	biquad_init(b, b0, b1, b2, a0, a1, a2);
}

sample_t biquad(struct biquad_state *state, sample_t s)
{
	sample_t r = (state->c0 * s) + (state->c1 * state->x[0]) + (state->c2 * state->x[1]) - (state->c3 * state->y[0]) - (state->c4 * state->y[1]);

	state->x[1] = state->x[0];
	state->x[0] = s;

	state->y[1] = state->y[0];
	state->y[0] = r;

	return r;
}

void biquad_effect_run(struct effect *e, sample_t *s)
{
	int i;
	struct biquad_state *state = (struct biquad_state *) e->data;
	for (i = 0; i < dsp_globals.channels; ++i)
		s[i] = biquad(&state[i], s[i]);
}

void biquad_effect_plot(struct effect *e, int i)
{
	struct biquad_state *state = (struct biquad_state *) e->data;
	printf(
		"c%d0=%.15e; c%d1=%.15e; c%d2=%.15e; c%d3=%.15e; c%d4=%.15e\n"
		"H%d(f)=20*log10(sqrt((c%d0*c%d0+c%d1*c%d1+c%d2*c%d2+2.*(c%d0*c%d1+c%d1*c%d2)*cos(f*o)+2.*(c%d0*c%d2)*cos(2.*f*o))/(1.+c%d3*c%d3+c%d4*c%d4+2.*(c%d3+c%d3*c%d4)*cos(f*o)+2.*c%d4*cos(2.*f*o))))\n",
		i, state->c0, i, state->c1, i, state->c2, i, state->c3, i, state->c4, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i
	);
}

void biquad_effect_destroy(struct effect *e)
{
	free(e->data);
}

struct effect * biquad_effect_init(struct effect_info *ei, int argc, char **argv)
{
	int i, type;
	double arg0 = 0, arg1 = 0, arg2 = 0, arg3 = 0;
	double b0 = 0, b1 = 0, b2 = 0, a0 = 0, a1 = 0, a2 = 0;
	struct biquad_state *state;
	struct effect *e;

	if (strcmp(argv[0], "lowpass_1") == 0) {
		if (argc != 2) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_LOWPASS_1;
		arg0 = parse_freq(argv[1]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
	}
	else if (strcmp(argv[0], "highpass_1") == 0) {
		if (argc != 2) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_HIGHPASS_1;
		arg0 = parse_freq(argv[1]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
	}
	else if (strcmp(argv[0], "lowpass") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_LOWPASS;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "highpass") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_HIGHPASS;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "bandpass_skirt") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_BANDPASS_SKIRT;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "bandpass_peak") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_BANDPASS_PEAK;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "notch") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_NOTCH;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "allpass") == 0) {
		if (argc != 3) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_ALLPASS;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "eq") == 0) {
		if (argc != 4) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_PEAK;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		arg2 = atof(argv[3]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "lowshelf") == 0) {
		if (argc != 4) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_LOWSHELF;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		arg2 = atof(argv[3]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "highshelf") == 0) {
		if (argc != 4) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_HIGHSHELF;
		arg0 = parse_freq(argv[1]);
		arg1 = atof(argv[2]);
		arg2 = atof(argv[3]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "frequency");
		CHECK_RANGE(arg1 > 0.0, "q");
	}
	else if (strcmp(argv[0], "linkwitz_transform") == 0) {
		if (argc != 5) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_LINKWITZ_TRANSFORM;
		arg0 = atof(argv[1]);
		arg1 = atof(argv[2]);
		arg2 = atof(argv[3]);
		arg3 = atof(argv[4]);
		CHECK_RANGE(arg0 >= 0.0 && arg0 < (double) dsp_globals.fs / 2.0, "fz");
		CHECK_RANGE(arg1 > 0.0, "qz");
		CHECK_RANGE(arg2 >= 0.0 && arg2 < (double) dsp_globals.fs / 2.0, "fp");
		CHECK_RANGE(arg3 > 0.0, "qp");
	}
	else if (strcmp(argv[0], "biquad") == 0) {
		if (argc != 7) {
			LOG(LL_ERROR, "dsp: %s: usage: %s\n", argv[0], ei->usage);
			return NULL;
		}
		type = BIQUAD_NONE;
		b0 = atof(argv[1]);
		b1 = atof(argv[2]);
		b2 = atof(argv[3]);
		a0 = atof(argv[4]);
		a1 = atof(argv[5]);
		a2 = atof(argv[6]);
	}
	else {
		LOG(LL_ERROR, "dsp: biquad: BUG: unknown filter type: %s\n", argv[0]);
		return NULL;
	}

	e = malloc(sizeof(struct effect));
	e->run = biquad_effect_run;
	e->plot = biquad_effect_plot;
	e->destroy = biquad_effect_destroy;
	state = malloc(sizeof(struct biquad_state) * dsp_globals.channels);
	if (type == BIQUAD_NONE) {
		for (i = 0; i < dsp_globals.channels; ++i)
			biquad_init(&state[i], b0, b1, b2, a0, a1, a2);
	}
	else {
		for (i = 0; i < dsp_globals.channels; ++i)
			biquad_init_using_type(&state[i], type, dsp_globals.fs, arg0, arg1, arg2, arg3);
	}
	e->data = state;
	return e;
}
