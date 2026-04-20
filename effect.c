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
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include "effect.h"
#include "util.h"
#include "list_util.h"

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
	LIST_APPEND(chain, e);
}

enum ec_token_id {
	EC_TOKEN_LITERAL = 0,
	EC_TOKEN_ESC_LITERAL,
	EC_TOKEN_CH_SEL,
	EC_TOKEN_BLOCK_START,
	EC_TOKEN_BLOCK_END,
	EC_TOKEN_SOURCE,
	EC_TOKEN_ALLOW_FAIL,
};

struct ec_token {
	struct ec_token *prev, *next;
	enum ec_token_id id;
	int line, col, len;
	char str[];
};

struct ec_token_list {
	struct ec_token *head, *tail;
};

static enum ec_token_id ec_get_token_id(const char *s)
{
	if (s[0] == ':')
		return EC_TOKEN_CH_SEL;
	else if (s[0] == '{' && s[1] == '\0')
		return EC_TOKEN_BLOCK_START;
	else if (s[0] == '}' && s[1] == '\0')
		return EC_TOKEN_BLOCK_END;
	else if (s[0] == '@' && s[1] != '\0')
		return EC_TOKEN_SOURCE;
	else if (s[0] == '!' && s[1] == '\0')
		return EC_TOKEN_ALLOW_FAIL;
	return EC_TOKEN_LITERAL;
}

int is_effect_or_token(const char *s)
{
	if (ec_get_token_id(s) == EC_TOKEN_LITERAL)
		return (get_effect_info(s) != NULL);
	return 1;
}

static int ec_lex_word(struct ec_token_list *tokens, const char *s, int line, int col, int len)
{
	size_t slen = 0;
	enum ec_token_id id = EC_TOKEN_ESC_LITERAL;
	if (*s == '\\') ++s;
	else id = ec_get_token_id(s);
	switch (id) {
	case EC_TOKEN_CH_SEL:
	case EC_TOKEN_SOURCE:
		++s;
	case EC_TOKEN_ESC_LITERAL:
	case EC_TOKEN_LITERAL:
		slen = strlen(s);
	default: break;
	}
	struct ec_token *tok = calloc(1, sizeof(struct ec_token)+slen+1);
	if (!tok) {
		dsp_perror(DSP_ENOMEM, NULL);
		return 1;
	}
	tok->id = id;
	tok->line = line;
	tok->col = col;
	tok->len = len;
	if (slen > 0) memcpy(tok->str, s, slen);
	LIST_APPEND(tokens, tok);
	return 0;
}

static void ec_token_list_destroy(struct ec_token_list *tokens)
{
	while (tokens->head) {
		struct ec_token *tok = tokens->head;
		LIST_REMOVE(tokens, tok);
		free(tok);
	}
}

static void ec_print_escaped_str(const char *s, int max_len)
{
	const char *end = (max_len > 0) ? s + max_len : NULL;
	while (*s && s != end) {
		const char *esc = NULL;
		switch (*s) {
		case '\a': esc = "\\a"; break;
		case '\b': esc = "\\b"; break;
		case '\t': esc = "\\t"; break;
		case '\n': esc = "\\n"; break;
		case '\v': esc = "\\v"; break;
		case '\f': esc = "\\f"; break;
		case '\r': esc = "\\r"; break;
		}
		if (esc) dsp_log_puts(esc);
		else {
			if (iscntrl(*s)) dsp_log_printf("\\%03o", *s);
			else dsp_log_putc(*s);
		}
		++s;
	}
	if (s == end && *s != '\0')
		dsp_log_puts("...");
}

