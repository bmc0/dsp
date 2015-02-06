#include "sampleconv.h"

void write_buf_u8(sample_t *in, char *out, ssize_t s)
{
	uint8_t *outn = (uint8_t *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_U8(in[p]);
}

void read_buf_u8(char *in, sample_t *out, ssize_t s)
{
	uint8_t *inn = (uint8_t *) in;
	while (s-- > 0)
		out[s] = U8_TO_SAMPLE(inn[s]);
}

void write_buf_s8(sample_t *in, char *out, ssize_t s)
{
	int8_t *outn = (int8_t *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S8(in[p]);
}

void read_buf_s8(char *in, sample_t *out, ssize_t s)
{
	int8_t *inn = (int8_t *) in;
	while (s-- > 0)
		out[s] = S8_TO_SAMPLE(inn[s]);
}

void write_buf_s16(sample_t *in, char *out, ssize_t s)
{
	int16_t *outn = (int16_t *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S16(in[p]);
}

void read_buf_s16(char *in, sample_t *out, ssize_t s)
{
	int16_t *inn = (int16_t *) in;
	while (s-- > 0)
		out[s] = S16_TO_SAMPLE(inn[s]);
}

void write_buf_s24(sample_t *in, char *out, ssize_t s)
{
	int32_t *outn = (int32_t *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S24(in[p]);
}

void read_buf_s24(char *in, sample_t *out, ssize_t s)
{
	int32_t *inn = (int32_t *) in;
	while (s-- > 0)
		out[s] = S24_TO_SAMPLE(inn[s]);
}

void write_buf_s32(sample_t *in, char *out, ssize_t s)
{
	int32_t *outn = (int32_t *) out;
	ssize_t p = -1;
	while (++p < s)
		outn[p] = SAMPLE_TO_S32(in[p]);
}

void read_buf_s32(char *in, sample_t *out, ssize_t s)
{
	int32_t *inn = (int32_t *) in;
	while (s-- > 0)
		out[s] = S32_TO_SAMPLE(inn[s]);
}

void write_buf_s24_3(sample_t *in, char *out, ssize_t s)
{
	int32_t v;
	ssize_t p = -1;
	while (++p < s) {
		v = SAMPLE_TO_S24(in[p]);
		out[p * 3 + 0] = (v >> 0) & 0xff;
		out[p * 3 + 1] = (v >> 8) & 0xff;
		out[p * 3 + 2] = (v >> 16) & 0xff;
	}
}

void read_buf_s24_3(char *in, sample_t *out, ssize_t s)
{
	int32_t v;
	while (s-- > 0) {
		v = (in[s * 3 + 0] & 0xff) << 0;
		v |= (in[s * 3 + 1] & 0xff) << 8;
		v |= (in[s * 3 + 2] & 0xff) << 16;
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
