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
#include <math.h>
#include <errno.h>
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

#include "align.h"

#define DO_EFFECTS_CHAIN_OPTIMIZE 1

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

void effects_chain_append(struct effects_chain *chain, struct effect *e)
{
	if (chain->tail == NULL)
		chain->head = e;
	else
		chain->tail->next = e;
	chain->tail = e;
	e->next = NULL;
}

static int build_effects_chain_block(int, const char *const *, struct effects_chain *, struct stream_info *, const char *, const char *);
static int build_effects_chain_block_from_file(const char *, struct effects_chain *, struct stream_info *, const char *, const char *, int);

static int build_effects_chain_block(int argc, const char *const *argv, struct effects_chain *chain, struct stream_info *stream, const char *initial_channel_mask, const char *dir)
{
	int i, k = 0, allow_fail = 0, last_stream_channels = stream->channels;
	char *channel_selector, *channel_mask;
	const char *last_channel_selector_str = NULL;
	const struct effect_info *ei = NULL;
	struct effect *e = NULL;

	channel_mask = NEW_SELECTOR(stream->channels);
	if (initial_channel_mask)
		COPY_SELECTOR(channel_mask, initial_channel_mask, stream->channels);
	else
		SET_SELECTOR(channel_mask, stream->channels);

	channel_selector = NEW_SELECTOR(stream->channels);
	COPY_SELECTOR(channel_selector, channel_mask, stream->channels);

	while (k < argc) {
		if (strcmp(argv[k], "!") == 0) {
			allow_fail = 1;
			++k;
			continue;
		}
		if (last_stream_channels != stream->channels) {  /* construct new channel mask */
			const int delta = stream->channels - last_stream_channels;
			char *tmp_mask = NEW_SELECTOR(stream->channels);
			if (delta > 0) {
				/* additional channels are appended */
				COPY_SELECTOR(tmp_mask, channel_mask, last_stream_channels);
				free(channel_mask);
				channel_mask = tmp_mask;
				for (int j = last_stream_channels; j < stream->channels; ++j)
					SET_BIT(channel_mask, j);
			}
			else {
				int nb = num_bits_set(channel_mask, last_stream_channels) + delta;
				for (int j = 0; j < stream->channels && nb > 0; ++j) {
					if (GET_BIT(channel_mask, j)) {
						SET_BIT(tmp_mask, j);
						--nb;
					}
				}
				free(channel_mask);
				channel_mask = tmp_mask;
			}
		}
		if (argv[k][0] == ':') {
			if (last_stream_channels != stream->channels) {
				free(channel_selector);
				channel_selector = NEW_SELECTOR(stream->channels);
				last_stream_channels = stream->channels;
			}
			if (parse_selector_masked(&argv[k][1], channel_selector, channel_mask, stream->channels))
				goto fail;
			last_channel_selector_str = &argv[k][1];
			++k;
			continue;
		}
		if (last_stream_channels != stream->channels) {  /* re-parse the channel selector */
			char *tmp_channel_selector = NEW_SELECTOR(stream->channels);
			if (last_channel_selector_str == NULL)
				COPY_SELECTOR(tmp_channel_selector, channel_mask, stream->channels);
			else if (parse_selector_masked(last_channel_selector_str, tmp_channel_selector, channel_mask, stream->channels)) {
				LOG_S(LL_VERBOSE, "note: the last effect changed the number of channels");
				free(tmp_channel_selector);
				goto fail;
			}
			free(channel_selector);
			channel_selector = tmp_channel_selector;
			last_stream_channels = stream->channels;
		}
		if (argv[k][0] == '@') {
			if (build_effects_chain_block_from_file(&argv[k][1], chain, stream, channel_selector, dir, 0))
				goto fail;
			++k;
			continue;
		}
		if (strcmp(argv[k], "{") == 0) {
			int bc = 1;
			for (i = k + 1; bc > 0 && i < argc; ++i) {
				if      (strcmp(argv[i], "{") == 0) ++bc;
				else if (strcmp(argv[i], "}") == 0) --bc;
			}
			if (bc > 0) {
				LOG_S(LL_ERROR, "error: missing '}'");
				goto fail;
			}
			if (build_effects_chain_block(i - k - 2, &argv[k + 1], chain, stream, channel_selector, dir))
				goto fail;
			k = i;
			continue;
		}
		if (strcmp(argv[k], "}") == 0) {
			LOG_S(LL_ERROR, "error: unexpected '}'");
			goto fail;
		}
		ei = get_effect_info(argv[k]);
		/* Find end of argument list */
		for (i = k + 1; i < argc && !IS_EFFECTS_CHAIN_START(argv[i]); ++i);
		if (ei == NULL) {
			if (allow_fail)
				LOG_FMT(LL_VERBOSE, "warning: no such effect: %s", argv[k]);
			else {
				LOG_FMT(LL_ERROR, "error: no such effect: %s", argv[k]);
				goto fail;
			}
		}
		else if (ei->init == NULL) {
			if (allow_fail)
				LOG_FMT(LL_VERBOSE, "warning: effect not available: %s", argv[k]);
			else {
				LOG_FMT(LL_ERROR, "error: effect not available: %s", argv[k]);
				goto fail;
			}
		}
		else {
			if (LOGLEVEL(LL_VERBOSE)) {
				dsp_log_acquire();
				dsp_log_printf("%s: effect:", dsp_globals.prog_name);
				for (int j = 0; j < i - k; ++j)
					dsp_log_printf(" %s", argv[k + j]);
				dsp_log_printf("; channels=%d [", stream->channels);
				print_selector(channel_selector, stream->channels);
				dsp_log_printf("] fs=%d\n", stream->fs);
				dsp_log_release();
			}
			e = ei->init(ei, stream, channel_selector, dir, i - k, &argv[k]);
			if (e == NULL) {
				if (allow_fail)
					LOG_FMT(LL_VERBOSE, "warning: failed to initialize non-essential effect: %s", argv[k]);
				else {
					LOG_FMT(LL_ERROR, "error: failed to initialize effect: %s", argv[k]);
					goto fail;
				}
			}
			while (e != NULL) {
				struct effect *e_n = e->next;
				if (e->run == NULL) {
					LOG_FMT(LL_VERBOSE, "info: not using effect: %s", argv[k]);
					destroy_effect(e);
				}
				else {
					effects_chain_append(chain, e);
					*stream = e->ostream;
				}
				e = e_n;
			}
		}
		allow_fail = 0;
		k = i;
	}
	free(channel_selector);
	free(channel_mask);
	return 0;

	fail:
	free(channel_selector);
	free(channel_mask);
	return 1;
}

