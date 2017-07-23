#ifndef _EFFECT_H
#define _EFFECT_H

#include "dsp.h"

struct effect_info {
	const char *name;
	const char *usage;
	struct effect * (*init)(struct effect_info *, struct stream_info *, char *, const char *, int, char **);
};

struct effect {
	struct effect *next;
	const char *name;
	struct stream_info istream, ostream;
	char *channel_selector;  /* for use *only* by the effect */
	double ratio, worst_case_ratio;
	/* All functions may be NULL except run() */
	void (*run)(struct effect *, ssize_t *, sample_t *, sample_t *);
	ssize_t (*delay)(struct effect *);  /* returns the latency in frames at ostream.fs */
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
void destroy_effect(struct effect *);
void append_effect(struct effects_chain *, struct effect *);
int build_effects_chain(int, char **, struct effects_chain *, struct stream_info *, char *, const char *);
int build_effects_chain_from_file(struct effects_chain *, struct stream_info *, char *, const char *, const char *);
double get_effects_chain_max_ratio(struct effects_chain *);
double get_effects_chain_total_ratio(struct effects_chain *);
sample_t * run_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
double get_effects_chain_delay(struct effects_chain *);
void reset_effects_chain(struct effects_chain *);
void plot_effects_chain(struct effects_chain *, int);
sample_t * drain_effects_chain(struct effects_chain *, ssize_t *, sample_t *, sample_t *);
void destroy_effects_chain(struct effects_chain *);
void print_all_effects(void);

#endif
