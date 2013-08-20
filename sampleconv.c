#include <math.h>
#include "sampleconv.h"

void write_buf_u8(sample_t *in, char *out, ssize_t s)
{
	unsigned char *outn = (unsigned char *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_U8(in[p]);
}

void read_buf_u8(char *in, sample_t *out, ssize_t s)
{
	unsigned char *inn = (unsigned char *) in;
	while (s-- > 0)
		out[s] = U8_TO_SAMPLE(inn[s]);
}

void write_buf_s8(sample_t *in, char *out, ssize_t s)
{
	signed char *outn = (signed char *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S8(in[p]);
}

void read_buf_s8(char *in, sample_t *out, ssize_t s)
{
	signed char *inn = (signed char *) in;
	while (s-- > 0)
		out[s] = S8_TO_SAMPLE(inn[s]);
}

void write_buf_s16(sample_t *in, char *out, ssize_t s)
{
	signed short *outn = (signed short *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S16(in[p]);
}

void read_buf_s16(char *in, sample_t *out, ssize_t s)
{
	signed short *inn = (signed short *) in;
	while (s-- > 0)
		out[s] = S16_TO_SAMPLE(inn[s]);
}

void write_buf_s24(sample_t *in, char *out, ssize_t s)
{
	signed int *outn = (signed int *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S24(in[p]);
}

void read_buf_s24(char *in, sample_t *out, ssize_t s)
{
	signed int *inn = (signed int *) in;
	while (s-- > 0)
		out[s] = S24_TO_SAMPLE(inn[s]);
}

void write_buf_s32(sample_t *in, char *out, ssize_t s)
{
	signed int *outn = (signed int *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S32(in[p]);
}

void read_buf_s32(char *in, sample_t *out, ssize_t s)
{
	signed int *inn = (signed int *) in;
	while (s-- > 0)
		out[s] = S32_TO_SAMPLE(inn[s]);
}

void write_buf_s24_3(sample_t *in, char *out, ssize_t s)
{
	signed int v;
	ssize_t p = -1;
	while (++p < s) {
		v = SAMPLE_TO_S24(in[p]);
		out[p * 3 + 2] = (unsigned char) (v >> 16);
		out[p * 3 + 1] = (unsigned char) (v >> 8);
		out[p * 3 + 0] = (unsigned char) (v >> 0);
	}
}

void read_buf_s24_3(char *in, sample_t *out, ssize_t s)
{
	signed int v;
	while (s-- > 0) {
		v = (unsigned int) in[s * 3 + 2] << 0;
		v |= (unsigned int) in[s * 3 + 1] << 8;
		v |= (unsigned int) in[s * 3 + 0] << 16;
		out[s] = S24_TO_SAMPLE(v);
	}
}

void write_buf_float(sample_t *in, char *out, ssize_t s)
{
	float *outn = (float *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_FLOAT(in[p]);
}

void read_buf_float(char *in, sample_t *out, ssize_t s)
{
	float *inn = (float *) in;
	while (s-- > 0)
		out[s] = FLOAT_TO_SAMPLE(inn[s]);
}

void write_buf_double(sample_t *in, char *out, ssize_t s)
{
	double *outn = (double *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_DOUBLE(in[p]);
}

void read_buf_double(char *in, sample_t *out, ssize_t s)
{
	double *inn = (double *) in;
	while (s-- > 0)
		out[s] = DOUBLE_TO_SAMPLE(inn[s]);
}
