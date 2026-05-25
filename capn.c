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

#include <float.h>
#include <math.h>
#include "capn.h"
#include "util.h"

void ap3_reset(struct ap3_state *state)
{
	ap2_reset(&state->ap2);
	ap1_reset(&state->ap1);
}

void ap4_reset(struct ap4_state *state)
{
	ap2_reset(&state->ap2[0]);
	ap2_reset(&state->ap2[1]);
}

static void capn_butterworth_ap(int n, double complex *ap)
{
	const int n_ap = n/2+1;
	for (int i = 0; i < n_ap; ++i) {
		const double theta = (2*i+1)*M_PI/(2.0*n);
		ap[i] = -sin(theta) + cos(theta)*I;  /* normalized pole in s-plane */
	}
}

static void capn_chebyshev_ap(int n, int type2, double stop_dB, double complex *ap)
{
	if (stop_dB > 100.0) {
		capn_butterworth_ap(n, ap);
		return;
	}
	const int n_ap = n/2+1;
	const double epsilon = sqrt(pow(10.0, stop_dB/10.0) - 1.0);
	const double sigma = asinh(epsilon)/n;
	const double scale = cosh(acosh(epsilon)/n);
	for (int i = 0; i < n_ap; ++i) {
		const double theta = (2*i+1)*M_PI/(2.0*n);
		ap[i] = -sinh(sigma)*sin(theta) + cosh(sigma)*cos(theta)*I;  /* normalized pole in s-plane */
		ap[i] = ap[i] / scale;  /* scale so H(1) = sqrt(0.5) */
		if (type2) ap[i] = 1.0/ap[i];
	}
}

static inline int fz_sgn(double x)
{
	if (x < 0.0) return -1;
	else if (x > 0.0) return 1;
	return 0;
}

#define FIND_ZERO_MAX_ITER 100
static double find_zero(double (*fn)(double, const void *), const void *arg, double a, double b, double tol)
{
	double c = a, fn_a = fn(a, arg), fn_b = fn(b, arg);
	if (tol < DBL_EPSILON) tol = DBL_EPSILON*2;
	for (int i = 0, side = 0; i < FIND_ZERO_MAX_ITER; ++i) {
		c = (fn_a*b - fn_b*a) / (fn_a - fn_b);
		if (fabs(b-a) < tol*fabs(b+a)) return c;
		const double fn_c = fn(c, arg);
		if (fz_sgn(fn_b) == fz_sgn(fn_c)) {
			b = c; fn_b = fn_c;
			if (side == -1) fn_a /= 2.0;
			side = -1;
		}
		else if (fz_sgn(fn_a) == fz_sgn(fn_c)) {
			a = c; fn_a = fn_c;
			if (side == 1) fn_b /= 2.0;
			side = 1;
		}
		else {
			if (i == 0) return -NAN;  /* no zero within interval [a, b] */
			return c;
		}
	}
	return -NAN;  /* failed to converge */
}

static double ellip_q_err(double k, const void *arg)
{
	const double target_q = *((double *) arg);
	//LOG_FMT(LL_VERBOSE, "%s(): k=%.15e; target_q=%.15e", __func__, k, target_q);
	const double kp = sqrt(sqrt(1.0-k*k));
	const double l = (1.0-kp)/((1.0+kp)*2.0);
	return (l + 2.0*pow(l, 5) + 15.0*pow(l, 9) + 150.0*pow(l, 13)) - target_q;
}

/* Evaluate allpass given by poles ap at jw (=j*w) */
static double complex eval_allpass_ap(const double complex *ap, int n, int stride, double complex jw)
{
	int end = (n-1)*stride;
	double complex num = 1.0, den = 1.0;
	if (cimag(ap[end]) == 0) {  /* real root is always last */
		num *= jw + ap[end];
		den *= jw - ap[end];
		end -= stride;
	}
	for (int i = end; i >= 0; i -= stride) {
		num *= (jw + ap[i]) * (jw + conj(ap[i]));  /* conjugates not stored */
		den *= (jw - ap[i]) * (jw - conj(ap[i]));
	}
	return num / den;
}

