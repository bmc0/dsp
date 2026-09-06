/*
 * This file is part of dsp.
 *
 * Copyright (c) 2022-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#include "capn.h"
#include "allpass.h"
#include "biquad.h"

#define FB_MAX_BANDS      21
#define FB_MAX_BANDS_CAP5 13
#define FB_MAX_BANDS_CAP7 21

static const double fb_fdiv_13band[] =
	{ 170, 316.39, 516.52, 790.1, 1164.1, 1675.4, 2374.3, 3329.8, 4636.1, 6421.7, 8862.9, 12200 };
static const double fb_fc_13band[] =
	{ 112.28, 237.49, 408.65, 642.64, 962.52, 1399.8, 1997.6, 2814.8, 3932, 5459.3, 7547.1, 10401, 14303 };
static const int fb_ap_idx_13band[] =
	{ 6, 4, 7, 3, 8, 2, 9, 1, 10, 0, 11, 3, 1, 9, 7, 4, 0, 10, 6, 11, 1, 4, 7, 11, 9 };
static const double fb_stop_scale_13band[] =
	{ 1.5, 1.2, 1.1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const double fb_thresh_scale_13band[] =
	{ 1.06, 0.936, 0.792, 0.695, 0.634, 0.585, 0.539, 0.512, 0.484, 0.451, 0.433, 0.431, 0.4 };
static const double an_fb_tpeak_13band[] =
	{ 5.98, 4.31, 2.98, 1.98, 1.31, 0.896, 0.646, 0.479, 0.312, 0.229, 0.146, 0.104, 0.0625 };

static const double fb_fdiv_13band_lfx[] =
	{ 115, 246.17, 427.39, 677.74, 1023.6, 1501.4, 2161.5, 3073.4, 4333.3, 6073.8, 8478.2, 11800 };
static const double fb_fc_13band_lfx[] =
	{ 63.698, 175.3, 329.48, 542.48, 836.73, 1243.3, 1804.9, 2580.7, 3652.6, 5133.4, 7179.1, 10005, 13910 };
#define fb_ap_idx_13band_lfx fb_ap_idx_13band
static const double fb_stop_scale_13band_lfx[] =
	{ 1.7, 1.3, 1.2, 1.1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const double fb_thresh_scale_13band_lfx[] =
	{ 1.22, 1.01, 0.841, 0.721, 0.649, 0.599, 0.549, 0.517, 0.49, 0.458, 0.435, 0.432, 0.4 };
static const double an_fb_tpeak_13band_lfx[] =
	{ 7.31, 5.31, 3.31, 2.15, 1.48, 1.06, 0.729, 0.479, 0.354, 0.229, 0.167, 0.104, 0.0625 };

static const double fb_fdiv_21band[] =
	{ 78, 149.49, 237.63, 346.3, 480.3, 645.52, 849.22, 1100.4, 1410.1, 1791.9, 2262.7, 2843.2, 3558.9, 4441.4, 5529.5, 6871.1, 8525.2, 10565, 13079, 16180 };
static const double fb_fc_21band[] =
	{ 47.494, 111.87, 191.25, 289.12, 409.8, 558.59, 742.04, 968.24, 1247.1, 1591, 2015, 2537.8, 3182.3, 3977.1, 4957, 6165.2, 7654.9, 9491.6, 11756, 14549, 17991 };
static const int fb_ap_idx_21band[] =
	{ 10, 8, 11, 7, 12, 6, 13, 5, 14, 4, 15, 3, 16, 2, 17, 1, 18, 0, 19, 5, 3, 15, 13, 6, 2, 16, 12, 7, 1, 17, 11, 8, 0, 18, 10, 19, 2, 0, 7, 5, 12, 10, 18, 16, 3, 8, 13, 19, 15, 3, 8, 13, 16, 19 };
static const double fb_stop_scale_21band[] =
	{ 2, 1.5, 1.3, 1.2, 1.1, 1.1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const double fb_thresh_scale_21band[] =
	{ 1.31, 1.19, 1.1, 0.986, 0.899, 0.844, 0.795, 0.739, 0.719, 0.68, 0.637, 0.627, 0.609, 0.579, 0.56, 0.53, 0.503, 0.502, 0.502, 0.5, 0.5 };
static const double an_fb_tpeak_21band[] =
	{ 18, 15.3, 12.6, 8.65, 6.31, 5.65, 4.31, 3.31, 2.65, 2.15, 1.65, 1.31, 1.06, 0.812, 0.646, 0.438, 0.396, 0.25, 0.229, 0.208, 0.0625 };

#define FB_MAX_AP_2(X) LENGTH(X)
#define FB_MAX_AP_1(N) FB_MAX_AP_2(fb_ap_idx_ ## N ## band)
#define FB_MAX_AP(N) FB_MAX_AP_1(N)

enum filter_bank_order {
	FB_ORDER_5 = 5,
	FB_ORDER_7 = 7,
};

enum filter_bank_id {
	FB_ID_21band = 1,
	FB_ID_13band,
};
#define FB_ID_13band_lfx FB_ID_13band

struct syn_filter_bank {
	union {
		struct {
			struct cap5_state f[FB_MAX_BANDS_CAP5-1];
			struct ap2_state ap[FB_MAX_AP(FB_MAX_BANDS_CAP5)];
		} cap5;
		struct {
			struct cap7_state f[FB_MAX_BANDS_CAP7-1];
			struct ap3_state ap[FB_MAX_AP(FB_MAX_BANDS_CAP7)];
		} cap7;
	};
	sample_t s[FB_MAX_BANDS];
};

struct filter_bank {
	struct syn_filter_bank syn[2];
	struct {
		union {
			struct cap3_state cap3[FB_MAX_BANDS_CAP5-1][2];
			struct cap5_state cap5[FB_MAX_BANDS_CAP7-1][2];
		};
		sample_t s[FB_MAX_BANDS][2];
		int p, mask[FB_MAX_BANDS];
	} an;
	enum filter_bank_id id;
	int n_bands, n_ap;
	enum filter_bank_order order;
};

struct filter_bank_params {
	const char *id_str;
	enum filter_bank_id id;
	enum filter_bank_order order;
	int n_bands, n_ap, min_fs;
	const double *fdiv, *fc, *stop_scale, *thresh_scale, *an_tpeak;
	const int *ap_idx;
	double fir_len, width_scale, an_stop;
};

#define FB_PARAMS_DEF(ID, N, MIN_FS, FIR_LEN, WS, AN_ST) { \
	.id_str = XSTR(ID), \
	.id = FB_ID_ ## ID, \
	.order = FB_ORDER_ ## N, \
	.n_bands = LENGTH(fb_fc_ ## ID), \
	.n_ap = LENGTH(fb_ap_idx_ ## ID), \
	.min_fs = MIN_FS, \
	.fdiv = fb_fdiv_ ## ID, \
	.fc = fb_fc_ ## ID, \
	.stop_scale = fb_stop_scale_ ## ID, \
	.thresh_scale = fb_thresh_scale_ ## ID, \
	.ap_idx = fb_ap_idx_ ## ID, \
	.fir_len = FIR_LEN, \
	.width_scale = WS, \
	.an_tpeak = an_fb_tpeak_ ## ID, \
	.an_stop = AN_ST, \
}
static const struct filter_bank_params fb_params[] = {
	FB_PARAMS_DEF(21band,     7, 40000, 160.0, 1.94031, 27.0),
	FB_PARAMS_DEF(13band,     5, 32000,  50.0, 2.89661, 18.0),
	FB_PARAMS_DEF(13band_lfx, 5, 32000,  70.0, 2.99404, 18.0),
};

typedef const double fshape_params[4];
static fshape_params fshape_lf = { 10, M_SQRT1_2, 180, 0.4 };
static fshape_params fshape_hf = { 0.46, 0.35, 16000, 0.35 };  /* note: [0] is multiplied by fs */