static int build_effects_chain_block_from_file(const char *path, struct effects_chain *chain, struct stream_info *stream, const char *channel_mask, const char *dir, int enforce_eof_marker)
{
	char **argv = NULL, *tmp, *d = NULL, *p, *c;
	int i, ret = 0, argc = 0;

	p = construct_full_path(dir, path, stream);
	if (!(c = get_file_contents(p))) {
		LOG_FMT(LL_ERROR, "error: failed to load effects file: %s: %s", p, strerror(errno));
		goto fail;
	}
	if (enforce_eof_marker) {
		const ssize_t l = LENGTH(EFFECTS_FILE_EOF_MARKER)-1;
		ssize_t k = strlen(c);
		while (k > l && IS_WHITESPACE(c[k-1])) --k;
		if (k < l || strncmp(&c[k-l], EFFECTS_FILE_EOF_MARKER, l) != 0 || (k > l && c[k-l-1] != '\n')) {
			LOG_FMT(LL_ERROR, "error: no valid end-of-file marker: %s", p);
			goto fail;
		}
	}
	if (gen_argv_from_string(c, &argc, &argv))
		goto fail;
	d = strdup(p);
	tmp = strrchr(d, '/');
	if (tmp == NULL) {
		free(d);
		d = strdup(".");
	}
	else
		*tmp = '\0';
	LOG_FMT(LL_VERBOSE, "info: begin effects file: %s", p);
	if (build_effects_chain_block(argc, (const char *const *) argv, chain, stream, channel_mask, d))
		goto fail;
	LOG_FMT(LL_VERBOSE, "info: end effects file: %s", p);
	done:
	free(c);
	free(p);
	free(d);
	for (i = 0; i < argc; ++i)
		free(argv[i]);
	free(argv);
	return ret;

	fail:
	ret = 1;
	goto done;
}

