#ifndef STATS_H
#define STATS_H

#include <glib.h>

struct sampler {
	int num_params;
	long *(*next_params)(struct sampler *s);
	void (*report)(struct sampler *s, int counts, int triggers);
	void (*destroy)(struct sampler *s);
	void *private;
};

// TODO: granularity option

struct sampler *alloc_learning_sampler(int num_dimensions, long *durations,
				       float explore_probability);
struct sampler *alloc_random_sampler(int num_dimensions, long *durations);

#endif
