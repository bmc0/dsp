/*
 * This file is part of dsp.
 *
 * Copyright (c) 2026 Michael Barbour <barbour.michael.0@gmail.com>
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
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <float.h>
#include "reverse_iir.h"
#include "biquad.h"
#include "util.h"

/*
 * References:
 *     [1] M. Vicanek, "A New Reverse IIR Filtering Algorithm," Oct. 2015,
 *         rev. Jan. 2022
*/

#define RIIR_POLE_CMP_TOL  1e-4
#define RIIR_RES_LIM       (1e-8/DBL_EPSILON)
#define RIIR_SORT_SECTIONS 1
#define RIIR_EXTRA_VERBOSE 0

enum riir_pq_type {
	RIIR_PQ_TYPE_NONE = 0,
	RIIR_PQ_TYPE_1R = 1,
	RIIR_PQ_TYPE_2R = 2,
	RIIR_PQ_TYPE_CC,
};
#define RIIR_PQ_N(t)      (((t)==RIIR_PQ_TYPE_CC)?2:(int)(t))
#define RIIR_PQ_N_EVAL(t) (((t)==RIIR_PQ_TYPE_CC)?1:(int)(t))

typedef union {
	double r[2];
	double complex c;
} qroots;

struct riir_init_sec {
	enum riir_pq_type pt, qt;
	qroots p, q, res;
	double g, thresh;
};

struct riir_init_state {
	struct riir_init_sec *sec;
	int n, len;
};

#define DECL_FILTER_STRUCTS(name, T) \
	struct riir_stage_ ## name { \
		T p, *m; \
	}; \
	struct riir_ ## name { \
		T res; \
		struct { T p, m[1<<0]; } s0; \
		struct { T p, m[1<<1]; } s1; \
		struct { T p, m[1<<2]; } s2; \
		struct riir_stage_ ## name *sn; \
	};

DECL_FILTER_STRUCTS(real, double)
DECL_FILTER_STRUCTS(cc, double complex)

struct riir_state {
	struct riir_state *cascade;
	struct riir_real *real;
	struct riir_cc *cc;
	int n_real, n_cc;
	int idx, mask, N, latency;
	struct {
		double c[8], m[8], *buf;
		int n, idx;
	} fir;
};

#define RIIR_SEC_RUN_X_DEFINE_FN(X, T, C) \
	static inline double riir_sec_run_ ## X (struct riir_ ## X *sec, const double s, const int idx, const int N) \
	{ \
		T x = s, y; \
		int mi = 0;          y = sec->s0.p*x + sec->s0.m[mi]; sec->s0.m[mi] = x; x = y; \
		mi = idx&((1<<1)-1); y = sec->s1.p*x + sec->s1.m[mi]; sec->s1.m[mi] = x; x = y; \
		mi = idx&((1<<2)-1); y = sec->s2.p*x + sec->s2.m[mi]; sec->s2.m[mi] = x; x = y; \
		for (int j = 0, n = 1<<3; j < N-3; ++j, n<<=1) \
			{ mi = idx&(n-1); y = sec->sn[j].p*x + sec->sn[j].m[mi]; sec->sn[j].m[mi] = x; x = y; } \
		C \
	}
RIIR_SEC_RUN_X_DEFINE_FN(real, double, return sec->res*y; )
RIIR_SEC_RUN_X_DEFINE_FN(cc, double complex, return 2.0*creal(y*sec->res); )

static double riir_run_fir_part(struct riir_state *state, const double s, const int idx)
{
	const double x = state->fir.buf[idx];
	state->fir.buf[idx] = s;

	if (state->fir.n == 1)
		return x*state->fir.c[0];

	const int mi = state->fir.idx;
	for (int n = mi, m = 0; m < state->fir.n; ++m) {
		state->fir.m[n] += x*state->fir.c[m];
		n = (n+1 < state->fir.n) ? n+1 : 0;
	}
	const double r = state->fir.m[mi];
	state->fir.m[mi] = 0.0;
	state->fir.idx = (mi+1 < state->fir.n) ? mi+1 : 0;
	return r;
}

