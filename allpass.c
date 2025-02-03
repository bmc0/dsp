/*
 * This file is part of dsp.
 *
 * Copyright (c) 2025 Michael Barbour <barbour.michael.0@gmail.com>
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
#include "allpass.h"

struct thiran_ap_state * thiran_ap_new(int n, double delay)
{
	if (n < 1 || delay <= n-1)  /* unstable if delay <= n-1 */
		return NULL;
	struct thiran_ap_state * state = calloc(1, sizeof(struct thiran_ap_state) + n*sizeof(state->fb[0]));
	if (!state) return NULL;
	state->n = n;
	for (int k = 0; k < n; ++k) {
		state->fb[k].c0 = delay - k;
		state->fb[k].c1 = -1.0 / (delay + (k+1));
		state->fb[k].c2 = 2*k + 1;
	}
	return state;
}

void thiran_ap_plot(struct thiran_ap_state *state)
{
	printf("((abs(w)<=pi)?(1.0");
	for (int k = 0; k < state->n; ++k)
		printf("+%.15e/(%.15e*(exp(-j*w)/(1.0-exp(-j*w)))+%.15e/(2.0",
			state->fb[k].c0, -state->fb[k].c2, 1.0/state->fb[k].c1);
	for (int k = 0; k < state->n; ++k) printf("))");
	printf("):0/0)");
}
