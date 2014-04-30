#include <stdlib.h>
#include "dither.h"

sample_t tpdf_dither_sample(sample_t s, int prec)
{
	if (prec < 1 || prec > 32)
		return s;
	sample_t d = (unsigned long int) 1 << (prec - 1);
	sample_t n1 = (sample_t) random() / RAND_MAX / d;
	sample_t n2 = (sample_t) random() / RAND_MAX / d;
	return s + n1 - n2;
}