static void ec_print_line(const char *reason, const char *path, const char *msg,
	const char *s, int line, int col, int len)
{
	dsp_log_acquire();
	dsp_log_printf("%s: ", dsp_globals.prog_name);
	if (path) dsp_log_printf("%s: line %d: ", path, line+1);
	dsp_log_printf("%s: %s\n  | ", reason, msg);
	for (int i = 0; s[i] && s[i] != '\n'; ++i) {
		if (s[i] == '\t') dsp_log_puts("    ");
		else dsp_log_putc(s[i]);
	}
	dsp_log_puts("\n  | ");
	for (int i = 0; (len < 1 || i < col+len) && s[i] && s[i] != '\n'; ++i) {
		char hl[5] = { (i < col) ? ' ' : (i == col) ? '^' : '~', '\0' };
		if (s[i] == '\t') {
			memset(hl+1, (hl[0] != '^') ? hl[0] : '~', 3);
			dsp_log_puts(hl);
		}
		else dsp_log_putc(hl[0]);
	}
	if (len < 1) dsp_log_puts(">>");
	dsp_log_putc('\n');
	dsp_log_release();
}
#define ec_line_err(path, msg, s, line, col, len) \
	ec_print_line("error", path, msg, s, line, col, len)

static int ec_split_and_lex_string(struct ec_token_list *tokens, const char *s, const char *path, int *r_lines)
{
	int line = *r_lines, sep = 1, esc = 0, quo = 0, cont = 0;
	int i = 0, k = 0, l = 0, bp = 0, bsz = 1024, done = 0;
	char *buf = malloc(bsz*sizeof(char));
	while (!done) {
		int sp = 1;
		const char c = s[k];
		if (c == '\\' && !esc) {
			esc = 1;
			if (sep) goto append_char;
		}
		else if (c == '"' && !esc)
			quo = !quo;
		else if (c == '#' && !esc && !quo && sep) {
			while (s[k] && s[k] != '\n') ++k;
			i = k+1;
		}
		else if (c == '\0' || (!esc && !quo && isspace((unsigned char) c))) {
			if (c == '\0') {
				if (quo) {
					ec_line_err(path, "unterminated quoted string", &s[l], line, i-l, 0);
					break;
				}
				done = 1;
			}
			if (i != k) {
				buf[bp] = '\0';
				ec_lex_word(tokens, buf, line, i-l, k-i);
				bp = 0;
				i = k;
			}
			sep = 1;
			++i;
		}
		else {
			sp = 0;
			append_char:
			buf[bp++] = c;
			if (bp == bsz) {
				bsz += 1024;
				buf = realloc(buf, bsz*sizeof(char));
			}
		}
		if (s[k] == '\n') {
			if (esc || quo) ++cont;
			else {
				line += cont+1;
				l = k+1;
				cont = 0;
			}
		}
		if (!sp) sep = esc = 0;
		++k;
	}
	free(buf);
	*r_lines = line+cont+1;
	return (done) ? 0 : 1;
}

static int ec_token_is_keyword(struct ec_token *tok)
{
	if (tok->id != EC_TOKEN_ESC_LITERAL) {
		if (tok->id != EC_TOKEN_LITERAL)
			return 1;
		else if (get_effect_info(tok->str))
			return 1;
	}
	return 0;
}

struct ec_parser_state {
	struct effects_chain *chain;
	struct stream_info *stream;
	const char *path, *dir;
	char **line_strs;
	char *ch_sel, *ch_mask;
	struct ec_token *last_ch_sel;
	int allow_fail, last_stream_ch;
};

enum ec_nest { EC_NEST_NONE = 0, EC_NEST_BLOCK };

static struct ec_token * ec_parse(struct ec_parser_state *, struct ec_token *, enum ec_nest);

static int ec_parser_state_ch_sel_mask(struct ec_parser_state *state, const char *initial_ch_mask)
{
	state->ch_sel = NEW_SELECTOR(state->stream->channels);
	state->ch_mask = NEW_SELECTOR(state->stream->channels);
	if (initial_ch_mask) COPY_SELECTOR(state->ch_mask, initial_ch_mask, state->stream->channels);
	else SET_SELECTOR(state->ch_mask, state->stream->channels);
	COPY_SELECTOR(state->ch_sel, state->ch_mask, state->stream->channels);
	return 0;
}

static void ec_parser_state_cleanup(struct ec_parser_state *state)
{
	free(state->ch_sel);
	free(state->ch_mask);
}