/* Dot product treating a and b as vectors */
static inline double geom_dot(double complex a, double complex b)
{
	return creal(a)*creal(b) + cimag(a)*cimag(b);
}

struct ellip_wc_err_arg {
	const double complex *ap;
	int n0, n1;
};

static double ellip_wc_err(double w, const void *arg)
{
	const double complex jw = w*I;
	struct ellip_wc_err_arg *a = (struct ellip_wc_err_arg *) arg;
	//LOG_FMT(LL_VERBOSE, "%s(): w=%.15e", __func__, w);
	return geom_dot(eval_allpass_ap(&a->ap[0], a->n0, 2, jw), eval_allpass_ap(&a->ap[1], a->n1, 2, jw));
}

static void capn_elliptic_ap(int n, double stop_dB_lp, double stop_dB_hp, double complex *ap)
{
	if (stop_dB_lp > 100.0) {
		capn_chebyshev_ap(n, 0, stop_dB_hp, ap);
		return;
	}
	else if (stop_dB_hp > 100.0) {
		capn_chebyshev_ap(n, 1, stop_dB_lp, ap);
		return;
	}

	const int n_ap = n/2+1;
	const double e2 = 1.0 / (pow(10.0, stop_dB_hp/10.0) - 1.0);
	const double D = (pow(10.0, stop_dB_lp/10.0) - 1.0) / e2;
	const double q = 1.0 / (exp2(4.0/n) * pow(D, 1.0/n));
	const double k = find_zero(ellip_q_err, &q, 0.0, 1.0, 0);
	if (!isnormal(k)) goto fail_fz;

	const double L = log((sqrt(1.0+e2)+1.0) / (sqrt(1.0+e2)-1.0)) / (2.0*n);
	double sigma0_s0 = sinh(L), sigma0_s1 = 0.0;
	for (int m = 1; m < 6; ++m) {
		const int sgn = (m&1)?-1:1;
		sigma0_s0 += sgn * pow(q, m*(m+1)) * sinh((2*m+1)*L);
		sigma0_s1 += sgn * pow(q, m*m) * cosh(2*m*L);
	}
	const double sigma0 = fabs((2.0*sqrt(sqrt(q))*sigma0_s0) / (1.0+2.0*sigma0_s1));
	const double sigma02 = sigma0*sigma0;

	const double W = sqrt((1.0+k*sigma02) * (1.0+sigma02/k));
	for (int i = 0; i < n_ap-1; ++i) {
		const double mu = (n_ap-1)-i;
		double omega_s0 = sin(M_PI*mu/n), omega_s1 = 0.0;
		for (int m = 1; m < 6; ++m) {
			const int sgn = (m&1)?-1:1;
			omega_s0 += sgn * pow(q, m*(m+1)) * sin((2*m+1)*M_PI*mu/n);
			omega_s1 += sgn * pow(q, m*m) * cos(2*m*M_PI*mu/n);
		}
		const double omega = (2.0*sqrt(sqrt(q))*omega_s0) / (1.0+2.0*omega_s1);
		const double omega2 = omega*omega;
		const double Vi = sqrt((1.0-k*omega2)*(1.0-omega2/k));
		ap[i] = (-2.0*sigma0*Vi + 2.0*omega*W*I) / (2.0*(1.0+sigma02*omega2));  /* complex poles (conjugates not stored) */
	}
	ap[n_ap-1] = -sigma0;  /* real pole */

	if (fabs(stop_dB_lp - stop_dB_hp) > 0.01) {
		/* scale so that magnitude at w=1 is sqrt(0.5) */
		const int n1 = n_ap/2, n0 = n1+(n_ap&1);
		const struct ellip_wc_err_arg err_arg = { .ap = ap, .n0 = n0, .n1 = n1 };
		const double half_width = sqrt(1.0/k);
		const double wc = find_zero(ellip_wc_err, &err_arg, 1.0/half_width, half_width, 0);
		if (!isnormal(wc)) goto fail_fz;
		for (int i = 0; i < n_ap; ++i) ap[i] /= wc;
	}
	//for (int i = 0; i < n_ap; ++i) LOG_FMT(LL_VERBOSE, "%s(): ap: %g%+gi", __func__, creal(ap[i]), cimag(ap[i]));
	return;

	fail_fz:
	LOG_FMT(LL_ERROR, "%s(): BUG: failed to converge; falling back to butterworth", __func__);
	capn_butterworth_ap(n, ap);
}