struct fshape_state {
	struct biquad_state lf, hf;
};

static void fshape_filter_init(struct biquad_state *b, double fs, fshape_params p, int is_hf, int is_inv)
{
	const int type = (is_hf) ? BIQUAD_LOWPASS_TRANSFORM : BIQUAD_HIGHPASS_TRANSFORM;
	const double f0 = (is_hf) ? fs*p[0] : p[0];
	const double f2 = (is_hf) ? MINIMUM(f0*0.9, p[2]) : p[2];
	if (is_inv) biquad_init_using_type(b, type, fs, f2, p[3], f0, p[1], BIQUAD_WIDTH_Q);
	else biquad_init_using_type(b, type, fs, f0, p[1], f2, p[3], BIQUAD_WIDTH_Q);
}

static void fshape_init(struct fshape_state *state, double fs, fshape_params lfp, fshape_params hfp, int is_inv)
{
	fshape_filter_init(&state->lf, fs, lfp, 0, is_inv);
	fshape_filter_init(&state->hf, fs, hfp, 1, is_inv);
}

static inline sample_t fshape_run(struct fshape_state *state, sample_t s)
{
	return biquad(&state->hf, biquad(&state->lf, s));
}

static double get_stop_scale(const struct filter_bank_params *fbp, int i, double fs)
{
	double hf_warp = log(tan(M_PI*fbp->fdiv[i]/fs)+1.9)*1.142245;
	if (hf_warp < 1.0) hf_warp = 1.0;
	double sc = (fbp->stop_scale) ? fbp->stop_scale[i] : 1.0;
	return nearbyint(sc*hf_warp*10.0)/10.0;
}