static struct ec_token * ec_parse_child_block(struct ec_token *tok, struct ec_parser_state *parent_state)
{
	struct ec_parser_state state = {
		.chain = parent_state->chain,
		.stream = parent_state->stream,
		.path = parent_state->path,
		.dir = parent_state->dir,
		.last_stream_ch = parent_state->last_stream_ch,
	};
	if (ec_parser_state_ch_sel_mask(&state, parent_state->ch_sel))
		return tok;
	tok = ec_parse(&state, tok, EC_NEST_BLOCK);
	ec_parser_state_cleanup(&state);
	return tok;
}

static int ec_parse_string(char *s, const char *path, const char *dir, struct effects_chain *chain,
	struct stream_info *stream, const char *initial_ch_mask)
{
	int lines = 0;
	struct ec_token_list tokens = {0};
	if (ec_split_and_lex_string(&tokens, s, path, &lines))
		return 1;

	struct ec_parser_state state = {
		.chain = chain,
		.stream = stream,
		.path = path,
		.dir = dir,
		.last_stream_ch = stream->channels,
	};
	if (ec_parser_state_ch_sel_mask(&state, initial_ch_mask))
		return 1;
	if (lines > 0) {
		state.line_strs = calloc(lines, sizeof(const char *));
		char *line = s;
		for (int i = 0; i < lines && *line != '\0'; ++i) {
			state.line_strs[i] = line;
			line = isolate(line, '\n');
		}
	}

	struct ec_token *tok = ec_parse(&state, tokens.head, EC_NEST_NONE);
	ec_token_list_destroy(&tokens);
	free(state.line_strs);
	ec_parser_state_cleanup(&state);
	return (tok == NULL) ? 0 : 1;
}

static int ec_parse_file(const char *path, const char *dir, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, int enforce_eof_marker)
{
	int ret = 0;
	char *p = NULL, *c = NULL, *d = NULL;
	p = construct_full_path(dir, path, stream->fs, num_bits_set(ch_mask, stream->channels));
	if (!p) goto fail_nomem;
	if (!(c = get_file_contents(p))) {
		LOG_FMT(LL_ERROR, "error: failed to load effects file: %s: %s", p, strerror(errno));
		goto fail;
	}
	if (enforce_eof_marker) {
		const ssize_t l = LENGTH(EFFECTS_FILE_EOF_MARKER)-1;
		ssize_t k = strlen(c);
		while (k > l && isspace((unsigned char) c[k-1])) --k;
		if (k < l || strncmp(&c[k-l], EFFECTS_FILE_EOF_MARKER, l) != 0 || (k > l && c[k-l-1] != '\n')) {
			LOG_FMT(LL_ERROR, "error: no valid end-of-file marker: %s", p);
			goto fail;
		}
	}
	char *b = strrchr(p, '/');
	if (b && !(d = strndup(p, b-p))) goto fail_nomem;
	LOG_FMT(LL_VERBOSE, "info: begin effects file: %s", p);
	if (ec_parse_string(c, p, (d)?d:".", chain, stream, ch_mask))
		goto fail;
	LOG_FMT(LL_VERBOSE, "info: end effects file: %s", p);
	done:
	free(c);
	free(p);
	free(d);
	return ret;

	fail_nomem:
	dsp_perror(DSP_ENOMEM, NULL);
	fail:
	ret = 1;
	goto done;
}

