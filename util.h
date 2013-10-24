#ifndef _UTIL_H
#define _UTIL_H

#define LENGTH(x) (sizeof(x) / sizeof(x[0]))
double parse_freq(const char *);

#define CHECK_RANGE(cond, name) \
	if (!(cond)) { \
		LOG(LL_ERROR, "dsp: %s: error: %s out of range\n", argv[0], name); \
		return NULL; \
	}

#endif