static int filter_bank_init(struct filter_bank *fb, const struct filter_bank_params *fbp, double fs, enum capn_filter_type fb_type, double fb_stop[2])
{
	double complex ap[CAPN_MAX_AP], an_ap[CAPN_MAX_AP];
	fb->id = fbp->id;
	fb->n_bands = fbp->n_bands;
	fb->n_ap = fbp->n_ap;
	fb->order = fbp->order;
	fb->an.p = 0;
	double prev_sc = 0.0, an_tr_bw = 2.0;
	switch (fb->order) {
		case FB_ORDER_5: an_tr_bw = 2.4; break;
		case FB_ORDER_7: an_tr_bw = 1.6; break;
	}
	fb->an.mask[fb->n_bands-1] = 0;
	fb->an.mask[fb->n_bands-2] = 0;
	for (int i = fb->n_bands-3, an_fs_div = 1; i >= 0; --i) {
		while (fbp->fdiv[i]*an_tr_bw*(an_fs_div*4) < fs && an_fs_div <= DOWNSAMPLE_FACTOR/2)
			an_fs_div *= 2;
		fb->an.mask[i] = an_fs_div-1;
		//LOG_FMT(LL_VERBOSE, "%s: band %d: an_fs_div=%d", __func__, i, an_fs_div);
	}
	for (int i = 0; i < fb->n_bands-1; ++i) {
		const double an_fs = fs/(fb->an.mask[i+1]+1);
		const double an_st_sc[2] = { fbp->an_stop*get_stop_scale(fbp, i, an_fs), 0.0 };
		if (capn_ap(CAPN_FILTER_CHEBYSHEV2, (int) fb->order - 2, an_st_sc, an_ap)) return 1;
		const double sc = get_stop_scale(fbp, i, fs);
		if (sc != prev_sc) {
			const double st_sc[2] = { fb_stop[0]*sc, fb_stop[1]*sc };
			if (capn_ap(fb_type, (int) fb->order, st_sc, ap)) return 1;
			prev_sc = sc;
		}
		switch (fb->order) {
		case FB_ORDER_5:
			cap5_init(&fb->syn[0].cap5.f[i], fs, fbp->fdiv[i], ap);
			cap3_init(&fb->an.cap3[i][0], an_fs, fbp->fdiv[i], an_ap);
			fb->an.cap3[i][1] = fb->an.cap3[i][0];
			break;
		case FB_ORDER_7:
			cap7_init(&fb->syn[0].cap7.f[i], fs, fbp->fdiv[i], ap);
			cap5_init(&fb->an.cap5[i][0], an_fs, fbp->fdiv[i], an_ap);
			fb->an.cap5[i][1] = fb->an.cap5[i][0];
			break;
		}
	}
	switch (fb->order) {
	case FB_ORDER_5:
		for (int i = 0; i < fb->n_ap; ++i)
			fb->syn[0].cap5.ap[i] = fb->syn[0].cap5.f[fbp->ap_idx[i]].a1;
		break;
	case FB_ORDER_7:
		for (int i = 0; i < fb->n_ap; ++i)
			fb->syn[0].cap7.ap[i] = fb->syn[0].cap7.f[fbp->ap_idx[i]].a1;
		break;
	}
	memcpy(&fb->syn[1], &fb->syn[0], sizeof(struct syn_filter_bank));
	return 0;
}

