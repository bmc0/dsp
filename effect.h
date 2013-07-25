#ifndef _EFFECT_H
#define _EFFECT_H

#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(struct effect_info *, int, char **);
};

struct effect {
	struct effect *next;
	void (*run)(struct effect *, sample_t *);
	void (*plot)(struct effect *, int);
	void (*destroy)(struct effect *);
	void *data;
};

struct effects_chain {
	struct effect *head;
	struct effect *tail;
};

struct effect_info * get_effect_info(const char *);
struct effect * init_effect(struct effect_info *, int, char **);
void destroy_effect(struct effect *);
void append_effect(struct effects_chain *, struct effect *);
void destroy_effects_chain(struct effects_chain *);
void run_effects_chain(struct effects_chain *, sample_t *);
void plot_effects_chain(struct effects_chain *);
void print_all_effects(void);

#endif