static void effects_chain_optimize(struct effects_chain *chain)
{
#if DO_EFFECTS_CHAIN_OPTIMIZE
	ssize_t chain_len = 0, chain_len_opt = 0;
	for (struct effect *e = chain->head; e; e = e->next) ++chain_len;
	struct effect *m_dest = chain->head;
	while (m_dest) {
		if (m_dest->merge) {
			struct effect *prev = m_dest;
			struct effect *m_src = m_dest->next;
			while (m_src) {
				if (m_src->istream.fs != m_dest->istream.fs
					|| m_src->istream.channels != m_dest->istream.channels
					|| m_src->ostream.fs != m_dest->ostream.fs
					|| m_src->ostream.channels != m_dest->ostream.channels
					) break;
				if (m_src->merge == NULL) {
					if (m_src->flags & EFFECT_FLAG_OPT_REORDERABLE) goto skip;
					break;
				}
				if (m_dest->merge(m_dest, m_src)) {
					/* LOG_FMT(LL_VERBOSE, "optimize: merged effect: %s <- %s", m_dest->name, m_src->name); */
					struct effect *tmp = m_src->next;
					destroy_effect(m_src);
					if (tmp == NULL) chain->tail = prev;
					prev->next = m_src = tmp;
				}
				else {
					skip:
					prev = m_src;
					m_src = m_src->next;
				}
			}
		}
		m_dest = m_dest->next;
	}
	for (struct effect *e = chain->head; e; e = e->next) ++chain_len_opt;
	if (chain_len_opt < chain_len)
		LOG_FMT(LL_VERBOSE, "optimize: info: reduced number of effects from %zd to %zd", chain_len, chain_len_opt);
#else
	return;
#endif
}

struct effects_chain_postproc_state {
	char **ch_deps;
	ssize_t *samples[4];
	int max_in_ch, max_out_ch, max_ch;
};

static int effects_chain_postproc_state_init(struct effects_chain_postproc_state *state, struct effects_chain *chain)
{
	for (struct effect *e = chain->head; e; e = e->next) {
		state->max_in_ch = MAXIMUM(state->max_in_ch, e->istream.channels);
		state->max_out_ch = MAXIMUM(state->max_out_ch, e->ostream.channels);
	}
	state->max_ch = MAXIMUM(state->max_in_ch, state->max_out_ch);

	state->ch_deps = calloc(state->max_out_ch, sizeof(char *));
	for (int i = 0; i < state->max_out_ch; ++i)
		state->ch_deps[i] = NEW_SELECTOR(state->max_in_ch);
	for (int i = 0; i < LENGTH(state->samples); ++i)
		state->samples[i] = calloc(state->max_ch, sizeof(ssize_t));

	return 0;
}

static void effects_chain_postproc_state_cleanup(struct effects_chain_postproc_state *state)
{
	if (state->ch_deps) {
		for (int i = 0; i < state->max_out_ch; ++i)
			free(state->ch_deps[i]);
		free(state->ch_deps);
	}
	for (int i = 0; i < LENGTH(state->samples); ++i)
		free(state->samples[i]);
}

static int query_channel_deps(struct effects_chain_postproc_state *state, struct effects_chain *chain, struct effect *e)
{
	if (e->channel_deps) {
		for (int i = 0; i < state->max_out_ch; ++i)
			CLEAR_SELECTOR(state->ch_deps[i], state->max_in_ch);
		/* set identity as initial state */
		const int min_ch = MINIMUM(e->istream.channels, e->ostream.channels);
		for (int i = 0; i < min_ch; ++i)
			SET_BIT(state->ch_deps[i], i);
		e->channel_deps(e, state->ch_deps);
		return 1;
	}
	return 0;
}

/* FIXME: Seems to work, but could probably be done in a better way... */
static void find_input_deps(int ch, char **ch_deps, int n_in, int n_out, char *r_deps)
{
	CLEAR_SELECTOR(r_deps, n_in);
	SET_BIT(r_deps, ch);
	restart:
	for (int i = 0; i < n_out; ++i) {
		int mod = 0;
		for (int k = 0; k < n_in; ++k) {
			if (GET_BIT(r_deps, k) && GET_BIT(ch_deps[i], k))
				goto has_dep;
		}
		continue;
		has_dep:
		for (int k = 0; k < n_in; ++k) {
			if (GET_BIT(r_deps, k)) continue;
			if (GET_BIT(ch_deps[i], k)) {
				SET_BIT(r_deps, k);
				mod = 1;
			}
		}
		if (mod && i > 0) goto restart;
	}
}

