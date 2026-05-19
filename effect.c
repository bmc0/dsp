/*
 * This file is part of dsp.
 *
 * Copyright (c) 2013-2026 Michael Barbour <barbour.michael.0@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "effect.h"
#include "util.h"

#include "biquad.h"
#include "gain.h"
#include "crossfeed.h"
#include "matrix4.h"
#include "matrix4_mb.h"
#include "remix.h"
#include "st2ms.h"
#include "delay.h"
#include "resample.h"
#include "fir.h"
#include "fir_p.h"
#include "zita_convolver.h"
#include "hilbert.h"
#include "decorrelate.h"
#include "noise.h"
#include "dither.h"
#include "ladspa_host.h"
#include "stats.h"
#include "watch.h"
#include "levels.h"

static const struct effect_info effects[] = {
	BIQUAD_EFFECT_INFO,
	GAIN_EFFECT_INFO,
	CROSSFEED_EFFECT_INFO,
	MATRIX4_EFFECT_INFO,
	MATRIX4_MB_EFFECT_INFO,
	REMIX_EFFECT_INFO,
	ST2MS_EFFECT_INFO,
	DELAY_EFFECT_INFO,
	RESAMPLE_EFFECT_INFO,
	FIR_EFFECT_INFO,
	FIR_P_EFFECT_INFO,
	ZITA_CONVOLVER_EFFECT_INFO,
	HILBERT_EFFECT_INFO,
	DECORRELATE_EFFECT_INFO,
	NOISE_EFFECT_INFO,
	DITHER_EFFECT_INFO,
	LADSPA_HOST_EFFECT_INFO,
	STATS_EFFECT_INFO,
	WATCH_EFFECT_INFO,
	LEVELS_EFFECT_INFO,
};

const struct effect_info * get_effect_info(const char *name)
{
	int i;
	for (i = 0; i < LENGTH(effects); ++i)
		if (strcmp(name, effects[i].name) == 0)
			return &effects[i];
	return NULL;
}

void destroy_effect(struct effect *e)
{
	if (e == NULL)
		return;
	if (e->destroy != NULL)
		e->destroy(e);
	free(e);
}

void effect_list_append(struct effect *list, struct effect *e)
{
	while (list) {
		if (!list->next) {
			list->next = e;
			break;
		}
		list = list->next;
	}
}

void effect_plot_noop(struct effect *e, int i)
{
	for (int k = 0; k < e->istream.channels; ++k)
		printf("H%d_%d(f)=1.0\n", k, i);
}

void print_all_effects(void)
{
	fprintf(stdout, "Effects:\n");
	for (int i = 0; i < LENGTH(effects); ++i)
		fprintf(stdout, "  %s %s\n", effects[i].name, (effects[i].init) ? effects[i].usage : "(not available)");
}

void print_effect_usage(const struct effect_info *ei)
{
	LOG_FMT(LL_ERROR, "%s: usage: %s %s", ei->name, ei->name, ei->usage);
}
