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
#include <pthread.h>
#include <semaphore.h>
#include "effects_chain.h"
#include "util.h"
#include "list_util.h"
#include "align.h"
#include "dither.h"

struct effects_subchain {
	struct effects_subchain *prev, *next;
	struct effect *head, *tail;
	sample_t *buf1, *buf2;
	pthread_t thread;
	pthread_mutex_t lock;
	struct {
		sem_t in, out;
		sem_t *prev, *next;
	} sync;
	struct {
		sample_t *buf;
		ssize_t frames;
	} out;
	int has_thread, flush;
};

enum ec_token_id {
	EC_TOKEN_LITERAL = 0,
	EC_TOKEN_ESC_LITERAL,
	EC_TOKEN_CH_SEL,
	EC_TOKEN_BLOCK_START,
	EC_TOKEN_BLOCK_END,
	EC_TOKEN_SOURCE,
	EC_TOKEN_ALLOW_FAIL,
	EC_TOKEN_NEW_THREAD,
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
	else if (strcmp(s, "new_thread") == 0)
		return EC_TOKEN_NEW_THREAD;
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
	if (check_alloc(__func__, tok)) return 1;
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
	if (check_alloc(__func__, buf)) {
		*r_lines = 0;
		return 1;
	}
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
				char *buf_tmp = realloc(buf, bsz*sizeof(char));
				if (check_alloc(__func__, buf_tmp)) break;
				buf = buf_tmp;
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

void effects_subchain_insert(struct effects_subchain *sc, struct effect *e, struct effect *prev)
{
	LIST_INSERT(sc, e, prev);
}

static int ec_add_subchain(struct effects_chain *chain)
{
	struct effects_subchain *sc = calloc(1, sizeof(struct effects_subchain));
	if (!sc) return DSP_ENOMEM;
	pthread_mutex_init(&sc->lock, NULL);
	sem_init(&sc->sync.in, 0, 0);
	sem_init(&sc->sync.out, 0, 1);
	LIST_APPEND(chain, sc);
	return 0;
}

static int ec_append(struct effects_chain *chain, struct effect *e)
{
	if (!chain->tail || chain->new_sc) {
		LOG_FMT(LL_VERBOSE, "info: new subchain: %s", e->name);
		const int err = ec_add_subchain(chain);
		if (err) return err;
		chain->new_sc = 0;
	}
	LIST_APPEND(chain->tail, e);
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

#define EC_PARSE_MAX_RDEPTH 512  /* surely enough for any practical use... */
enum ec_nest { EC_NEST_NONE = 0, EC_NEST_BLOCK };

static struct ec_token * ec_parse(struct ec_parser_state *, struct ec_token *, enum ec_nest, int);

static int ec_parser_state_ch_sel_mask(struct ec_parser_state *state, const char *initial_ch_mask)
{
	state->ch_sel = state->ch_mask = NULL;
	state->ch_sel = NEW_SELECTOR(state->stream->channels);
	if (check_alloc(__func__, state->ch_sel)) return 1;
	state->ch_mask = NEW_SELECTOR(state->stream->channels);
	if (check_alloc(__func__, state->ch_mask)) {
		free(state->ch_sel);
		state->ch_sel = NULL;
		return 1;
	}
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

static struct ec_token * ec_parse_child_block(struct ec_token *tok, struct ec_parser_state *parent_state, int rdepth)
{
	struct ec_parser_state state = {
		.chain = parent_state->chain,
		.stream = parent_state->stream,
		.path = parent_state->path,
		.dir = parent_state->dir,
		.line_strs = parent_state->line_strs,
		.last_stream_ch = parent_state->last_stream_ch,
	};
	if (ec_parser_state_ch_sel_mask(&state, parent_state->ch_sel) == 0)
		tok = ec_parse(&state, tok, EC_NEST_BLOCK, rdepth+1);
	ec_parser_state_cleanup(&state);
	return tok;
}

static int ec_parse_string(char *s, const char *path, const char *dir, struct effects_chain *chain,
	struct stream_info *stream, const char *initial_ch_mask, int rdepth)
{
	int lines = 0, ret = 0;
	struct ec_token *tok = NULL;
	struct ec_token_list tokens = {0};
	struct ec_parser_state state = {
		.chain = chain,
		.stream = stream,
		.path = path,
		.dir = dir,
		.last_stream_ch = stream->channels,
	};

	if (ec_split_and_lex_string(&tokens, s, path, &lines)) goto fail;
	if (ec_parser_state_ch_sel_mask(&state, initial_ch_mask)) goto fail;
	if (lines > 0) {
		state.line_strs = calloc(lines, sizeof(const char *));
		if (check_alloc(__func__, state.line_strs)) goto fail;
		char *line = s;
		for (int i = 0; i < lines && *line != '\0'; ++i) {
			state.line_strs[i] = line;
			line = isolate(line, '\n');
		}
	}

	tok = ec_parse(&state, tokens.head, EC_NEST_NONE, rdepth+1);

	done:
	ec_token_list_destroy(&tokens);
	free(state.line_strs);
	ec_parser_state_cleanup(&state);
	return (ret || tok);

	fail:
	ret = 1;
	goto done;
}

static int ec_parse_file(const char *path, const char *dir, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, int enforce_eof_marker, int rdepth)
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
	if (ec_parse_string(c, p, (d)?d:".", chain, stream, ch_mask, rdepth+1))
		goto fail;
	LOG_FMT(LL_VERBOSE, "info: end effects file: %s", p);
	done:
	free(c);
	free(p);
	free(d);
	return ret;

	fail_nomem:
	dsp_perror(DSP_ENOMEM, NULL, NULL);
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
	if (check_alloc(__func__, line)) goto fail;
	for (int i = 0; i < argc; ++i) {
		const int len = strlen(argv[i]);
		if (p+len >= s) {
			while (p+len >= s) s += 2048;
			char *line_tmp = realloc(line, s * sizeof(char));
			if (check_alloc(__func__, line_tmp)) goto fail;
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
	if (ec_parse(&state, tokens.head, EC_NEST_NONE, 1) != NULL)
		goto fail;

	done:
	ec_token_list_destroy(&tokens);
	ec_parser_state_cleanup(&state);
	free(line);
	return ret;

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
	if (!state->allow_fail || LOGLEVEL(LL_VERBOSE)) {
		const int len = (hl_end->line == tok->line) ? hl_end->col + hl_end->len - tok->col : 0;
		ec_parse_print_line("note", state, "defined here:", tok->line, tok->col, len);
	}
	return (state->allow_fail) ? 0 : 1;
}

static struct ec_token * ec_parse(struct ec_parser_state *state, struct ec_token *tok, enum ec_nest nest, int rdepth)
{
	struct ec_token *prev_effect = NULL;
	if (rdepth > EC_PARSE_MAX_RDEPTH) {
		ec_parse_err(state, "maximum recursion depth exceeded", tok);
		return tok;
	}
	while (tok) {
		if (nest == EC_NEST_BLOCK && tok->id == EC_TOKEN_BLOCK_END)
			return tok;
		if (tok->id == EC_TOKEN_ALLOW_FAIL) {
			state->allow_fail = 1;
			tok = tok->next; continue;
		}
		if (tok->id == EC_TOKEN_NEW_THREAD) {
			state->chain->new_sc = 1;
			tok = tok->next; continue;
		}
		if (state->last_stream_ch != state->stream->channels) {  /* construct new channel mask */
			const int delta = state->stream->channels - state->last_stream_ch;
			char *tmp_mask = NEW_SELECTOR(state->stream->channels);
			if (check_alloc(__func__, tmp_mask)) return tok;
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
				if (check_alloc(__func__, state->ch_sel)) return tok;
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
			if (check_alloc(__func__, tmp_ch_sel)) return tok;
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
			if (ec_parse_file(tok->str, state->dir, state->chain, state->stream, state->ch_sel, 0, rdepth))
				return tok;
			tok = tok->next; continue;
		}
		if (tok->id == EC_TOKEN_BLOCK_START) {
			struct ec_token *end = ec_parse_child_block(tok->next, state, rdepth);
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
			if (check_alloc(__func__, argv)) return tok;
			struct ec_token *arg = tok;
			for (int i = 0; i < argc; ++i) {
				argv[i] = strdup(arg->str);
				if (check_alloc(__func__, argv[i])) {
					while (--i >= 0) free(argv[i]);
					free(argv);
					return tok;
				}
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
			for (int i = 0; e != NULL; ++i) {
				struct effect *e_n = e->next;
				if (e->run == NULL) {
					if (e_n || i > 0) LOG_FMT(LL_VERBOSE, "info: not using sub-effect #%d of %s: %s", i+1, tok->str, e->name);
					else LOG_FMT(LL_VERBOSE, "info: not using effect: %s", e->name);
					destroy_effect(e);
				}
				else {
					const int err = ec_append(state->chain, e);
					if (err) {
						dsp_perror(err, __func__, NULL);
						return tok;
					}
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
	ssize_t chain_len = 0;
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) ++chain_len;
	ssize_t chain_len_opt = chain_len;
	LIST_FOREACH(chain, sc) {
		struct effect *m_dest = sc->head;
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
						LIST_REMOVE(sc, tmp);
						destroy_effect(tmp);
						--chain_len_opt;
					}
					else {
						skip:
						m_src = m_src->next;
					}
				}
			}
			m_dest = m_dest->next;
		}
	}
	if (chain_len_opt < chain_len)
		LOG_FMT(LL_VERBOSE, "optimize: info: reduced number of effects from %zd to %zd", chain_len, chain_len_opt);
}

struct effects_chain_postproc_state {
	char **ch_deps;
	ssize_t *samples[4];
	int max_in_ch, max_out_ch, max_ch;
};

static void effects_chain_postproc_state_cleanup(struct effects_chain_postproc_state *state)
{
	if (state->ch_deps) {
		for (int i = 0; i < state->max_out_ch; ++i)
			free(state->ch_deps[i]);
		free(state->ch_deps);
	}
	for (int i = 0; i < LENGTH(state->samples); ++i)
		free(state->samples[i]);
	memset(state, 0, sizeof(struct effects_chain_postproc_state));
}

static int effects_chain_postproc_state_init(struct effects_chain_postproc_state *state, struct effects_chain *chain)
{
	memset(state, 0, sizeof(struct effects_chain_postproc_state));
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		state->max_in_ch = MAXIMUM(state->max_in_ch, e->istream.channels);
		state->max_out_ch = MAXIMUM(state->max_out_ch, e->ostream.channels);
	}
	state->max_ch = MAXIMUM(state->max_in_ch, state->max_out_ch);

	state->ch_deps = calloc(state->max_out_ch, sizeof(char *));
	if (check_alloc(__func__, state->ch_deps)) goto fail;
	for (int i = 0; i < state->max_out_ch; ++i) {
		state->ch_deps[i] = NEW_SELECTOR(state->max_in_ch);
		if (check_alloc(__func__, state->ch_deps[i])) goto fail;
	}
	for (int i = 0; i < LENGTH(state->samples); ++i) {
		state->samples[i] = calloc(state->max_ch, sizeof(ssize_t));
		if (check_alloc(__func__, state->samples[i])) goto fail;
	}
	return 0;

	fail:
	effects_chain_postproc_state_cleanup(state);
	return 1;
}

static int sel_is_identity(char *s, int n, int i)
{
	if (!GET_BIT(s, i)) return 0;
	for (int k = 0; k < n; ++k)
		if (k != i && GET_BIT(s, k)) return 0;
	return 1;
}

/* returns 1 if square identity, 0 otherwise */
static int query_channel_deps(struct effects_chain_postproc_state *state, struct effect *e, int is_align)
{
	for (int i = 0; i < state->max_out_ch; ++i)
		CLEAR_SELECTOR(state->ch_deps[i], state->max_in_ch);
	/* set identity as initial state */
	const int min_ch = MINIMUM(e->istream.channels, e->ostream.channels);
	for (int i = 0; i < min_ch; ++i) SET_BIT(state->ch_deps[i], i);
	const int is_square = (e->istream.channels == e->ostream.channels);
	if (e->channel_deps) {
		e->channel_deps(e, state->ch_deps);
		if (is_square) {
			for (int i = 0; i < e->ostream.channels; ++i)
				if (!sel_is_identity(state->ch_deps[i], e->istream.channels, i)) return 0;
		}
	}
	else if (!(e->flags & EFFECT_FLAG_CH_DEPS_IDENTITY)
			|| (is_align && e->flags & EFFECT_FLAG_ALIGN_BARRIER)) {
		if (!e->channel_selector) LOG_FMT(LL_VERBOSE, "warning: %s: channel deps unknown", e->name);
		for (int i = 0; i < e->ostream.channels; ++i) {
			if (e->channel_selector) {
				if (i >= e->istream.channels || GET_BIT(e->channel_selector, i))
					COPY_SELECTOR(state->ch_deps[i], e->channel_selector, e->istream.channels);
			}
			else SET_SELECTOR(state->ch_deps[i], e->istream.channels);
		}
		return 0;
	}
	return is_square;
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

static int first_bit_set(const char *b, int n)
{
	for (int i = 0; i < n; ++i)
		if (GET_BIT(b, i)) return i;
	return -1;
}

static int effects_chain_align_channels(struct effects_chain_postproc_state *state, struct effects_chain *chain)
{
	int ret = 0;
	char *in_deps = NEW_SELECTOR(state->max_ch);
	char *in_deps_all = NEW_SELECTOR(state->max_ch);
	if (!in_deps || !in_deps_all) {
		dsp_perror(DSP_ENOMEM, __func__, NULL);
		goto fail;
	}

	ssize_t nd_part = 0;  /* negative part of delays */
	ssize_t *offsets = state->samples[0], *delays = state->samples[1];
	memset(offsets, 0, state->max_ch * sizeof(ssize_t));
	memset(delays, 0, state->max_ch * sizeof(ssize_t));

	struct effect *prev = NULL;
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		const int did_remap = (query_channel_deps(state, e, 1) == 0);
		if (prev && (e->istream.fs != e->ostream.fs || did_remap)) {
			/* align channels */
			ssize_t *align_refs = NULL;
			if (e->istream.fs != e->ostream.fs)
				LOG_FMT(LL_VERBOSE, "info: %s: sample rate changed; doing full alignment", e->name);
			else {
				align_refs = state->samples[2];
				memcpy(align_refs, offsets, e->istream.channels * sizeof(ssize_t));
				CLEAR_SELECTOR(in_deps_all, e->istream.channels);
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
			}
			if (align_effect_insert(sc, prev, e, offsets, align_refs)) goto fail;
		}
		/* find initial output offsets and delays */
		if (did_remap) {
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
				const int ref_idx = first_bit_set(state->ch_deps[i], e->istream.channels);
				delays[i] = (ref_idx >= 0) ? tmp_delays[ref_idx] : 0;
				if (ref_idx >= 0) {
					for (int k = ref_idx+1; k < e->istream.channels; ++k) {
						if (!GET_BIT(state->ch_deps[i], k)) continue;
						if (tmp_offsets[k] != tmp_offsets[ref_idx]) {
							LOG_FMT(LL_ERROR, "%s(): BUG: channel %d offset incorrect: %zd!=%zd",
								__func__, k, tmp_offsets[k], tmp_offsets[ref_idx]);
							goto fail;
						}
						else delays[i] = MINIMUM(delays[i], tmp_delays[k]);
					}
				}
				offsets[i] = (ref_idx >= 0) ? tmp_offsets[ref_idx] : max_offset;
			}
		}
		for (int i = e->ostream.channels; i < e->istream.channels; ++i)
			delays[i] = offsets[i] = 0;
		/* recalculate offsets */
		for (int i = 0; i < e->ostream.channels; ++i)
			offsets[i] += delays[i]-nd_part;  /* cumulative latency */
		if (e->channel_offsets)  /* query effect latency and requested delay */
			e->channel_offsets(e, offsets, delays);
		else if (e->ostream.fs != e->istream.fs) {
			/* FIXME: should store fractional samples as well, but the error is generally small */
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			const int ratio_n = e->ostream.fs/gcd, ratio_d = e->istream.fs/gcd;
			for (int i = 0; i < e->ostream.channels; ++i)
				delays[i] = ratio_mult_ceil(delays[i], ratio_n, ratio_d);
		}
		nd_part = 0;
		for (int i = 0; i < e->ostream.channels; ++i)
			nd_part = MINIMUM(nd_part, delays[i]);
		/* LOG_FMT(LL_VERBOSE, "%s(): nd_part=%zd", __func__, nd_part); */
		for (int i = 0; i < e->ostream.channels; ++i) {
			/* LOG_FMT(LL_VERBOSE, "%s(): output channel %d: offset=%zd latency=%zd delay=%zd",
				__func__, i, offsets[i]-(delays[i]-nd_part), offsets[i], delays[i]); */
			offsets[i] -= delays[i]-nd_part;
		}
		prev = e;
	}
	if (prev && align_effect_insert(chain->tail, prev, NULL, offsets, NULL))
		goto fail;
	chain->zero_ref = -nd_part;

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
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		if (query_channel_deps(state, e, 0) == 0) {
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
		if (e->drain_samples)
			e->drain_samples(e, samples);
		else if (e->ostream.fs != e->istream.fs) {
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			const int ratio_n = e->ostream.fs/gcd, ratio_d = e->istream.fs/gcd;
			for (int i = 0; i < e->ostream.channels; ++i)
				samples[i] = ratio_mult_ceil(samples[i], ratio_n, ratio_d);
		}
		for (int i = e->ostream.channels; i < e->istream.channels; ++i)
			samples[i] = 0;
	}
	chain->drain_frames = 0;
	for (int i = 0; i < chain->ostream.channels; ++i)
		chain->drain_frames = MAXIMUM(chain->drain_frames, samples[i]);
	if (chain->istream.fs != chain->ostream.fs) {
		const int gcd = find_gcd(chain->istream.fs, chain->ostream.fs);
		chain->drain_frames = (long long int) chain->drain_frames *
			(chain->istream.fs / gcd) / (chain->ostream.fs / gcd);
	}
	LOG_FMT(LL_VERBOSE, "info: input drain frames: %zd", chain->drain_frames);
}

static int effects_chain_prepare(struct effects_chain *chain)
{
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		if (e->prepare && e->prepare(e))
			return 1;
	}
	return 0;
}

static int build_effects_chain_start(struct effects_chain *chain, struct stream_info *istream)
{
	memcpy(&chain->istream, istream, sizeof(struct stream_info));
	memcpy(&chain->ostream, istream, sizeof(struct stream_info));
	chain->ratio.d = chain->ratio.n = 1;
	return 0;
}

static int build_effects_chain_finish(struct effects_chain *chain)
{
	if (!chain->head) {
		const int err = ec_add_subchain(chain);
		if (err) {
			dsp_perror(err, __func__, NULL);
			return 1;
		}
	}
	else if (chain->head->head) {
		struct effects_chain_postproc_state state;
		memcpy(&chain->ostream, &chain->tail->tail->ostream, sizeof(struct stream_info));
		const int gcd = find_gcd(chain->ostream.fs, chain->istream.fs);
		chain->ratio.n = chain->ostream.fs / gcd;
		chain->ratio.d = chain->istream.fs / gcd;
		effects_chain_optimize(chain);
		if (effects_chain_prepare(chain)) return 1;
		if (effects_chain_postproc_state_init(&state, chain)) return 1;
		if (effects_chain_align_channels(&state, chain)) {
			effects_chain_postproc_state_cleanup(&state);
			return 1;
		}
		effects_chain_set_drain_frames(&state, chain);
		effects_chain_postproc_state_cleanup(&state);
	}
	LIST_FOREACH(chain, sc) {
		sc->sync.prev = (sc->prev) ? &sc->prev->sync.out : &chain->tail->sync.out;
		sc->sync.next = (sc->next) ? &sc->next->sync.in : &chain->head->sync.in;
	}
	return 0;
}

int build_effects_chain_from_argv(int argc, const char *const *argv, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir)
{
	if (build_effects_chain_start(chain, stream)) return 1;
	if (ec_parse_argv(argc, argv, dir, chain, stream, ch_mask)) return 1;
	return build_effects_chain_finish(chain);
}

int build_effects_chain_from_string(const char *cs, const char *path, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir)
{
	char *s = strdup(cs);
	if (check_alloc(__func__, s)) return 1;
	if (build_effects_chain_start(chain, stream)) return 1;
	if (ec_parse_string(s, path, dir, chain, stream, ch_mask, 0)) {
		free(s);
		return 1;
	}
	free(s);
	return build_effects_chain_finish(chain);
}

int build_effects_chain_from_file(const char *path, struct effects_chain *chain,
	struct stream_info *stream, const char *ch_mask, const char *dir, int enforce_eof_marker)
{
	if (build_effects_chain_start(chain, stream)) return 1;
	if (ec_parse_file(path, dir, chain, stream, ch_mask, enforce_eof_marker, 0)) return 1;
	return build_effects_chain_finish(chain);
}

static ssize_t effect_max_out_frames(struct effect *e, ssize_t in_frames)
{
	if (e->buffer_frames) return e->buffer_frames(e, in_frames);
	if (e->ostream.fs != e->istream.fs) {
		const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
		return ratio_mult_ceil(in_frames, e->ostream.fs / gcd, e->istream.fs / gcd);
	}
	return in_frames;
}

static ssize_t effects_chain_buffer_len(struct effects_chain *chain, ssize_t in_frames)
{
	ssize_t frames = in_frames, len, max_len = in_frames * chain->istream.channels;
	LIST_FOREACH(chain, sc) {
		pthread_mutex_lock(&sc->lock);
		LIST_FOREACH(sc, e) {
			frames = effect_max_out_frames(e, frames);
			len = frames * e->ostream.channels;
			if (len  > max_len) max_len = len;
		}
		pthread_mutex_unlock(&sc->lock);
	}
	return max_len;
}

static sample_t * ec_cycle_blocks(struct effects_chain *chain, ssize_t *frames)
{
	struct effects_subchain *sc = chain->head;
	if (sc->next) {
		/* write block */
		while (sem_wait(&sc->sync.out) != 0);
		sc->out.buf = sc->buf1;
		sc->out.frames = *frames;
		sem_post(sc->sync.next);

		/* read block from tail */
		while (sem_wait(&sc->sync.in) != 0);
		sc->buf1 = chain->tail->out.buf;
		*frames = chain->tail->out.frames;
		sem_post(sc->sync.prev);
	}
	return sc->buf1;
}

static void ec_flush_begin(struct effects_chain *chain)
{
	LIST_FOREACH(chain, sc) {
		pthread_mutex_lock(&sc->lock);
		sc->flush = 1;
		pthread_mutex_unlock(&sc->lock);
	}
}

static void ec_flush_end(struct effects_chain *chain)
{
	LIST_FOREACH(chain, sc) sc->flush = 0;
}

static void ec_free_buffers(struct effects_chain *chain)
{
	struct effects_subchain *sc = chain->head;
	if (chain->buf_len > 0 && sc) {
		ec_flush_begin(chain);
		/* flush chain; free all buf1 */
		ssize_t frames;
		do {
			free(sc->buf1);
			sc->buf1 = NULL;
			frames = 0;
		} while (ec_cycle_blocks(chain, &frames));
		/* free all buf2 */
		do { free(sc->buf2); } while ((sc = sc->next));
		ec_flush_end(chain);
	}
}

static void run_effect_list(struct effect *e, ssize_t *frames, sample_t **buf1, sample_t **buf2)
{
	while (e && *frames > 0) {
		sample_t *tmp = e->run(e, frames, *buf1, *buf2);
		if (tmp == *buf2) {
			*buf2 = *buf1;
			*buf1 = tmp;
		}
		e = e->next;
	}
}

static void drain_effect_list(struct effect *e, ssize_t *frames, sample_t **buf1, sample_t **buf2)
{
	ssize_t dframes = -1;
	while (e && dframes == -1) {
		if (e->drain2) {
			dframes = *frames;
			sample_t *tmp = e->drain2(e, &dframes, *buf1, *buf2);
			if (tmp == *buf2) {
				*buf2 = *buf1;
				*buf1 = tmp;
			}
		}
		if (e->ostream.fs != e->istream.fs) {
			const int gcd = find_gcd(e->ostream.fs, e->istream.fs);
			*frames = ratio_mult_ceil(*frames, e->ostream.fs / gcd, e->istream.fs / gcd);
		}
		e = e->next;
	}
	if (dframes > 0) {
		*frames = dframes;
		run_effect_list(e, frames, buf1, buf2);
	}
	else *frames = -(*frames);
}

static void * subchain_worker(void *arg)
{
	struct effects_subchain *sc = (struct effects_subchain *) arg;
	ssize_t frames = 0;
	for (;;) {
		/* write block */
		while (sem_wait(&sc->sync.out) != 0);
		sc->out.buf = sc->buf1;
		sc->out.frames = frames;
		sem_post(sc->sync.next);

		/* read block */
		while (sem_wait(&sc->sync.in) != 0);
		sc->buf1 = sc->prev->out.buf;
		frames = sc->prev->out.frames;
		sem_post(sc->sync.prev);

		pthread_mutex_lock(&sc->lock);
		if (sc->flush) frames = 0;
		else if (frames < 0) {
			frames = -frames;
			drain_effect_list(sc->head, &frames, &sc->buf1, &sc->buf2);
		}
		else run_effect_list(sc->head, &frames, &sc->buf1, &sc->buf2);
		pthread_mutex_unlock(&sc->lock);
	}
	return NULL;
}

int effects_chain_realloc_buffers(struct effects_chain *chain, ssize_t in_frames)
{
	if (in_frames < 1) return 1;
	const ssize_t new_buf_len = effects_chain_buffer_len(chain, in_frames);
	if (new_buf_len > chain->buf_len) {
		if (chain->buf_len == 0) {
			/* spawn worker threads */
			struct effects_subchain *sc = chain->head;
			while ((sc = sc->next)) {
				if ((errno = pthread_create(&sc->thread, NULL, subchain_worker, sc)) != 0) {
					LOG_FMT(LL_ERROR, "%s(): error: pthread_create() failed: %s", __func__, strerror(errno));
					return 1;
				}
				sc->has_thread = 1;
			}
		}
		else ec_free_buffers(chain);
		/* alloc and distribute all buf1 */
		ssize_t frames;
		do {
			chain->head->buf1 = calloc(new_buf_len, sizeof(sample_t));
			if (check_alloc(__func__, chain->head->buf1)) return 1;
			frames = 0;
		} while (ec_cycle_blocks(chain, &frames) == NULL);
		/* alloc all buf2 */
		LIST_FOREACH(chain, sc) {
			sc->buf2 = calloc(new_buf_len, sizeof(sample_t));
			if (check_alloc(__func__, sc->buf1)) return 1;
		}
		chain->buf_len = new_buf_len;
	}
	return 0;
}

sample_t * effects_chain_get_input_buffer(struct effects_chain *chain)
{
	return (chain->head) ? chain->head->buf1 : NULL;
}

ssize_t get_effects_chain_max_out_frames(struct effects_chain *chain, ssize_t in_frames)
{
	ssize_t frames = in_frames;
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e)
		frames = effect_max_out_frames(e, frames);
	return frames;
}