static int effects_chain_align_channels(struct effects_chain_postproc_state *state, struct effects_chain *chain)
{
	int ret = 0;
	char *in_deps = NEW_SELECTOR(state->max_ch);
	char *in_deps_all = NEW_SELECTOR(state->max_ch);

	chain->delay_offset = 0;
	ssize_t *offsets = state->samples[0], *delays = state->samples[1];
	memset(offsets, 0, state->max_ch * sizeof(ssize_t));
	memset(delays, 0, state->max_ch * sizeof(ssize_t));

	struct effect *e = chain->head, *prev = NULL;
	while (e) {
		const int is_passthrough = (e->istream.channels == e->ostream.channels
			&& e->flags & (EFFECT_FLAG_CH_DEPS_IDENTITY|EFFECT_FLAG_OPT_REORDERABLE));
		const int have_ch_deps = query_channel_deps(state, chain, e);
		if (prev) {
			/* align channels */
			if (e->flags & EFFECT_FLAG_ALIGN_BARRIER) {
				if (align_effect_insert(e, prev, offsets, NULL))
					goto fail;
			}
			else if (have_ch_deps) {
				CLEAR_SELECTOR(in_deps_all, e->istream.channels);
				ssize_t *align_refs = state->samples[2];
				memcpy(align_refs, offsets, e->istream.channels);
				/* find channels which need to be aligned */
				for (int k = 0; k < e->istream.channels; ++k) {
					if (GET_BIT(in_deps_all, k)) continue;  /* already did channel k */
					find_input_deps(k, state->ch_deps, e->istream.channels, e->ostream.channels, in_deps);
					ssize_t max_offset = offsets[k];
					for (int i = 0; i < e->istream.channels; ++i) {
						if (GET_BIT(in_deps, i)) {
							SET_BIT(in_deps_all, i);
							max_offset = MAXIMUM(max_offset, offsets[i]);
						}
					}
					for (int i = 0; i < e->istream.channels; ++i)
						if (GET_BIT(in_deps, i)) align_refs[i] = max_offset;
				}
				if (align_effect_insert(e, prev, offsets, align_refs))
					goto fail;
			}
			else if (e->istream.fs != e->ostream.fs) {
				LOG_FMT(LL_VERBOSE, "info: %s: sample rate changed; doing full alignment", e->name);
				if (align_effect_insert(e, prev, offsets, NULL))
					goto fail;
			}
			else if (!is_passthrough) {
				LOG_FMT(LL_VERBOSE, "warning: %s: channel deps unknown; doing full alignment", e->name);
				if (align_effect_insert(e, prev, offsets, NULL))
					goto fail;
			}
		}
		/* find initial output offsets and delays */
		if (have_ch_deps) {
			#if 0
				dsp_log_acquire();
				dsp_log_printf("%s(): channel deps map:\n", __func__);
				for (int i = 0; i < e->ostream.channels; ++i) {
					for (int k = 0; k < e->istream.channels; ++k)
						dsp_log_printf("  %d", GET_BIT(state->ch_deps[i], k));
					dsp_log_printf("\n");
				}
				dsp_log_release();
			#endif
			ssize_t *tmp_offsets = state->samples[2], *tmp_delays = state->samples[3];
			memcpy(tmp_offsets, offsets, e->istream.channels * sizeof(ssize_t));
			memcpy(tmp_delays, delays, e->istream.channels * sizeof(ssize_t));
			ssize_t max_offset = 0;
			for (int k = 0; k < e->istream.channels; ++k)
				max_offset = MAXIMUM(max_offset, tmp_offsets[k]);
			for (int i = 0; i < e->ostream.channels; ++i) {
				int offset_idx = -1;
				delays[i] = 0;
				for (int k = 0; k < e->istream.channels; ++k) {
					if (GET_BIT(state->ch_deps[i], k)) {
						if (offset_idx < 0) {
							offset_idx = k;
							delays[i] = tmp_delays[k];
						}
						else if (tmp_offsets[k] != tmp_offsets[offset_idx]) {
							LOG_FMT(LL_ERROR, "%s(): BUG: channel %d offset incorrect: %zd!=%zd",
								__func__, k, tmp_offsets[k], tmp_offsets[offset_idx]);
							goto fail;
						}
						else delays[i] = MINIMUM(delays[i], tmp_delays[k]);
					}
				}
				offsets[i] = (offset_idx >= 0) ? tmp_offsets[offset_idx] : max_offset;
			}
		}
		else if (!is_passthrough) {
			ssize_t min_delay = delays[0];
			for (int k = 1; k < e->istream.channels; ++k) {
				min_delay = MINIMUM(min_delay, delays[k]);
				if (offsets[k] != offsets[k-1]) {
					LOG_FMT(LL_ERROR, "%s(): BUG: channel %d offset incorrect: %zd!=%zd",
						__func__, k, offsets[k], offsets[k-1]);
					goto fail;
				}
			}
			for (int i = 0; i < e->ostream.channels; ++i)
				delays[i] = min_delay;
		}
		for (int i = e->ostream.channels; i < e->istream.channels; ++i)
			delays[i] = offsets[i] = 0;
		/* recalculate offsets */
		for (int i = 0; i < e->ostream.channels; ++i)
			offsets[i] += delays[i]-chain->delay_offset;  /* cumulative latency */
		if (e->channel_offsets)  /* query effect latency and requested delay */
			e->channel_offsets(e, offsets, delays);
		chain->delay_offset = 0;
		for (int i = 0; i < e->ostream.channels; ++i)
			chain->delay_offset = MINIMUM(chain->delay_offset, delays[i]);
		/* LOG_FMT(LL_VERBOSE, "%s(): delay_offset=%zd", __func__, chain->delay_offset); */
		for (int i = 0; i < e->ostream.channels; ++i) {
			/* LOG_FMT(LL_VERBOSE, "%s(): output channel %d: offset=%zd latency=%zd delay=%zd",
				__func__, i, offsets[i]-(delays[i]-chain->delay_offset), offsets[i], delays[i]); */
			offsets[i] -= delays[i]-chain->delay_offset;
		}

		prev = e;
		e = e->next;
	}
	if (prev && align_effect_insert(e, prev, offsets, NULL))
		goto fail;

	done:
	free(in_deps_all);
	free(in_deps);
	return ret;

	fail:
	ret = 1;
	goto done;
}