static void riir_run_filter(struct riir_state *state, sample_t *buf, ssize_t samples, ssize_t stride)
{
	while (samples-- > 0) {
		const double s = *buf;
		*buf = 0.0;
		for (int i = 0; i < state->n_real; ++i)
			*buf += riir_sec_run_real(&state->real[i], s, state->idx, state->N);
		for (int i = 0; i < state->n_cc; ++i)
			*buf += riir_sec_run_cc(&state->cc[i], s, state->idx, state->N);
		if (state->fir.n)
			*buf += riir_run_fir_part(state, s, state->idx);
		buf += stride;
		state->idx = (state->idx+1)&(state->mask);
	}
}

static sample_t * reverse_iir_effect_run(struct effect *e, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		if (state[k].N > 0) {
			riir_run_filter(&state[k], &ibuf[k], *frames, e->istream.channels);
			for (struct riir_state *cs = state[k].cascade; cs; cs = cs->cascade)
				riir_run_filter(cs, &ibuf[k], *frames, e->istream.channels);
		}
	}
	return ibuf;
}

#define RESET_FILTER_STAGES(s, n, N) \
	do { for (int i = 0; i < (n); ++i) { \
		memset((s)[i].s0.m, 0, sizeof((s)->s0.m)); \
		memset((s)[i].s1.m, 0, sizeof((s)->s1.m)); \
		memset((s)[i].s2.m, 0, sizeof((s)->s2.m)); \
		for (int j = 3; j < (N); ++j) \
			memset((s)[i].sn[j-3].m, 0, (1<<j)*sizeof(*(s)->sn->m)); \
	} } while(0)
static void reverse_iir_effect_reset(struct effect *e)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		for (struct riir_state *cs = &state[k]; cs; cs = cs->cascade) {
			cs->fir.idx = cs->idx = 0;
			if (cs->fir.buf)
				memset(cs->fir.buf, 0, (1<<cs->N)*sizeof(double));
			memset(cs->fir.m, 0, sizeof(cs->fir.m));
			RESET_FILTER_STAGES(cs->real, cs->n_real, cs->N);
			RESET_FILTER_STAGES(cs->cc, cs->n_cc, cs->N);
		}
	}
}

#define PRINTF_CARGS(x) creal(x), cimag(x)
static void reverse_iir_effect_plot(struct effect *e, int idx)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		if (state[k].N > 0) {
			printf("H%d_%d(w)=(abs(w)<=pi)?1.0", k, idx);
			for (struct riir_state *cs = &state[k]; cs; cs = cs->cascade) {
				fputs("*(0", stdout);
				if (cs->fir.n > 0) {
					printf("+(%.15e", cs->fir.c[0]);
					for (int i = 1; i < cs->fir.n; ++i)
						printf("+%.15e*exp(-%d*j*w)", cs->fir.c[i], i);
					printf(")*exp(-2**%d*j*w)", cs->N);
				}
				for (int i = 0; i < cs->n_real; ++i) {
					printf("+%.15e", cs->real[i].res);
					for (int j = 0; j < cs->N; ++j)
						printf("*((%.15e)**(2**%d)+exp(-2**%d*j*w))", cs->real[i].s0.p, j, j);
				}
				for (int i = 0; i < cs->n_cc; ++i) {
					printf("+{%.15e,%.15e}", PRINTF_CARGS(cs->cc[i].res));
					for (int j = 0; j < cs->N; ++j)
						printf("*({%.15e,%.15e}**(2**%d)+exp(-2**%d*j*w))", PRINTF_CARGS(cs->cc[i].s0.p), j, j);
					printf("+{%.15e,%.15e}", PRINTF_CARGS(conj(cs->cc[i].res)));
					for (int j = 0; j < cs->N; ++j)
						printf("*({%.15e,%.15e}**(2**%d)+exp(-2**%d*j*w))", PRINTF_CARGS(conj(cs->cc[i].s0.p)), j, j);
				}
				putchar(')');
			}
			printf("*exp(%d*j*w):0/0\n", state[k].latency);
		}
		else printf("H%d_%d(w)=1.0\n", k, idx);
	}
}

static void reverse_iir_effect_drain_samples(struct effect *e, ssize_t *drain_samples)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state[k].N > 0) drain_samples[k] += state[k].latency;
}