int capn_ap(enum capn_filter_type f, int n, const double stop_dB[2], double complex *ap)
{
	if ((n&1)==0) return 1;  /* n must be odd */
	switch (f) {
	case CAPN_FILTER_BUTTERWORTH:
		capn_butterworth_ap(n, ap);
		break;
	case CAPN_FILTER_CHEBYSHEV1:
	case CAPN_FILTER_CHEBYSHEV2:
		capn_chebyshev_ap(n, (f==CAPN_FILTER_CHEBYSHEV2), stop_dB[0], ap);
		break;
	case CAPN_FILTER_ELLIPTIC:
		capn_elliptic_ap(n, stop_dB[0], stop_dB[1], ap);
		break;
	}
	return 0;
}

static void capn_bilinear(int n, double fs, double fc, const double complex *ap, double complex *p)
{
	const int n_ap = n/2+1;
	const double fc_w = 2.0*fs*tan(M_PI*fc/fs);  /* pre-warped corner frequency */
	for (int i = 0; i < n_ap; ++i) {
		p[i] = ap[i]*fc_w;  /* scale */
		p[i] = (2.0*fs + p[i]) / (2.0*fs - p[i]);  /* bilinear transform */
		//LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: p[%d] = %f%+fi", __func__, fc, i, creal(p[i]), cimag(p[i]));
	}
}

static void ap1_init_coefs(struct ap1_state *ap, double p0)
{
	ap->c0 = -p0;
}

static void ap2_init_coefs(struct ap2_state *ap, double complex p0)
{
	ap->c0 = -2.0*creal(p0);
	ap->c1 = creal(p0)*creal(p0) + cimag(p0)*cimag(p0);
}

static void ap3_init_coefs(struct ap3_state *ap, double complex p0, double p1)
{
	ap2_init_coefs(&ap->ap2, p0);
	ap1_init_coefs(&ap->ap1, p1);
}

static void ap4_init_coefs(struct ap4_state *ap, double complex p0, double complex p1)
{
	ap2_init_coefs(&ap->ap2[0], p0);
	ap2_init_coefs(&ap->ap2[1], p1);
}

void cap5_reset(struct cap5_state *state)
{
	ap2_reset(&state->a1);
	ap3_reset(&state->a2);
}

void cap5_init(struct cap5_state *state, double fs, double fc, const double complex ap[3])
{
	double complex p[3] = {0};
	capn_bilinear(5, fs, fc, ap, p);
	ap2_init_coefs(&state->a1, p[1]);
	ap3_init_coefs(&state->a2, p[0], creal(p[2]));
#if 0
	LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a1: c0=%g  c1=%g", __func__, fc, state->a1.c0, state->a1.c1);
	LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a2.ap2: c0=%g  c1=%g", __func__, fc, state->a2.ap2.c0, state->a2.ap2.c1);
	LOG_FMT(LL_VERBOSE, "%s(): fc=%gHz: a2.ap1: c0=%g", __func__, fc, state->a2.ap1.c0);
#endif
	cap5_reset(state);
}

void cap7_reset(struct cap7_state *state)
{
	ap3_reset(&state->a1);
	ap4_reset(&state->a2);
}

void cap7_init(struct cap7_state *state, double fs, double fc, const double complex ap[4])
{
	double complex p[4] = {0};
	capn_bilinear(7, fs, fc, ap, p);
	ap3_init_coefs(&state->a1, p[1], creal(p[3]));
	ap4_init_coefs(&state->a2, p[0], p[2]);
	cap7_reset(state);
}