static void effects_chain_set_drain_frames(struct effects_chain_postproc_state *state, struct effects_chain *chain)
{
	ssize_t *samples = state->samples[0];
	memset(samples, 0, state->max_ch * sizeof(ssize_t));
	for (struct effect *e = chain->head; e; e = e->next) {
		if (query_channel_deps(state, chain, e)) {
			ssize_t *tmp_samples = state->samples[1];
			memcpy(tmp_samples, samples, state->max_ch * sizeof(ssize_t));
			for (int i = 0; i < e->ostream.channels; ++i) {
				ssize_t ch_drain = 0;
				for (int k = 0; k < e->istream.channels; ++k) {
					if (GET_BIT(state->ch_deps[i], k))
						ch_drain = MAXIMUM(ch_drain, tmp_samples[k]);
				}
				samples[i] = ch_drain;
			}
		}
		else if (!(e->flags & (EFFECT_FLAG_CH_DEPS_IDENTITY|EFFECT_FLAG_OPT_REORDERABLE))
				&& e->istream.channels != e->ostream.channels) {
			/* effect does not drain, but channel deps unknown */
			ssize_t drain_frames = 0;
			for (int i = 0; i < e->istream.channels; ++i)
				drain_frames = MAXIMUM(drain_frames, samples[i]);
			for (int i = 0; i < e->ostream.channels; ++i)
				samples[i] = drain_frames;
		}
		if (e->drain_samples)
			e->drain_samples(e, samples);
		if (!e->drain_samples && e->ostream.fs != e->istream.fs) {
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			const int ratio_n = e->ostream.fs/gcd, ratio_d = e->istream.fs/gcd;
			for (int i = 0; i < e->ostream.channels; ++i)
				samples[i] = ratio_mult_ceil(samples[i], ratio_n, ratio_d);
		}
		for (int i = e->ostream.channels; i < e->istream.channels; ++i)
			samples[i] = 0;
	}
	chain->drain_frames = 0;
	for (int i = 0; i < chain->tail->ostream.channels; ++i)
		chain->drain_frames = MAXIMUM(chain->drain_frames, samples[i]);
	if (chain->head->istream.fs != chain->tail->ostream.fs) {
		const int gcd = find_gcd(chain->head->istream.fs, chain->tail->ostream.fs);
		chain->drain_frames = (long long int) chain->drain_frames *
			(chain->head->istream.fs / gcd) / (chain->tail->ostream.fs / gcd);
	}
	LOG_FMT(LL_VERBOSE, "info: input drain frames: %zd", chain->drain_frames);
}