static int ec_parse_argv(int argc, const char *const *argv, const char *dir, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask)
{
	if (argc < 1) return 0;
	int ret = 0;
	ssize_t s = 2048, p = 0;
	struct ec_token_list tokens = {0};
	struct ec_parser_state state = {0};
	char *line = malloc(s * sizeof(char));
	if (!line) goto fail_nomem;
	for (int i = 0; i < argc; ++i) {
		const int len = strlen(argv[i]);
		if (p+len >= s) {
			while (p+len >= s) s += 2048;
			char *line_tmp = realloc(line, s * sizeof(char));
			if (!line_tmp) goto fail_nomem;
			line = line_tmp;
		}
		if (ec_lex_word(&tokens, argv[i], 0, p, len))
			goto fail;
		memcpy(line+p, argv[i], len);
		p += len+1;
		line[p-1] = ' ';
	}
	line[p-1] = '\0';

	state.chain = chain;
	state.stream = stream;
	state.path = NULL;
	state.dir = dir;
	state.line_strs = &line;
	state.last_stream_ch = stream->channels;
	if (ec_parser_state_ch_sel_mask(&state, ch_mask))
		goto fail;
	if (ec_parse(&state, tokens.head, EC_NEST_NONE) != NULL)
		goto fail;

	done:
	ec_token_list_destroy(&tokens);
	ec_parser_state_cleanup(&state);
	free(line);
	return ret;

	fail_nomem:
	dsp_perror(DSP_ENOMEM, NULL);
	fail:
	ret = 1;
	goto done;
}

#define ec_parse_print_line(reason, state, msg, line, col, len) \
	ec_print_line(reason, (state)->path, msg, (state)->line_strs[line], line, col, len)
#define ec_parse_hl_token(reason, state, msg, tok) \
	ec_parse_print_line(reason, state, msg, (tok)->line, (tok)->col, (tok)->len)
#define ec_parse_err(state, msg, tok) ec_parse_hl_token("error", state, msg, tok)
#define ec_parse_note(state, msg, tok) ec_parse_hl_token("note", state, msg, tok)
static int ec_parse_effect_err(struct ec_parser_state *state, const char *msg, struct ec_token *tok, struct ec_token *hl_end)
{
	dsp_log_acquire();
	dsp_log_printf("%s: ", dsp_globals.prog_name);
	if (state->path) dsp_log_printf("%s: line %d: ", state->path, tok->line+1);
	dsp_log_printf("%s: %s: ", (state->allow_fail) ? "warning" : "error", msg);
	ec_print_escaped_str(tok->str, 0);
	dsp_log_putc('\n');
	dsp_log_release();
	const int len = (hl_end->line == tok->line) ? hl_end->col + hl_end->len - tok->col : 0;
	ec_parse_print_line("note", state, "defined here:", tok->line, tok->col, len);
	return (state->allow_fail) ? 0 : 1;
}