int effects_chain_needs_dither(struct effects_chain *chain)
{
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		if (!(e->flags & EFFECT_FLAG_NO_DITHER) && !effect_is_dither(e))
			return 1;
	}
	return 0;
}

int effects_chain_set_dither_params(struct effects_chain *chain, int prec, int enabled)
{
	int r = 1;
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		if (effect_is_dither(e)) {
			pthread_mutex_lock(&sc->lock);
			dither_effect_set_params(e, prec, enabled);
			pthread_mutex_unlock(&sc->lock);
			r = 0;
		}
		else if (!(e->flags & EFFECT_FLAG_NO_DITHER)) r = 1;
	}
	return r && enabled;  /* note: non-zero return value means dither should be added */
}

sample_t * run_effects_chain(struct effects_chain *chain, ssize_t *frames)
{
	struct effects_subchain *sc = chain->head;
	if (!sc->head) return sc->buf1;

	const ssize_t iframes = *frames;
	run_effect_list(sc->head, frames, &sc->buf1, &sc->buf2);
	sample_t *obuf = ec_cycle_blocks(chain, frames);
	const ssize_t oframes = *frames;

	chain->iframes += iframes;
	chain->oframes += oframes;
	if (chain->istream.fs == chain->ostream.fs)
		chain->delay += iframes - oframes;
	else {
		const long long int n = (long long int) iframes * chain->ratio.n;
		ssize_t oframes_nd = n / chain->ratio.d;
		chain->frac += n % chain->ratio.d;
		if (chain->frac >= chain->ratio.d) {
			chain->frac -= chain->ratio.d;
			++oframes_nd;
		}
		chain->delay += oframes_nd - oframes;
	}
	return obuf;
}