static int effects_chain_prepare(struct effects_chain *chain)
{
	for (struct effect *e = chain->head; e; e = e->next)
		if (e->prepare && e->prepare(e)) return 1;
	return 0;
}

static int build_effects_chain_finish(struct effects_chain *chain)
{
	if (chain->head == NULL) return 0;
	struct effects_chain_postproc_state state = {0};

	effects_chain_optimize(chain);
	if (effects_chain_prepare(chain)) goto fail;
	if (effects_chain_postproc_state_init(&state, chain)) goto fail;
	if (effects_chain_align_channels(&state, chain)) goto fail;
	effects_chain_set_drain_frames(&state, chain);
	effects_chain_postproc_state_cleanup(&state);
	return 0;

	fail:
	effects_chain_postproc_state_cleanup(&state);
	return 1;
}

int build_effects_chain(int argc, const char *const *argv, struct effects_chain *chain, struct stream_info *stream, const char *dir)
{
	if (build_effects_chain_block(argc, argv, chain, stream, NULL, dir))
		return 1;
	return build_effects_chain_finish(chain);
}

int build_effects_chain_from_file(const char *path, struct effects_chain *chain, struct stream_info *stream, const char *channel_mask, const char *dir, int enforce_eof_marker)
{
	if (build_effects_chain_block_from_file(path, chain, stream, channel_mask, dir, enforce_eof_marker))
		return 1;
	return build_effects_chain_finish(chain);
}

static ssize_t effect_max_out_frames(struct effect *e, ssize_t in_frames)
{
	if (e->buffer_frames != NULL)
		return e->buffer_frames(e, in_frames);
	if (e->ostream.fs != e->istream.fs) {
		const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
		return ratio_mult_ceil(in_frames, e->ostream.fs / gcd, e->istream.fs / gcd);
	}
	return in_frames;
}

ssize_t get_effects_chain_buffer_len(struct effects_chain *chain, ssize_t in_frames, int in_channels)
{
	ssize_t frames = in_frames, len, max_len = in_frames * in_channels;
	struct effect *e = chain->head;
	while (e != NULL) {
		frames = effect_max_out_frames(e, frames);
		len = frames * e->ostream.channels;
		if (len  > max_len) max_len = len;
		e = e->next;
	}
	return max_len;
}

ssize_t get_effects_chain_max_out_frames(struct effects_chain *chain, ssize_t in_frames)
{
	ssize_t frames = in_frames;
	struct effect *e = chain->head;
	while (e != NULL) {
		frames = effect_max_out_frames(e, frames);
		e = e->next;
	}
	return frames;
}

int effects_chain_needs_dither(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		if (!(e->flags & EFFECT_FLAG_NO_DITHER) && !effect_is_dither(e))
			return 1;
		e = e->next;
	}
	return 0;
}

int effects_chain_set_dither_params(struct effects_chain *chain, int prec, int enabled)
{
	struct effect *e = chain->head;
	int r = 1;
	while (e != NULL) {
		if (effect_is_dither(e)) {
			dither_effect_set_params(e, prec, enabled);
			r = 0;
		}
		else if (!(e->flags & EFFECT_FLAG_NO_DITHER)) r = 1;
		e = e->next;
	}
	return r && enabled;  /* note: non-zero return value means dither should be added */
}

static sample_t * run_effect_list(struct effect *e, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	sample_t *ibuf = buf1, *obuf = buf2, *tmp;
	while (e != NULL && *frames > 0) {
		tmp = e->run(e, frames, ibuf, obuf);
		if (tmp == obuf) {
			obuf = ibuf;
			ibuf = tmp;
		}
		e = e->next;
	}
	return ibuf;
}

sample_t * run_effects_chain(struct effects_chain *chain, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	if (*frames < 1)
		return buf1;
	chain->frames += *frames;
	return run_effect_list(chain->head, frames, buf1, buf2);
}

