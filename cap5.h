#ifndef DSP_CAP5_H
#define DSP_CAP5_H

#include "dsp.h"

struct ap1_state {
	sample_t c0;
	sample_t i0, o0;
};

struct ap2_state {
	sample_t c0, c1;
	sample_t i0, i1, o0, o1;
};

struct ap3_state {
	struct ap2_state ap2;
	struct ap1_state ap1;
};

struct cap5_state {
	struct ap2_state a1;
	struct ap3_state a2;
};

void ap1_reset(struct ap1_state *);
void ap2_reset(struct ap2_state *);
void ap3_reset(struct ap3_state *);
void cap5_reset(struct cap5_state *);
void cap5_init(struct cap5_state *, double, double);

static __inline__ sample_t ap1_run(struct ap1_state *state, sample_t s)
{
	sample_t r = state->c0 * (s - state->o0)
		+ state->i0;

	state->i0 = s;
	state->o0 = r;

	return r;
}

static __inline__ sample_t ap2_run(struct ap2_state *state, sample_t s)
{
	sample_t r = state->c1 * (s - state->o1)
		+ state->c0 * (state->i0 - state->o0)
		+ state->i1;

	state->i1 = state->i0;
	state->i0 = s;

	state->o1 = state->o0;
	state->o0 = r;

	return r;
}

static __inline__ sample_t ap3_run(struct ap3_state *state, sample_t s)
{
	return ap1_run(&state->ap1, ap2_run(&state->ap2, s));
}

static __inline__ void cap5_run(struct cap5_state *state, sample_t s, sample_t *lp, sample_t *hp)
{
	sample_t a1 = ap2_run(&state->a1, s);
	sample_t a2 = ap3_run(&state->a2, s);
	*lp = (a1+a2)*0.5;
	*hp = (a1-a2)*0.5;
}

#endif