static struct ec_token * ec_parse(struct ec_parser_state *state, struct ec_token *tok, enum ec_nest nest)
{
	struct ec_token *prev_effect = NULL;
	while (tok) {
		if (nest == EC_NEST_BLOCK && tok->id == EC_TOKEN_BLOCK_END)
			return tok;
		if (tok->id == EC_TOKEN_ALLOW_FAIL) {
			state->allow_fail = 1;
			tok = tok->next; continue;
		}
		if (state->last_stream_ch != state->stream->channels) {  /* construct new channel mask */
			const int delta = state->stream->channels - state->last_stream_ch;
			char *tmp_mask = NEW_SELECTOR(state->stream->channels);
			if (delta > 0) {
				/* additional channels are appended */
				COPY_SELECTOR(tmp_mask, state->ch_mask, state->last_stream_ch);
				free(state->ch_mask);
				state->ch_mask = tmp_mask;
				for (int j = state->last_stream_ch; j < state->stream->channels; ++j)
					SET_BIT(state->ch_mask, j);
			}
			else {
				int nb = num_bits_set(state->ch_mask, state->last_stream_ch) + delta;
				for (int j = 0; j < state->stream->channels && nb > 0; ++j) {
					if (GET_BIT(state->ch_mask, j)) {
						SET_BIT(tmp_mask, j);
						--nb;
					}
				}
				free(state->ch_mask);
				state->ch_mask = tmp_mask;
			}
		}
		if (tok->id == EC_TOKEN_CH_SEL) {
			if (state->last_stream_ch != state->stream->channels) {
				free(state->ch_sel);
				state->ch_sel = NEW_SELECTOR(state->stream->channels);
				state->last_stream_ch = state->stream->channels;
			}
			if (parse_selector_masked(tok->str, state->ch_sel, state->ch_mask, state->stream->channels)) {
				ec_parse_note(state, "defined here:", tok);
				return tok;
			}
			state->last_ch_sel = tok;
			tok = tok->next; continue;
		}
		if (state->last_stream_ch != state->stream->channels) {  /* re-parse the channel selector */
			char *tmp_ch_sel = NEW_SELECTOR(state->stream->channels);
			if (!state->last_ch_sel)
				COPY_SELECTOR(tmp_ch_sel, state->ch_mask, state->stream->channels);
			else if (parse_selector_masked(state->last_ch_sel->str, tmp_ch_sel, state->ch_mask, state->stream->channels)) {
				ec_parse_note(state, "active channel selector defined here:", state->last_ch_sel);
				ec_parse_note(state, "number of channels modified by this effect:", prev_effect);
				free(tmp_ch_sel);
				return tok;
			}
			free(state->ch_sel);
			state->ch_sel = tmp_ch_sel;
			state->last_stream_ch = state->stream->channels;
		}
		if (tok->id == EC_TOKEN_SOURCE) {
			if (ec_parse_file(tok->str, state->dir, state->chain, state->stream, state->ch_sel, 0))
				return tok;
			tok = tok->next; continue;
		}
		if (tok->id == EC_TOKEN_BLOCK_START) {
			struct ec_token *end = ec_parse_child_block(tok->next, state);
			if (!end) {
				ec_parse_err(state, "unterminated block", tok);
				return tok;
			}
			else if (end->id != EC_TOKEN_BLOCK_END)
				return tok;
			tok = end->next;
			continue;
		}
		if (tok->id != EC_TOKEN_LITERAL) {
			ec_parse_err(state, "unexpected token", tok);
			return tok;
		}
		const struct effect_info *ei = get_effect_info(tok->str);
		/* find end of argument list */
		int argc = 1;
		struct ec_token *argv_end = tok;
		while (argv_end->next && !ec_token_is_keyword(argv_end->next)) {
			argv_end = argv_end->next;
			++argc;
		}
		if (ei == NULL) {
			if (ec_parse_effect_err(state, "no such effect", tok, argv_end))
				return tok;
		}
		else if (ei->init == NULL) {
			if (ec_parse_effect_err(state, "effect not available", tok, argv_end))
				return tok;
		}
		else {
			/* build argument vector */
			char **argv = calloc(argc, sizeof(char *));
			struct ec_token *arg = tok;
			for (int i = 0; i < argc; ++i) {
				argv[i] = strdup(arg->str);
				arg = arg->next;
			}
			if (LOGLEVEL(LL_VERBOSE)) {
				dsp_log_acquire();
				dsp_log_printf("%s: effect:", dsp_globals.prog_name);
				for (int i = 0; i < argc; ++i) {
					const int do_quo = !!strchr(argv[i], ' ');
					dsp_log_putc(' ');
					if (do_quo) dsp_log_putc('"');
					ec_print_escaped_str(argv[i], 160);
					if (do_quo) dsp_log_putc('"');
				}
				dsp_log_printf("; channels=%d [", state->stream->channels);
				print_selector(state->ch_sel, state->stream->channels);
				dsp_log_printf("] fs=%d\n", state->stream->fs);
				dsp_log_release();
			}
			struct effect *e = ei->init(ei, state->stream, state->ch_sel, state->dir, argc, (const char *const *) argv);
			for (int i = 0; i < argc; ++i) free(argv[i]);
			free(argv);
			if (e == NULL) {
				if (ec_parse_effect_err(state, "failed to initialize effect", tok, argv_end))
					return tok;
			}
			while (e != NULL) {
				struct effect *e_n = e->next;
				if (e->run == NULL) {
					LOG_FMT(LL_VERBOSE, "info: not using effect: %s", tok->str);
					destroy_effect(e);
				}
				else {
					effects_chain_append(state->chain, e);
					*state->stream = e->ostream;
				}
				e = e_n;
			}
		}
		state->allow_fail = 0;
		prev_effect = tok;
		tok = argv_end->next;
	}
	return NULL;
}