double get_effects_chain_delay(struct effects_chain *chain)
{
	int out_fs = 0;
	double delay = 0.0, delay_lim = 0.0;
	struct effect *e = chain->head;
	if (e) delay_lim = (double) chain->frames / e->istream.fs;
	while (e) {
		out_fs = e->ostream.fs;
		if (e->delay)
			delay += (double) e->delay(e) / out_fs;
		e = e->next;
	}
	if (out_fs && chain->delay_offset < 0)
		delay += (double) -chain->delay_offset / out_fs;
	return MINIMUM(delay, delay_lim);
}

void reset_effects_chain(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->reset != NULL) e->reset(e);
		e = e->next;
	}
	chain->frames = 0;
}

void signal_effects_chain(struct effects_chain *chain)
{
	struct effect *e = chain->head;
	while (e != NULL) {
		if (e->signal != NULL) e->signal(e);
		e = e->next;
	}
}

static const char gnuplot_header[] =
	"set xlabel 'Frequency (Hz)'\n"
	"set ylabel 'Magnitude (dB)'\n"
	"set logscale x\n"
	/* "set format x '10^{%L}'\n" */  /* problematic when zooming */
	"set samples 500\n"
	"set mxtics\n"
	"set mytics\n"
	"set grid xtics ytics mxtics mytics lw 0.8, lw 0.3\n"
	"set key on\n"
	"j={0,1}\n"
	"\n"
	"set yrange [-30:20]\n";

static const char gnuplot_header_phase[] =
	"set ytics nomirror\n"
	"set y2tics -180,90,180 format '%g°'\n"
	"set y2range [-180:720]\n";

void plot_effects_chain(struct effects_chain *chain, int input_fs, int input_channels, int plot_phase)
{
	struct effect *e = chain->head;
	int fs = input_fs;
	while (e != NULL) {
		if (e->plot == NULL) {
			LOG_FMT(LL_ERROR, "plot: error: effect '%s' does not support plotting", e->name);
			return;
		}
		if (e->istream.channels != e->ostream.channels && !(e->flags & EFFECT_FLAG_PLOT_MIX)) {
			LOG_FMT(LL_ERROR, "plot: BUG: effect '%s' changed the number of channels but does not have EFFECT_FLAG_PLOT_MIX set!", e->name);
			return;
		}
		fs = e->ostream.fs;
		e = e->next;
	}
	printf("%sset xrange [10:%d/2]\n%s\n",
		gnuplot_header, fs, (plot_phase)?gnuplot_header_phase:"");
	struct effect *start_e = chain->head;
	e = start_e;
	int channels = input_channels, start_idx = 0;
	for (int i = 0; e != NULL; ++i) {
		if (e->flags & EFFECT_FLAG_PLOT_MIX) {
			for (int k = 0; k < e->istream.channels; ++k) {
				printf("Ht%d_%d(f)=1.0", k, i);
				struct effect *e2 = start_e;
				for (int j = start_idx; e2 != NULL && e2 != e; ++j) {
					printf("*H%d_%d(2.0*pi*f/%d)", k, j, e2->ostream.fs);
					e2 = e2->next;
				}
				putchar('\n');
			}
			start_idx = i;
			start_e = e;
			channels = e->ostream.channels;
		}
		e->plot(e, i);
		e = e->next;
	}
	for (int k = 0; k < channels; ++k) {
		printf("Ht%d(f)=1.0", k);
		e = start_e;
		for (int i = start_idx; e != NULL; ++i) {
			printf("*H%d_%d(2.0*pi*f/%d)", k, i, e->ostream.fs);
			e = e->next;
		}
		putchar('\n');
		printf("Ht%d_mag(f)=abs(Ht%d(f))\n", k, k);
		printf("Ht%d_mag_dB(f)=20*log10(Ht%d_mag(f))\n", k, k);
		printf("Ht%d_phase(f)=arg(Ht%d(f))\n", k, k);
		printf("Ht%d_phase_deg(f)=Ht%d_phase(f)*180/pi\n", k, k);
		printf("Hsum%d(f)=Ht%d_mag_dB(f)\n", k, k);
	}
	printf("\nplot ");
	for (int k = 0; k < channels; ++k) {
		printf("%sHt%d_mag_dB(x) lt %d lw 2 title 'Channel %d'", (k==0)?"":", ", k, k+1, k);
		if (plot_phase)
			printf(", Ht%d_phase_deg(x) axes x1y2 lt %d lw 1 dt '-' notitle", k, k+1);
	}
	puts("\npause mouse close");
}

