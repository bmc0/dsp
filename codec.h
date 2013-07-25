#ifndef _CODEC_H
#define _CODEC_H

#include "dsp.h"

enum {
	CODEC_MODE_READ  = 1 << 0,
	CODEC_MODE_WRITE = 1 << 1,
};

enum {
	CODEC_ENDIAN_DEFAULT,
	CODEC_ENDIAN_BIG,
	CODEC_ENDIAN_LITTLE,
	CODEC_ENDIAN_NATIVE,
};

struct codec {
	struct codec *next;
	const char *type, *path, *enc;
	int fs, prec, channels, interactive;
	size_t frames;
	ssize_t (*read)(struct codec *, sample_t *, ssize_t);
	ssize_t (*write)(struct codec *, sample_t *, ssize_t);
	ssize_t (*seek)(struct codec *, ssize_t);
	ssize_t (*delay)(struct codec *);
	void (*reset)(struct codec *);  /* drop pending frames */
	void (*pause)(struct codec *, int);
	void (*destroy)(struct codec *);
	void *data;
};

struct codec_list {
	struct codec *head;
	struct codec *tail;
};

struct codec * init_codec(const char *, int, const char *, const char *, int, int, int);
void destroy_codec(struct codec *);
void append_codec(struct codec_list *, struct codec *);
void destroy_codec_list_head(struct codec_list *);
void destroy_codec_list(struct codec_list *);
void print_all_codecs(void);

#endif