double get_effects_chain_delay(struct effects_chain *chain, int seek)
{
	ssize_t delay_f = chain->delay;
	if (!seek) delay_f += chain->zero_ref;
	const double frac_f = (double) chain->frac / chain->ratio.d;
	return ((double) delay_f + frac_f) / chain->ostream.fs;
}

void reset_effects_chain(struct effects_chain *chain)
{
	ec_flush_begin(chain);
	LIST_FOREACH(chain, sc) {
		ssize_t frames = 0;
		ec_cycle_blocks(chain, &frames);
	}
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e)
		if (e->reset) e->reset(e);
	chain->oframes = chain->iframes = 0;
	chain->frac = chain->delay = 0;
	ec_flush_end(chain);
}

void signal_effects_chain(struct effects_chain *chain)
{
	LIST_FOREACH(chain, sc) {
		pthread_mutex_lock(&sc->lock);
		LIST_FOREACH(sc, e) if (e->signal) e->signal(e);
		pthread_mutex_unlock(&sc->lock);
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

void plot_effects_chain(struct effects_chain *chain, int plot_phase)
{
	struct stream_info stream;
	memcpy(&stream, &chain->istream, sizeof(struct stream_info));
	LIST_FOREACH(chain, sc) LIST_FOREACH(sc, e) {
		if (e->plot == NULL) {
			LOG_FMT(LL_ERROR, "plot: error: effect '%s' does not support plotting", e->name);
			return;
		}
		if (e->istream.channels != e->ostream.channels && !(e->flags & EFFECT_FLAG_PLOT_MIX)) {
			LOG_FMT(LL_ERROR, "plot: BUG: effect '%s' changed the number of channels but does not have EFFECT_FLAG_PLOT_MIX set!", e->name);
			return;
		}
		stream.fs = e->ostream.fs;
	}
	LIST_FOREACH(chain, sc) if (sc->next) {  /* link subchains */
		sc->tail->next = sc->next->head;
		sc->next->head->prev = sc->tail;
	}
	printf("%sset xrange [10:%d/2]\n%s\n",
		gnuplot_header, stream.fs, (plot_phase)?gnuplot_header_phase:"");
	struct effect *e = chain->head->head, *start_e = e;
	int start_idx = 0;
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
			stream.channels = e->ostream.channels;
		}
		e->plot(e, i);
		e = e->next;
	}
	for (int k = 0; k < stream.channels; ++k) {
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
	LIST_FOREACH(chain, sc) if (sc->head) {  /* unlink subchains */
		sc->head->prev = NULL;
		sc->tail->next = NULL;
	}
	printf("\nplot ");
	for (int k = 0; k < stream.channels; ++k) {
		printf("%sHt%d_mag_dB(x) lt %d lw 2 title 'Channel %d'", (k==0)?"":", ", k, k+1, k);
		if (plot_phase)
			printf(", Ht%d_phase_deg(x) axes x1y2 lt %d lw 1 dt '-' notitle", k, k+1);
	}
	puts("\npause mouse close");
}

sample_t * drain_effects_chain(struct effects_chain *chain, ssize_t *frames)
{
	struct effects_subchain *sc = chain->head;
	if (!sc->head || chain->iframes < 1) {
		*frames = -1;
		return sc->buf1;
	}
	if (chain->drain_frames > 0) {
		*frames = MINIMUM(*frames, chain->drain_frames);
		chain->drain_frames -= *frames;
		memset(sc->buf1, 0, *frames * chain->istream.channels * sizeof(sample_t));
		return run_effects_chain(chain, frames);
	}
	drain_effect_list(sc->head, frames, &sc->buf1, &sc->buf2);
	return ec_cycle_blocks(chain, frames);
}

void destroy_effects_chain(struct effects_chain *chain)
{
	ec_free_buffers(chain);
	LIST_FOREACH(chain, sc) {
		if (sc->has_thread) {
			pthread_cancel(sc->thread);
			pthread_join(sc->thread, NULL);
		}
	}
	while (chain->head) {
		struct effects_subchain *sc = chain->head;
		LIST_REMOVE(chain, sc);
		while (sc->head) {
			struct effect *e = sc->head;
			LIST_REMOVE(sc, e);
			destroy_effect(e);
		}
		sem_destroy(&sc->sync.out);
		sem_destroy(&sc->sync.in);
		pthread_mutex_destroy(&sc->lock);
		free(sc);
	}
	*chain = (struct effects_chain) EFFECTS_CHAIN_INITIALIZER;
}

void effects_chain_xfade_reset(struct effects_chain_xfade_state *state)
{
	state->chain[1].c = state->chain[0].c = NULL;
	state->chain[1].buf = state->chain[0].buf = NULL;
	state->pos = 0;
}

void effects_chain_xfade_begin(struct effects_chain_xfade_state *state, struct effects_chain *old, struct effects_chain *new, double xfade_ms)
{
	state->chain[0].c = old;
	state->chain[0].buf = effects_chain_get_input_buffer(old);
	state->chain[1].c = new;
	state->chain[1].buf = effects_chain_get_input_buffer(new);
	state->pos = state->frames = lround(xfade_ms/1000.0 * old->ostream.fs);
}

static inline double xfade_mult(ssize_t pos, ssize_t n)
{
	return (double) (n-pos) / n;
}

sample_t * effects_chain_xfade_run(struct effects_chain_xfade_state *state, ssize_t *frames)
{
	ssize_t tmp_f = *frames, adj_xf_f = state->frames;
	const int in_ch = state->chain[0].c->istream.channels, out_ch = state->chain[0].c->ostream.channels;
	const int has_output = (state->chain[1].c->oframes > 0);

	memcpy(state->chain[1].buf, state->chain[0].buf, *frames*in_ch*sizeof(sample_t));
	state->chain[0].buf = run_effects_chain(state->chain[0].c, frames);
	state->chain[1].buf = run_effects_chain(state->chain[1].c, &tmp_f);
	if (state->chain[1].c->oframes <= 0) return state->chain[0].buf;

	const ssize_t min_f = MINIMUM(*frames, tmp_f);
	ssize_t offset_s = 0;
	if (!has_output) offset_s = (*frames-min_f)*out_ch;
	else if (*frames != tmp_f) {
		if (min_f < state->pos) {
			adj_xf_f = lround((double)min_f/state->pos*state->frames);
			/* LOG_FMT(LL_VERBOSE, "%s(): truncated crossfade: %zd -> %zd", __func__, state->frames, adj_xf_f); */
			state->pos = min_f;
		}
		*frames = tmp_f;
	}

	const ssize_t end_s = min_f*out_ch;
	for (ssize_t i = 0; i < end_s; i += out_ch) {
		const double m = (state->pos > 0) ? xfade_mult(state->pos--, adj_xf_f) : 1.0;
		for (int k = 0; k < out_ch; ++k)
			state->chain[0].buf[i+offset_s+k] = state->chain[1].buf[i+k]*m + state->chain[0].buf[i+offset_s+k]*(1.0-m);
	}
	return state->chain[0].buf;
}