void effect_plot_noop(struct effect *e, int i)
{
	for (int k = 0; k < e->istream.channels; ++k)
		printf("H%d_%d(f)=1.0\n", k, i);
}

sample_t * drain_effects_chain(struct effects_chain *chain, ssize_t *frames, sample_t *buf1, sample_t *buf2)
{
	struct effect *e = chain->head;
	if (e == NULL || chain->frames < 1 || *frames < 1) {
		*frames = -1;
		return buf1;
	}
	if (chain->drain_frames > 0) {
		*frames = MINIMUM(*frames, chain->drain_frames);
		chain->drain_frames -= *frames;
		memset(buf1, 0, *frames * e->istream.channels * sizeof(sample_t));
		return run_effect_list(e, frames, buf1, buf2);
	}
	ssize_t ftmp = *frames, dframes = -1;
	while (e != NULL && dframes == -1) {
		dframes = ftmp;
		if (e->drain2 != NULL) {
			sample_t *rbuf = e->drain2(e, &dframes, buf1, buf2);
			if (rbuf == buf2) {
				buf2 = buf1;
				buf1 = rbuf;
			}
		}
		else dframes = -1;
		if (e->ostream.fs != e->istream.fs) {
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			ftmp = ratio_mult_ceil(ftmp, e->ostream.fs / gcd, e->istream.fs / gcd);
		}
		e = e->next;
	}
	*frames = dframes;
	return run_effect_list(e, frames, buf1, buf2);
}

void destroy_effects_chain(struct effects_chain *chain)
{
	struct effect *e;
	while (chain->head != NULL) {
		e = chain->head;
		chain->head = e->next;
		destroy_effect(e);
	}
	chain->tail = NULL;
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

void effects_chain_xfade_reset(struct effects_chain_xfade_state *state)
{
	state->chain[0] = EFFECTS_CHAIN_INITIALIZER;
	state->chain[1] = EFFECTS_CHAIN_INITIALIZER;
	state->pos = 0;
	state->has_output = 0;
}

static inline double xfade_mult(ssize_t pos, ssize_t n) { return (double) (n-pos) / n; }

sample_t * effects_chain_xfade_run(struct effects_chain_xfade_state *state, ssize_t *frames, sample_t *ibuf, sample_t *obuf)
{
	ssize_t tmp_frames = *frames, adj_xfade_frames = state->frames;
	sample_t *rbuf[2];

	memcpy(state->buf, ibuf, *frames * state->istream.channels * sizeof(sample_t));
	rbuf[0] = run_effects_chain(&state->chain[0], frames, ibuf, obuf);
	rbuf[1] = (rbuf[0] == obuf) ? ibuf : obuf;
	rbuf[1] = run_effects_chain(&state->chain[1], &tmp_frames, state->buf, rbuf[1]);

	const ssize_t min_frames = MINIMUM(*frames, tmp_frames);
	const ssize_t offset_samples = (state->has_output) ? 0 : (*frames-min_frames)*state->ostream.channels;
	if (state->has_output && *frames != tmp_frames) {
		if (min_frames < state->pos) {
			adj_xfade_frames = lround((double) min_frames / state->pos * state->frames);
			/* LOG_FMT(LL_VERBOSE, "%s(): truncated crossfade: %zd -> %zd", __func__, state->frames, adj_xfade_frames); */
			state->pos = min_frames;
		}
		*frames = tmp_frames;
	}
	if (tmp_frames > 0) state->has_output = 1;

	const ssize_t xfade_samples = MINIMUM(state->pos, min_frames) * state->ostream.channels;
	for (size_t i = 0; i < xfade_samples; i += state->ostream.channels) {
		const double m = xfade_mult(state->pos--, adj_xfade_frames);
		for (int k = 0; k < state->ostream.channels; ++k)
			rbuf[0][i+offset_samples+k] = rbuf[1][i+k]*m + rbuf[0][i+offset_samples+k]*(1.0-m);
	}
	const ssize_t rem_samples = tmp_frames*state->ostream.channels - xfade_samples;
	if (rem_samples > 0)
		memcpy(&rbuf[0][offset_samples+xfade_samples], &rbuf[1][xfade_samples], rem_samples*sizeof(sample_t));

	return rbuf[0];
}