static void filter_bank_reset(struct filter_bank *fb)
{
	fb->an.p = 0;
	for (int k = 0; k < LENGTH(fb->syn); ++k) {
		switch (fb->order) {
		case FB_ORDER_5:
			for (int i = 0; i < fb->n_bands-1; ++i) cap5_reset(&fb->syn[k].cap5.f[i]);
			for (int i = 0; i < fb->n_ap; ++i) ap2_reset(&fb->syn[k].cap5.ap[i]);
			for (int i = 0; i < fb->n_bands-1; ++i) cap3_reset(&fb->an.cap3[i][k]);
			break;
		case FB_ORDER_7:
			for (int i = 0; i < fb->n_bands-1; ++i) cap7_reset(&fb->syn[k].cap7.f[i]);
			for (int i = 0; i < fb->n_ap; ++i) ap3_reset(&fb->syn[k].cap7.ap[i]);
			for (int i = 0; i < fb->n_bands-1; ++i) cap5_reset(&fb->an.cap5[i][k]);
			break;
		}
		memset(fb->syn[k].s, 0, sizeof(fb->syn[k].s));
	}
	memset(fb->an.s, 0, sizeof(fb->an.s));
}

static inline void syn_filter_bank_run_21band(struct syn_filter_bank *fb, const sample_t s)
{
	cap7_run(&fb->cap7.f[9], s, &fb->s[9], &fb->s[10]);  /* split at xover 9 (1791.9Hz) */
	fb->s[9] = ap3_run(&fb->cap7.ap[0], fb->s[9]);  /* xover 10 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[1], fb->s[10]);  /* xover 8 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[2], fb->s[9]);  /* xover 11 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[3], fb->s[10]);  /* xover 7 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[4], fb->s[9]);  /* xover 12 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[5], fb->s[10]);  /* xover 6 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[6], fb->s[9]);  /* xover 13 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[7], fb->s[10]);  /* xover 5 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[8], fb->s[9]);  /* xover 14 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[9], fb->s[10]);  /* xover 4 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[10], fb->s[9]);  /* xover 15 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[11], fb->s[10]);  /* xover 3 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[12], fb->s[9]);  /* xover 16 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[13], fb->s[10]);  /* xover 2 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[14], fb->s[9]);  /* xover 17 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[15], fb->s[10]);  /* xover 1 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[16], fb->s[9]);  /* xover 18 ap */
	fb->s[10] = ap3_run(&fb->cap7.ap[17], fb->s[10]);  /* xover 0 ap */
	fb->s[9] = ap3_run(&fb->cap7.ap[18], fb->s[9]);  /* xover 19 ap */

	cap7_run(&fb->cap7.f[4], fb->s[9], &fb->s[4], &fb->s[5]);  /* split at xover 4 (480.3Hz) */
	cap7_run(&fb->cap7.f[14], fb->s[10], &fb->s[14], &fb->s[15]);  /* split at xover 14 (5529.5Hz) */
	fb->s[4] = ap3_run(&fb->cap7.ap[19], fb->s[4]);  /* xover 5 ap */
	fb->s[5] = ap3_run(&fb->cap7.ap[20], fb->s[5]);  /* xover 3 ap */
	fb->s[14] = ap3_run(&fb->cap7.ap[21], fb->s[14]);  /* xover 15 ap */
	fb->s[15] = ap3_run(&fb->cap7.ap[22], fb->s[15]);  /* xover 13 ap */
	fb->s[4] = ap3_run(&fb->cap7.ap[23], fb->s[4]);  /* xover 6 ap */
	fb->s[5] = ap3_run(&fb->cap7.ap[24], fb->s[5]);  /* xover 2 ap */
	fb->s[14] = ap3_run(&fb->cap7.ap[25], fb->s[14]);  /* xover 16 ap */
	fb->s[15] = ap3_run(&fb->cap7.ap[26], fb->s[15]);  /* xover 12 ap */
	fb->s[4] = ap3_run(&fb->cap7.ap[27], fb->s[4]);  /* xover 7 ap */
	fb->s[5] = ap3_run(&fb->cap7.ap[28], fb->s[5]);  /* xover 1 ap */
	fb->s[14] = ap3_run(&fb->cap7.ap[29], fb->s[14]);  /* xover 17 ap */
	fb->s[15] = ap3_run(&fb->cap7.ap[30], fb->s[15]);  /* xover 11 ap */
	fb->s[4] = ap3_run(&fb->cap7.ap[31], fb->s[4]);  /* xover 8 ap */
	fb->s[5] = ap3_run(&fb->cap7.ap[32], fb->s[5]);  /* xover 0 ap */
	fb->s[14] = ap3_run(&fb->cap7.ap[33], fb->s[14]);  /* xover 18 ap */
	fb->s[15] = ap3_run(&fb->cap7.ap[34], fb->s[15]);  /* xover 10 ap */
	fb->s[14] = ap3_run(&fb->cap7.ap[35], fb->s[14]);  /* xover 19 ap */

	cap7_run(&fb->cap7.f[1], fb->s[4], &fb->s[1], &fb->s[2]);  /* split at xover 1 (149.49Hz) */
	cap7_run(&fb->cap7.f[6], fb->s[5], &fb->s[6], &fb->s[7]);  /* split at xover 6 (849.22Hz) */
	cap7_run(&fb->cap7.f[11], fb->s[14], &fb->s[11], &fb->s[12]);  /* split at xover 11 (2843.2Hz) */
	cap7_run(&fb->cap7.f[17], fb->s[15], &fb->s[17], &fb->s[18]);  /* split at xover 17 (10565Hz) */
	fb->s[1] = ap3_run(&fb->cap7.ap[36], fb->s[1]);  /* xover 2 ap */
	fb->s[2] = ap3_run(&fb->cap7.ap[37], fb->s[2]);  /* xover 0 ap */
	fb->s[6] = ap3_run(&fb->cap7.ap[38], fb->s[6]);  /* xover 7 ap */
	fb->s[7] = ap3_run(&fb->cap7.ap[39], fb->s[7]);  /* xover 5 ap */
	fb->s[11] = ap3_run(&fb->cap7.ap[40], fb->s[11]);  /* xover 12 ap */
	fb->s[12] = ap3_run(&fb->cap7.ap[41], fb->s[12]);  /* xover 10 ap */
	fb->s[17] = ap3_run(&fb->cap7.ap[42], fb->s[17]);  /* xover 18 ap */
	fb->s[18] = ap3_run(&fb->cap7.ap[43], fb->s[18]);  /* xover 16 ap */
	fb->s[1] = ap3_run(&fb->cap7.ap[44], fb->s[1]);  /* xover 3 ap */
	fb->s[6] = ap3_run(&fb->cap7.ap[45], fb->s[6]);  /* xover 8 ap */
	fb->s[11] = ap3_run(&fb->cap7.ap[46], fb->s[11]);  /* xover 13 ap */
	fb->s[17] = ap3_run(&fb->cap7.ap[47], fb->s[17]);  /* xover 19 ap */
	fb->s[18] = ap3_run(&fb->cap7.ap[48], fb->s[18]);  /* xover 15 ap */

	cap7_run(&fb->cap7.f[0], fb->s[1], &fb->s[0], &fb->s[1]);  /* split at xover 0 (78Hz) */
	cap7_run(&fb->cap7.f[2], fb->s[2], &fb->s[2], &fb->s[3]);  /* split at xover 2 (237.63Hz) */
	cap7_run(&fb->cap7.f[5], fb->s[6], &fb->s[5], &fb->s[6]);  /* split at xover 5 (645.52Hz) */
	cap7_run(&fb->cap7.f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (1100.4Hz) */
	cap7_run(&fb->cap7.f[10], fb->s[11], &fb->s[10], &fb->s[11]);  /* split at xover 10 (2262.7Hz) */
	cap7_run(&fb->cap7.f[12], fb->s[12], &fb->s[12], &fb->s[13]);  /* split at xover 12 (3558.9Hz) */
	cap7_run(&fb->cap7.f[15], fb->s[17], &fb->s[15], &fb->s[16]);  /* split at xover 15 (6871.1Hz) */
	cap7_run(&fb->cap7.f[18], fb->s[18], &fb->s[18], &fb->s[19]);  /* split at xover 18 (13079Hz) */
	fb->s[2] = ap3_run(&fb->cap7.ap[49], fb->s[2]);  /* xover 3 ap */
	fb->s[7] = ap3_run(&fb->cap7.ap[50], fb->s[7]);  /* xover 8 ap */
	fb->s[12] = ap3_run(&fb->cap7.ap[51], fb->s[12]);  /* xover 13 ap */
	fb->s[15] = ap3_run(&fb->cap7.ap[52], fb->s[15]);  /* xover 16 ap */
	fb->s[18] = ap3_run(&fb->cap7.ap[53], fb->s[18]);  /* xover 19 ap */

	cap7_run(&fb->cap7.f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (346.3Hz) */
	cap7_run(&fb->cap7.f[8], fb->s[8], &fb->s[8], &fb->s[9]);  /* split at xover 8 (1410.1Hz) */
	cap7_run(&fb->cap7.f[13], fb->s[13], &fb->s[13], &fb->s[14]);  /* split at xover 13 (4441.4Hz) */
	cap7_run(&fb->cap7.f[16], fb->s[16], &fb->s[16], &fb->s[17]);  /* split at xover 16 (8525.2Hz) */
	cap7_run(&fb->cap7.f[19], fb->s[19], &fb->s[19], &fb->s[20]);  /* split at xover 19 (16180Hz) */
}

static inline void syn_filter_bank_run_13band(struct syn_filter_bank *fb, const sample_t s)
{
	cap5_run(&fb->cap5.f[5], s, &fb->s[5], &fb->s[6]);  /* split at xover 5 (1675.4Hz) */
	fb->s[5] = ap2_run(&fb->cap5.ap[0], fb->s[5]);  /* xover 6 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[1], fb->s[6]);  /* xover 4 ap */
	fb->s[5] = ap2_run(&fb->cap5.ap[2], fb->s[5]);  /* xover 7 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[3], fb->s[6]);  /* xover 3 ap */
	fb->s[5] = ap2_run(&fb->cap5.ap[4], fb->s[5]);  /* xover 8 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[5], fb->s[6]);  /* xover 2 ap */
	fb->s[5] = ap2_run(&fb->cap5.ap[6], fb->s[5]);  /* xover 9 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[7], fb->s[6]);  /* xover 1 ap */
	fb->s[5] = ap2_run(&fb->cap5.ap[8], fb->s[5]);  /* xover 10 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[9], fb->s[6]);  /* xover 0 ap */
	fb->s[5] = ap2_run(&fb->cap5.ap[10], fb->s[5]);  /* xover 11 ap */

	cap5_run(&fb->cap5.f[2], fb->s[5], &fb->s[2], &fb->s[3]);  /* split at xover 2 (516.52Hz) */
	cap5_run(&fb->cap5.f[8], fb->s[6], &fb->s[8], &fb->s[9]);  /* split at xover 8 (4636.1Hz) */
	fb->s[2] = ap2_run(&fb->cap5.ap[11], fb->s[2]);  /* xover 3 ap */
	fb->s[3] = ap2_run(&fb->cap5.ap[12], fb->s[3]);  /* xover 1 ap */
	fb->s[8] = ap2_run(&fb->cap5.ap[13], fb->s[8]);  /* xover 9 ap */
	fb->s[9] = ap2_run(&fb->cap5.ap[14], fb->s[9]);  /* xover 7 ap */
	fb->s[2] = ap2_run(&fb->cap5.ap[15], fb->s[2]);  /* xover 4 ap */
	fb->s[3] = ap2_run(&fb->cap5.ap[16], fb->s[3]);  /* xover 0 ap */
	fb->s[8] = ap2_run(&fb->cap5.ap[17], fb->s[8]);  /* xover 10 ap */
	fb->s[9] = ap2_run(&fb->cap5.ap[18], fb->s[9]);  /* xover 6 ap */
	fb->s[8] = ap2_run(&fb->cap5.ap[19], fb->s[8]);  /* xover 11 ap */

	cap5_run(&fb->cap5.f[0], fb->s[2], &fb->s[0], &fb->s[1]);  /* split at xover 0 (170Hz) */
	cap5_run(&fb->cap5.f[3], fb->s[3], &fb->s[3], &fb->s[4]);  /* split at xover 3 (790.1Hz) */
	cap5_run(&fb->cap5.f[6], fb->s[8], &fb->s[6], &fb->s[7]);  /* split at xover 6 (2374.3Hz) */
	cap5_run(&fb->cap5.f[10], fb->s[9], &fb->s[10], &fb->s[11]);  /* split at xover 10 (8862.9Hz) */
	fb->s[0] = ap2_run(&fb->cap5.ap[20], fb->s[0]);  /* xover 1 ap */
	fb->s[3] = ap2_run(&fb->cap5.ap[21], fb->s[3]);  /* xover 4 ap */
	fb->s[6] = ap2_run(&fb->cap5.ap[22], fb->s[6]);  /* xover 7 ap */
	fb->s[10] = ap2_run(&fb->cap5.ap[23], fb->s[10]);  /* xover 11 ap */
	fb->s[11] = ap2_run(&fb->cap5.ap[24], fb->s[11]);  /* xover 9 ap */

	cap5_run(&fb->cap5.f[1], fb->s[1], &fb->s[1], &fb->s[2]);  /* split at xover 1 (316.39Hz) */
	cap5_run(&fb->cap5.f[4], fb->s[4], &fb->s[4], &fb->s[5]);  /* split at xover 4 (1164.1Hz) */
	cap5_run(&fb->cap5.f[7], fb->s[7], &fb->s[7], &fb->s[8]);  /* split at xover 7 (3329.8Hz) */
	cap5_run(&fb->cap5.f[9], fb->s[10], &fb->s[9], &fb->s[10]);  /* split at xover 9 (6421.7Hz) */
	cap5_run(&fb->cap5.f[11], fb->s[11], &fb->s[11], &fb->s[12]);  /* split at xover 11 (12200Hz) */
}

static void filter_bank_run_syn(struct filter_bank *fb, const sample_t s[2])
{
	switch (fb->id) {
	case FB_ID_21band:
		syn_filter_bank_run_21band(&fb->syn[0], s[0]);
		syn_filter_bank_run_21band(&fb->syn[1], s[1]);
		break;
	case FB_ID_13band:
		syn_filter_bank_run_13band(&fb->syn[0], s[0]);
		syn_filter_bank_run_13band(&fb->syn[1], s[1]);
		break;
	}
}

static sample_t filter_bank_sum(struct filter_bank *fb, sample_t s)
{
	switch (fb->order) {
	case FB_ORDER_5:
		for (int i = 0; i < fb->n_bands-1; ++i)
			s = ap2_run(&fb->syn[0].cap5.f[i].a1, s);
		break;
	case FB_ORDER_7:
		for (int i = 0; i < fb->n_bands-1; ++i)
			s = ap3_run(&fb->syn[0].cap7.f[i].a1, s);
		break;
	}
	return s;
}

static void filter_bank_run_an(struct filter_bank *fb, const sample_t s[2])
{
	++fb->an.p;
	switch (fb->id) {
	case FB_ID_21band:
		fb->an.s[20][0] = s[0];
		fb->an.s[20][1] = s[1];
		for (int i = 20; i > 0 && !(fb->an.p & fb->an.mask[i]); --i) {
			cap5_run(&fb->an.cap5[i-1][0], fb->an.s[i][0], &fb->an.s[i-1][0], &fb->an.s[i][0]);
			cap5_run(&fb->an.cap5[i-1][1], fb->an.s[i][1], &fb->an.s[i-1][1], &fb->an.s[i][1]);
		}
		break;
	case FB_ID_13band:
		fb->an.s[12][0] = s[0];
		fb->an.s[12][1] = s[1];
		for (int i = 12; i > 0 && !(fb->an.p & fb->an.mask[i]); --i) {
			cap3_run(&fb->an.cap3[i-1][0], fb->an.s[i][0], &fb->an.s[i-1][0], &fb->an.s[i][0]);
			cap3_run(&fb->an.cap3[i-1][1], fb->an.s[i][1], &fb->an.s[i-1][1], &fb->an.s[i][1]);
		}
		break;
	}
}