static void effects_chain_optimize(struct effects_chain *chain)
{
#if DO_EFFECTS_CHAIN_OPTIMIZE
	ssize_t chain_len = 0, chain_len_opt = 0;
	LIST_FOREACH(chain, e) ++chain_len;
	struct effect *m_dest = chain->head;
	while (m_dest) {
		if (m_dest->merge) {
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
					struct effect *tmp = m_src;
					m_src = m_src->next;
					LIST_REMOVE(chain, tmp);
					destroy_effect(tmp);
				}
				else {
					skip:
					m_src = m_src->next;
				}
			}
		}
		m_dest = m_dest->next;
	}
	LIST_FOREACH(chain, e) ++chain_len_opt;
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
	LIST_FOREACH(chain, e) {
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
				if (align_effect_insert(chain, prev, offsets, NULL))
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
				if (align_effect_insert(chain, prev, offsets, align_refs))
					goto fail;
			}
			else if (e->istream.fs != e->ostream.fs) {
				LOG_FMT(LL_VERBOSE, "info: %s: sample rate changed; doing full alignment", e->name);
				if (align_effect_insert(chain, prev, offsets, NULL))
					goto fail;
			}
			else if (!is_passthrough) {
				LOG_FMT(LL_VERBOSE, "warning: %s: channel deps unknown; doing full alignment", e->name);
				if (align_effect_insert(chain, prev, offsets, NULL))
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
	if (prev && align_effect_insert(chain, prev, offsets, NULL))
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
	LIST_FOREACH(chain, e) {
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
	LIST_FOREACH(chain, e) {
		if (e->prepare && e->prepare(e))
			return 1;
	}
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

int build_effects_chain_from_argv(int argc, const char *const *argv, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir)
{
	if (ec_parse_argv(argc, argv, dir, chain, stream, ch_mask))
		return 1;
	return build_effects_chain_finish(chain);
}

int build_effects_chain_from_string(const char *cs, const char *path, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir)
{
	char *s = strdup(cs);
	if (!s) {
		dsp_perror(DSP_ENOMEM, NULL);
		return 1;
	}
	if (ec_parse_string(s, path, dir, chain, stream, ch_mask)) {
		free(s);
		return 1;
	}
	free(s);
	return build_effects_chain_finish(chain);
}

int build_effects_chain_from_file(const char *path, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir, int enforce_eof_marker)
{
	if (ec_parse_file(path, dir, chain, stream, ch_mask, enforce_eof_marker))
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
	LIST_FOREACH(chain, e) {
		frames = effect_max_out_frames(e, frames);
		len = frames * e->ostream.channels;
		if (len  > max_len) max_len = len;
	}
	return max_len;
}

ssize_t get_effects_chain_max_out_frames(struct effects_chain *chain, ssize_t in_frames)
{
	ssize_t frames = in_frames;
	LIST_FOREACH(chain, e) frames = effect_max_out_frames(e, frames);
	return frames;
}

int effects_chain_needs_dither(struct effects_chain *chain)
{
	LIST_FOREACH(chain, e) {
		if (!(e->flags & EFFECT_FLAG_NO_DITHER) && !effect_is_dither(e))
			return 1;
	}
	return 0;
}

int effects_chain_set_dither_params(struct effects_chain *chain, int prec, int enabled)
{
	int r = 1;
	LIST_FOREACH(chain, e) {
		if (effect_is_dither(e)) {
			dither_effect_set_params(e, prec, enabled);
			r = 0;
		}
		else if (!(e->flags & EFFECT_FLAG_NO_DITHER)) r = 1;
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
	LIST_FOREACH(chain, e)
		if (e->reset != NULL) e->reset(e);
	chain->frames = 0;
}

void signal_effects_chain(struct effects_chain *chain)
{
	LIST_FOREACH(chain, e)
		if (e->signal != NULL) e->signal(e);
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
	while (chain->head) {
		struct effect *e = chain->head;
		LIST_REMOVE(chain, e);
		destroy_effect(e);
	}
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
	state->chain[0] = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
	state->chain[1] = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
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