static void reverse_iir_effect_destroy(struct effect *e)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k) {
		for (struct riir_state *cs = &state[k]; cs; cs = cs->cascade) {
			if (cs->N > 3) {
				for (int i = 0; i < cs->n_real; ++i) {
					for (int j = 0; j < cs->N-3; ++j)
						free(cs->real[i].sn[j].m);
					free(cs->real[i].sn);
				}
				for (int i = 0; i < cs->n_cc; ++i) {
					for (int j = 0; j < cs->N-3; ++j)
						free(cs->cc[i].sn[j].m);
					free(cs->cc[i].sn);
				}
			}
			free(cs->real);
			free(cs->cc);
			free(cs->fir.buf);
		}
		while (state[k].cascade) {
			struct riir_state *next = state[k].cascade->cascade;
			free(state[k].cascade);
			state[k].cascade = next;
		}
	}
	free(state);
}

static void reverse_iir_effect_channel_offsets(struct effect *e, ssize_t *latency, ssize_t *req_delay)
{
	struct riir_state *state = (struct riir_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		if (state[k].N > 0) req_delay[k] -= state[k].latency;
}

static void riir_init_state_append(struct riir_init_state *v, const struct riir_init_sec *sec, int n)
{
	if (n <= 0) return;
	if (v->len < v->n+n) {
		if (v->len <= 0) v->len = 4;
		while (v->len < v->n+n) v->len *= 2;
		v->sec = realloc(v->sec, v->len*sizeof(struct riir_init_sec));
	}
	memcpy(v->sec+v->n, sec, n*sizeof(struct riir_init_sec));
	v->n += n;
}

static void riir_init_state_remove(struct riir_init_state *v, int idx)
{
	if (idx+1 < v->n)
		memmove(v->sec+idx, v->sec+idx+1, (v->n-idx-1)*sizeof(struct riir_init_sec));
	--v->n;
}

static void riir_init_state_clear(struct riir_init_state *v)
{
	free(v->sec);
	v->sec = NULL;
	v->n = v->len = 0;
}

static int reverse_iir_effect_merge(struct effect *dest, struct effect *src)
{
	if (dest->merge == src->merge) {
		struct riir_init_state *dest_state = (struct riir_init_state *) dest->data;
		struct riir_init_state *src_state = (struct riir_init_state *) src->data;
		for (int k = 0; k < dest->istream.channels; ++k) {
			riir_init_state_append(&dest_state[k], src_state[k].sec, src_state[k].n);
			riir_init_state_clear(&src_state[k]);
		}
		return 1;
	}
	return 0;
}

static void riir_expand_pq(const qroots *pq, enum riir_pq_type type, double r[2])
{
	r[0] = 0.0; r[1] = 0.0;
	switch (type) {
	case RIIR_PQ_TYPE_CC:
		r[0] = -2.0*creal(pq->c);
		r[1] = creal(pq->c*conj(pq->c));
		break;
	case RIIR_PQ_TYPE_2R:
		r[0] = -pq->r[0]-pq->r[1];
		r[1] = pq->r[0]*pq->r[1];
		break;
	case RIIR_PQ_TYPE_1R:
		r[0] = -pq->r[0];
		break;
	case RIIR_PQ_TYPE_NONE:
	}
}

static double complex riir_eval_pq(const qroots *pq, enum riir_pq_type type, int i, double complex z)
{
	switch(type) {
	case RIIR_PQ_TYPE_CC:
		return (z-((i)?conj(pq->c):pq->c))/z;
	case RIIR_PQ_TYPE_2R:
		return (z-pq->r[(i)?1:0])/z;
	case RIIR_PQ_TYPE_1R:
		return (i)?1.0:(z-pq->r[0])/z;
	case RIIR_PQ_TYPE_NONE:
	}
	return 1.0;
}

static int riir_pq_close(const qroots *pq0, enum riir_pq_type t0, const qroots *pq1, enum riir_pq_type t1)
{
	for (int i = 0; i < RIIR_PQ_N(t0); ++i) {
		switch(t1) {
		case RIIR_PQ_TYPE_CC:
			if (cabs(riir_eval_pq(pq0, t0, i, pq1->c)) < RIIR_POLE_CMP_TOL) return 1;
			break;
		case RIIR_PQ_TYPE_2R:
			if (cabs(riir_eval_pq(pq0, t0, i, pq1->r[1])) < RIIR_POLE_CMP_TOL) return 1;
			/* fallthrough */
		case RIIR_PQ_TYPE_1R:
			if (cabs(riir_eval_pq(pq0, t0, i, pq1->r[0])) < RIIR_POLE_CMP_TOL) return 1;
			break;
		case RIIR_PQ_TYPE_NONE:
		}
	}
	return 0;
}

static double riir_pq_max_abs(const qroots *pq, enum riir_pq_type type)
{
	switch(type) {
	case RIIR_PQ_TYPE_CC: return cabs(pq->c);
	case RIIR_PQ_TYPE_2R: return MAXIMUM(fabs(pq->r[0]), fabs(pq->r[1]));
	case RIIR_PQ_TYPE_1R: return fabs(pq->r[0]);
	case RIIR_PQ_TYPE_NONE:
	}
	return 0.0;
}

#define RIIR_POLE_MIN_STAGES(thresh, abs_p) lrint(ceil(log2(-((thresh)+6.02)/(20.0*log10(abs_p)))))
#define INIT_FILTER_STAGES(s, n, N) \
	do { for (int i = 0; i < (n); ++i) { \
		typeof((s)->res) p2n = (s)[i].s0.p; \
		(s)[i].s1.p = (p2n *= p2n); \
		(s)[i].s2.p = (p2n *= p2n); \
		if ((N) > 3) { \
			(s)[i].sn = calloc((N)-3, sizeof(*(s)->sn)); \
			for (int j = 3; j < (N); ++j) { \
				(s)[i].sn[j-3].p = (p2n *= p2n); \
				(s)[i].sn[j-3].m = calloc(1<<j, sizeof(*(s)->sn->m)); \
			} \
		} \
	} } while(0)
static int reverse_iir_effect_prepare(struct effect *e)
{
	struct riir_init_state *init_state = (struct riir_init_state *) e->data;
	struct riir_state *state = calloc(e->istream.channels, sizeof(struct riir_state));
	e->data = state;
	e->destroy = reverse_iir_effect_destroy;

	for (int k = 0; k < e->istream.channels; ++k) {
		struct riir_init_state *v = &init_state[k], cascade_v = {0};
		if (v->n <= 0) continue;  /* nothing to do */
		struct riir_state *cs = &state[k];

		/* split sections with repeated real poles */
		for (int i = 0; i < v->n; ++i) {
			struct riir_init_sec *sec = &v->sec[i];
			if (sec->pt == RIIR_PQ_TYPE_2R && fabs(sec->p.r[1]-sec->p.r[0]) < RIIR_POLE_CMP_TOL) {
				struct riir_init_sec split = {0};
				split.thresh = sec->thresh;
				split.pt = sec->pt = RIIR_PQ_TYPE_1R;
				split.p.r[0] = sec->p.r[1];
				if (sec->qt == RIIR_PQ_TYPE_2R) {
					split.qt = sec->qt = RIIR_PQ_TYPE_1R;
					split.q.r[0] = sec->q.r[1];
					split.g = sec->g = sqrt(sec->g);
				}
				else split.g = 1.0;
				riir_init_state_append(&cascade_v, &split, 1);
			}
		}

		recalc_cs:
		/* move any other repeated poles */
		for (int i = 0; i < v->n; ++i) {
			if (v->sec[i].pt == RIIR_PQ_TYPE_NONE) continue;
			for (int j = i+1; j < v->n;) {
				if (riir_pq_close(&v->sec[i].p, v->sec[i].pt, &v->sec[j].p, v->sec[j].pt)) {
					riir_init_state_append(&cascade_v, &v->sec[j], 1);
					riir_init_state_remove(v, j);
				}
				else ++j;
			}
		}

		/* find minimum number of stages and total number of poles and zeros */
		cs->N = 3;  /* static stages run unconditionally */
		int nq = 0, np = 0;
		double g = 1.0;
		for (int i = 0; i < v->n; ++i) {
			struct riir_init_sec *sec = &v->sec[i];
			int p0N = 0, p1N = 0;
			nq += RIIR_PQ_N(sec->qt);
			np += RIIR_PQ_N(sec->pt);
			g *= sec->g;
			switch (sec->pt) {
			case RIIR_PQ_TYPE_CC:
				++cs->n_cc;
				p0N = RIIR_POLE_MIN_STAGES(sec->thresh, cabs(sec->p.c));
				break;
			case RIIR_PQ_TYPE_2R:
				++cs->n_real;
				p1N = RIIR_POLE_MIN_STAGES(sec->thresh, fabs(sec->p.r[1]));
				/* fallthrough */
			case RIIR_PQ_TYPE_1R:
				++cs->n_real;
				p0N = RIIR_POLE_MIN_STAGES(sec->thresh, fabs(sec->p.r[0]));
				break;
			case RIIR_PQ_TYPE_NONE: /* no poles */
			}
			cs->N = MAXIMUM(cs->N, MAXIMUM(p0N, p1N));
		}
		cs->mask = (1<<cs->N)-1;
		if (nq-np+1 > (int) LENGTH(cs->fir.c)) {
			LOG_FMT(LL_ERROR, "%s: error: channel %d: too many zeros: %d-%d+1 > %zd", e->name, k, nq, np, LENGTH(cs->fir.c));
			riir_init_state_clear(&cascade_v);
			goto fail;
		}

		/* compute partial fraction residues */
		int do_cascade = 0;
		for (int i = 0; i < v->n; ++i) {
			struct riir_init_sec *sec = &v->sec[i];
			const int is_cc = (sec->pt == RIIR_PQ_TYPE_CC);
			for (int l = 0; l < RIIR_PQ_N_EVAL(sec->pt); ++l) {
				const double complex p = (is_cc) ? sec->p.c : sec->p.r[l];
				/* evaluate z^(nq-np+1)*(1-p*z^-1)*H(z) at z=p */
				double complex num = (nq<np)?1.0:(nq==np)?p:cpow(p, nq-np+1), den = 1.0;
				for (int j = 0; j < v->n; ++j) {
					struct riir_init_sec *eval_sec = &v->sec[j];
					num *= riir_eval_pq(&eval_sec->q, eval_sec->qt, 0, p);
					num *= riir_eval_pq(&eval_sec->q, eval_sec->qt, 1, p);
					if (eval_sec != sec)
						den *= riir_eval_pq(&eval_sec->p, eval_sec->pt, l, p);
					den *= riir_eval_pq(&eval_sec->p, eval_sec->pt, (l)?0:1, p);
				}
				double complex res = num/den;
				if (isnan(cabs(res))) res = HUGE_VAL;
				if (cabs(res) > RIIR_RES_LIM) do_cascade = 1;
				if (is_cc) sec->res.c = g*res;
				else sec->res.r[l] = g*creal(res);
			}
		}

		if (do_cascade) {
			if (v->n < 2) {
				LOG_FMT(LL_ERROR, "%s: error: something has gone terribly wrong; aborting...", e->name);
				riir_init_state_clear(&cascade_v);
				goto fail;
			}
			/* move section with largest residue */
			int rm_idx = 0;
			double max_res = riir_pq_max_abs(&v->sec[0].res, v->sec[0].pt);
			for (int i = 1; i < v->n; ++i) {
				const double res = riir_pq_max_abs(&v->sec[i].res, v->sec[i].pt);
				if (res > max_res) {
					rm_idx = i;
					max_res = res;
				}
			}
			riir_init_state_append(&cascade_v, &v->sec[rm_idx], 1);
			riir_init_state_remove(v, rm_idx);
			goto recalc_cs;
		}

		#if RIIR_SORT_SECTIONS
		/* sort sections to try to minimize quantization error */
		double sort_sum = 0.0;
		for (int i = 0; i < v->n; ++i) {
			int min_idx = i;
			double min_sum = HUGE_VAL;
			for (int j = i; j < v->n; ++j) {
				struct riir_init_sec *sec = &v->sec[j];
				double sec_sum = sort_sum;
				switch (sec->pt) {
				case RIIR_PQ_TYPE_CC: sec_sum += 2.0*creal(sec->res.c); break;
				case RIIR_PQ_TYPE_2R: sec_sum += sec->res.r[1]; /* fallthrough */
				case RIIR_PQ_TYPE_1R: sec_sum += sec->res.r[0]; break;
				case RIIR_PQ_TYPE_NONE: /* does not contribute */
				}
				if (fabs(sec_sum) < fabs(min_sum)) {
					min_sum = sec_sum;
					min_idx = j;
				}
			}
			struct riir_init_sec tmp_sec = v->sec[i];
			v->sec[i] = v->sec[min_idx];
			v->sec[min_idx] = tmp_sec;
			sort_sum = min_sum;
		}
		#endif

		if (nq >= np) {
			/* get FIR part */
			cs->fir.n = nq-np+1;
			cs->fir.c[nq-np] = g;
			if (nq > np) {
				for (int i = 0; i < v->n; ++i) {
					double b[2], a[2];
					struct biquad_state bq;
					riir_expand_pq(&v->sec[i].q, v->sec[i].qt, b);
					riir_expand_pq(&v->sec[i].p, v->sec[i].pt, a);
					biquad_init(&bq, 1.0, b[0], b[1], 1.0, a[0], a[1]);
					for (int n = nq-np; n >= 0; --n)
						cs->fir.c[n] = biquad(&bq, cs->fir.c[n]);
				}
			}
		#if RIIR_EXTRA_VERBOSE
			if (LOGLEVEL(LL_VERBOSE)) {
				dsp_log_acquire();
				dsp_log_printf("%s: %s: info: channel %d: fir part: n=%d; c=[%g",
					dsp_globals.prog_name, e->name, k, cs->fir.n, cs->fir.c[0]);
				for (int i = 1; i < cs->fir.n; ++i) dsp_log_printf(" %g", cs->fir.c[i]);
				dsp_log_puts("]\n");
				dsp_log_release();
			}
		#endif
		}
		LOG_FMT(LL_VERBOSE, "%s: info: channel %d: nq=%d; np=%d; N=%d", e->name, k, nq, np, cs->N);

		/* alloc filter structures */
		if (cs->n_real > 0)
			cs->real = calloc(cs->n_real, sizeof(struct riir_real));
		if (cs->n_cc > 0)
			cs->cc = calloc(cs->n_cc, sizeof(struct riir_cc));

		/* copy poles and residues */
		int idx_cc = 0, idx_real = 0;
		for (int i = 0; i < v->n; ++i) {
			struct riir_init_sec *sec = &v->sec[i];
			if (sec->pt == RIIR_PQ_TYPE_CC) {
				double complex p = sec->p.c, res = sec->res.c;
			#if RIIR_EXTRA_VERBOSE
				LOG_FMT(LL_VERBOSE, "%s: info: channel %d: pole=%g%+gi; residue=%g%+gi", e->name, k, PRINTF_CARGS(p), PRINTF_CARGS(res));
				LOG_FMT(LL_VERBOSE, "%s: info: channel %d: pole=%g%+gi; residue=%g%+gi", e->name, k, PRINTF_CARGS(conj(p)), PRINTF_CARGS(conj(res)));
			#endif
				cs->cc[idx_cc].res = res;
				cs->cc[idx_cc].s0.p = p;
				++idx_cc;
			}
			else {
				for (int j = 0; j < RIIR_PQ_N_EVAL(sec->pt); ++j) {
					double p = sec->p.r[j], res = sec->res.r[j];
				#if RIIR_EXTRA_VERBOSE
					LOG_FMT(LL_VERBOSE, "%s: info: channel %d: pole=%g; residue=%g", e->name, k, p, res);
				#endif
					cs->real[idx_real].res = res;
					cs->real[idx_real].s0.p = p;
					++idx_real;
				}
			}
		}

		/* initialize filter stages */
		INIT_FILTER_STAGES(cs->real, cs->n_real, cs->N);
		INIT_FILTER_STAGES(cs->cc, cs->n_cc, cs->N);

		/* initialize FIR delay buffer */
		if (cs->fir.n > 0) cs->fir.buf = calloc(1<<cs->N, sizeof(double));

		riir_init_state_clear(v);
		if (cascade_v.n) {
		#if RIIR_EXTRA_VERBOSE
			LOG_FMT(LL_VERBOSE, "%s: info: channel %d: cascade", e->name, k);
		#endif
			memcpy(v, &cascade_v, sizeof(struct riir_init_state));
			memset(&cascade_v, 0, sizeof(struct riir_init_state));
			cs = cs->cascade = calloc(1, sizeof(struct riir_state));
			goto recalc_cs;
		}

		/* compute total latency */
		for (cs = &state[k]; cs; cs = cs->cascade)
			state[k].latency += (1<<cs->N) + cs->fir.n - 1;
	}
	free(init_state);
	return 0;

	fail:
	for (int k = 0; k < e->istream.channels; ++k)
		riir_init_state_clear(&init_state[k]);
	free(init_state);
	return 1;
}

static void reverse_iir_effect_destroy_init(struct effect *e)
{
	struct riir_init_state *state = (struct riir_init_state *) e->data;
	for (int k = 0; k < e->istream.channels; ++k)
		riir_init_state_clear(&state[k]);
	free(state);
}

static struct effect * reverse_iir_effect_init_common(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const struct riir_init_sec *sec, int n)
{
	struct effect *e = calloc(1, sizeof(struct effect));
	e->name = ei->name;
	e->istream.fs = e->ostream.fs = istream->fs;
	e->istream.channels = e->ostream.channels = istream->channels;
	e->flags |= EFFECT_FLAG_OPT_REORDERABLE;
	e->flags |= EFFECT_FLAG_CH_DEPS_IDENTITY;
	e->prepare = reverse_iir_effect_prepare;
	e->run = reverse_iir_effect_run;
	e->reset = reverse_iir_effect_reset;
	e->plot = reverse_iir_effect_plot;
	e->drain_samples = reverse_iir_effect_drain_samples;
	e->destroy = reverse_iir_effect_destroy_init;
	e->merge = reverse_iir_effect_merge;
	e->channel_offsets = reverse_iir_effect_channel_offsets;

	struct riir_init_state *state = calloc(e->istream.channels, sizeof(struct riir_init_state));
	for (int k = 0; k < e->istream.channels; ++k) {
		if (GET_BIT(channel_selector, k))
			riir_init_state_append(&state[k], sec, n);
	}
	e->data = state;
	return e;
}

static int calc_qroots(double b, double c, qroots *r_roots)
{
	const double d = b*b-4.0*c;
	if (d < 0.0) {
		const double complex r = (csqrt(d)-b)/2.0;
		if (fabs(cimag(r)) < 1e-6)
			goto force_real;
		r_roots->c = r;
		return 1;
	}
	force_real:
	const double sd = sqrt(MAXIMUM(d, 0.0));
	r_roots->r[0] = (sd-b)/2.0;
	r_roots->r[1] = (-sd-b)/2.0;
	return 0;
}

struct effect * reverse_iir_effect_init_from_biquad(const struct effect_info *ei, const struct stream_info *istream, const char *channel_selector, const struct biquad_state *b, double thresh)
{
	struct riir_init_sec sec = {0};
	sec.thresh = thresh;
	sec.g = b->c0;
	/* find poles */
	if (b->c4 == 0.0) {
		if (b->c3 == 0.0) sec.pt = RIIR_PQ_TYPE_NONE;
		else {
			sec.pt = RIIR_PQ_TYPE_1R;
			sec.p.r[0] = -b->c3;
		}
	}
	else {
		const int cc = calc_qroots(b->c3, b->c4, &sec.p);
		sec.pt = (cc) ? RIIR_PQ_TYPE_CC : RIIR_PQ_TYPE_2R;
	}
	/* find zeros */
	if (b->c2 == 0.0) {
		if (b->c1 == 0.0) sec.qt = RIIR_PQ_TYPE_NONE;
		else {
			sec.qt = RIIR_PQ_TYPE_1R;
			sec.q.r[0] = -b->c1/b->c0;
		}
	}
	else {
		const int cc = calc_qroots(b->c1/b->c0, b->c2/b->c0, &sec.q);
		sec.qt = (cc) ? RIIR_PQ_TYPE_CC : RIIR_PQ_TYPE_2R;
	}
	return reverse_iir_effect_init_common(ei, istream, channel_selector, &sec, 1);
}
