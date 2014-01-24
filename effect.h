#ifndef _EFFECT_H
#define _EFFECT_H

#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(struct effect_info *, struct stream_info *, int, char **);
};

struct effect {
	struct effect *next;
	struct stream_info istream, ostream;
	double ratio;
	void (*run)(struct effect *, ssize_t *, sample_t *, sample_t *);
	void (*reset)(struct effect *);
	void (*plot)(struct effect *, int);
	void (*drain)(struct effect *, ssize_t *, sample_t *);
	void (*destroy)(struct effect *);
	void *data;
};

struct effects_chain {
	struct effect *head;
	struct effect *tail;
};

struct effect_info * get_effect_info(const char *);
struct effect * init_effect(struct effect_info *, struct stream_info *, int, char **);
void destroy_effect(struct effect *);
void append_effect(struct effects_chain *, struct effect *);
double get_effects_chain_max_ratio(struct effects_chain *);
sample_t * run_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void reset_effects_chain(struct effects_chain *);
void plot_effects_chain(struct effects_chain *, int);
sample_t * drain_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void destroy_effects_chain(struct effects_chain *);
void print_all_effects(void);

#endif
