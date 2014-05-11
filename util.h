#ifndef _UTIL_H
#define _UTIL_H

#include <string.h>
#include "dsp.h"
#include "effect.h"

#define LENGTH(x) (sizeof(x) / sizeof(x[0]))
#define CHECK_RANGE(cond, name) \
	if (!(cond)) { \
		LOG(LL_ERROR, "dsp: %s: error: %s out of range\n", argv[0], name); \
		return NULL; \
	}
#define GET_BIT(x, o) (((char *) x)[(int) (o) / 8] & (1 << ((int) (o) % 8)))
#define SET_BIT(x, o) ((char *) x)[(int) (o) / 8] |= (1 << ((int) (o) % 8))
#define SET_BIT_ARRAY(x, n) memset(x, 0xff, ((n) - 1) / 8 + 1)
#define CLEAR_BIT_ARRAY(x, n) memset(x, 0x00, ((n) - 1) / 8 + 1)
#define NEW_BIT_ARRAY(n) calloc(((n) - 1) / 8 + 1, sizeof(char))
#define COPY_BIT_ARRAY(dest, src, n) memcpy(dest, src, ((n) - 1) / 8 + 1)

double parse_freq(const char *);
int parse_selector(const char *, char *, int);
void print_selector(char *, int);
int build_effects_chain(int, char **, struct effects_chain *, struct stream_info *, char *, const char *);
int build_effects_chain_from_file(struct effects_chain *, struct stream_info *, char *, const char *, const char *);
int gen_argv_from_string(char *, int *, char ***);

#endif
