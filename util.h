#ifndef _UTIL_H
#define _UTIL_H

#include <string.h>
#include "dsp.h"

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define MAXIMUM(a, b) (((a) > (b)) ? (a) : (b))
#define MINIMUM(a, b) (((a) < (b)) ? (a) : (b))
#define CHECK_RANGE(cond, name, action) \
	if (!(cond)) { \
		LOG(LL_ERROR, "dsp: %s: error: %s out of range\n", argv[0], name); \
		action; \
	}
#if 0
#define GET_BIT(x, o) (((char *) x)[(int) (o) / 8] & (1 << ((int) (o) % 8)))
#define SET_BIT(x, o) ((char *) x)[(int) (o) / 8] |= (1 << ((int) (o) % 8))
#define SET_SELECTOR(x, n) memset(x, 0xff, ((n) - 1) / 8 + 1)
#define CLEAR_SELECTOR(x, n) memset(x, 0x00, ((n) - 1) / 8 + 1)
#define NEW_SELECTOR(n) calloc(((n) - 1) / 8 + 1, sizeof(char))
#define COPY_SELECTOR(dest, src, n) memcpy(dest, src, ((n) - 1) / 8 + 1)
#else
#define GET_BIT(x, o) ((x)[(o)])
#define SET_BIT(x, o) (x)[(o)] = 1
#define SET_SELECTOR(x, n) memset(x, 0x01, n)
#define CLEAR_SELECTOR(x, n) memset(x, 0x00, n)
#define NEW_SELECTOR(n) calloc(n, sizeof(char))
#define COPY_SELECTOR(dest, src, n) memcpy(dest, src, n)
#endif
#define PM_RAND_MAX 2147483647

double parse_freq(const char *);
int parse_selector(const char *, char *, int);
void print_selector(const char *, int);
int gen_argv_from_string(const char *, int *, char ***);
char * get_file_contents(const char *);
char * construct_full_path(const char *, const char *);

static __inline__ long unsigned int pm_rand(void)
{
	static long unsigned int s = 1;
	long unsigned int h, l;

	l = 16807 * (s & 0xffff);
	h = 16807 * (s >> 16);
	l += (h & 0x7fff) << 16;
	l += h >> 15;
	l = (l & 0x7fffffff) + (l >> 31);
	return (s = l);
}

static __inline__ sample_t tpdf_dither_sample(sample_t s, int prec)
{
	if (prec < 1 || prec > 32)
		return s;
	unsigned long int d = (unsigned long int) 1 << (prec - 1);
	sample_t m = 1 / ((sample_t) PM_RAND_MAX * d);
	sample_t n1 = (sample_t) pm_rand() * m;
	sample_t n2 = (sample_t) pm_rand() * m;
	return s + n1 - n2;
}

#endif
