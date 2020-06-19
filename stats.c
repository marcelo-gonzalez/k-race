// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2020 Marcelo Diop-Gonzalez

#include <errno.h>
#include <glib.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_roots.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "stats.h"

struct bucket {
	long *left_edges;
	long *right_edges;
	int count;
	float race_probability;
};

struct learning_sampler {
	int num_params;
	long *params;
	struct bucket *buckets;
	GTree *ordered_buckets;
	struct bucket *current_bucket;
	float explore_probability;
	int found_something;
};

static int bucket_cmp(const void *a, const void *b) {
	const struct bucket *bucket_a = a;
	const struct bucket *bucket_b = b;

	if (bucket_a->race_probability > bucket_b->race_probability)
		return -1;
	if (bucket_a->race_probability < bucket_b->race_probability)
		return 1;
	if (bucket_a > bucket_b)
		return -1;
	if (bucket_a < bucket_b)
		return 1;
	return 0;
}

static void random_point(int n, long *left_edges, long *right_edges, long *dst) {
	for (int i = 0; i < n; i++) {
		dst[i] = left_edges[i] + random() % (right_edges[i] - left_edges[i]);
	}
}

static void set_current_bucket(struct sampler *s, struct bucket *b) {
	struct learning_sampler *ls = s->private;
	ls->current_bucket = b;
	random_point(s->num_params, b->left_edges, b->right_edges, ls->params);
}

struct top_n_arg {
	int idx;
	struct bucket *bucket;
};

// Take a random bucket from among the top n rather
// than just the top one, because the top bucket in this tree
// is the top with respect the measured number of times that
// "triggered_by" happens between "opened_by" and "closed_by".
// This is only a proxy for what we really want (triggering the
// real race), so we could be stuck hammering away at a bucket
// that isn't the "true" optimal one if the config gives a wide window.
// would be good to do something smarter than just the top 10...
static int tree_top_n(void *k, void *v, void *p) {
	struct top_n_arg *arg = p;
	struct bucket *b = k;

	if (b->race_probability < 0.0001 && arg->bucket)
		return 1;

	arg->bucket = b;
	if (arg->idx == 0)
		return 1;
	arg->idx--;
	return 0;
}

static struct bucket *random_top_bucket(GTree *tree) {
	struct top_n_arg arg;

	int size = g_tree_nnodes(tree);
	int n = size < 10 ? size : 10;

	arg.idx = random() % n;
	arg.bucket = NULL;
	g_tree_foreach(tree, tree_top_n, &arg);
	return arg.bucket;
}

static long *learning_next_params(struct sampler *s) {
	struct learning_sampler *ls = s->private;

	if (ls->found_something &&
	    (float)random() / (float) RAND_MAX > ls->explore_probability) {
		set_current_bucket(s, random_top_bucket(ls->ordered_buckets));
		return ls->params;
	}

	int idx = random() % g_tree_nnodes(ls->ordered_buckets);
	set_current_bucket(s, &ls->buckets[idx]);
	return ls->params;
}

static void learning_report(struct sampler *s, int count, int triggers) {
	if (count < 1)
		return;

	struct learning_sampler *ls = s->private;

	if (triggers > 0)
		ls->found_something = 1;

	float p = (float)triggers / (float)count;
	struct bucket *b = ls->current_bucket;

	g_tree_steal(ls->ordered_buckets, b);
	b->race_probability += ((p - b->race_probability) *
				(float)count / (float)(count + b->count));
	b->count += count;
	g_tree_insert(ls->ordered_buckets, b, NULL);
}

static void rand_init(void) {
	int n, seed;
	FILE *f = fopen("/dev/urandom", "r");
	if (!f) {
		fprintf(stderr, "not seeding RNG. opening /dev/urandom: %m\n");
		return;
	}

	n = fread(&seed, sizeof(seed), 1, f);
	if (fclose(f) || n != 1) {
		fprintf(stderr, "not seeding RNG. reading from /dev/urandom: %s\n", strerror(ferror(f)));
		return;
	}
	srandom(seed);
}

static void free_learning_sampler(struct sampler *s) {
	struct learning_sampler *ls = s->private;
	for (int i = 0; i < g_tree_nnodes(ls->ordered_buckets); i++) {
		struct bucket *b = &ls->buckets[i];
		free(b->left_edges);
		free(b->right_edges);
	}
	free(ls->buckets);
	g_tree_unref(ls->ordered_buckets);
	free(ls->params);
	free(ls);
	free(s);
}

static int get_param_boundaries(int num_dimensions, long *durations,
				long **left_edges, long **right_edges) {
	*left_edges = malloc(sizeof(long) * num_dimensions);
	if (!*left_edges) {
		fprintf(stderr, "%s out of memory\n", __func__);
		return -1;
	}
	*right_edges = malloc(sizeof(long) * num_dimensions);
	if (!*right_edges) {
		fprintf(stderr, "%s out of memory\n", __func__);
		return -1;
	}
	for (int i = 0; i < num_dimensions; i++) {
		(*left_edges)[i] = 0;
		(*right_edges)[i] = 0;
		for (int j = 0; j < num_dimensions; j++) {
			(*left_edges)[i] -= durations[j];
			if (i != j)
				(*right_edges)[i] += durations[j];
		}
		(*right_edges)[i] += durations[num_dimensions];
	}
	return 0;
}

static struct sampler *alloc_sampler(int num_params, long *(*next_params)(struct sampler *),
				     void (*destroy)(struct sampler *), void (*report)(struct sampler *, int, int),
				     void *private) {
	rand_init();

	struct sampler *sampler = malloc(sizeof(*sampler));
	if (!sampler)
		return NULL;

	sampler->num_params = num_params;
	sampler->next_params = next_params;
	sampler->destroy = destroy;
	sampler->report = report;
	sampler->private = private;
	return sampler;
}

struct polynomial_params {
	int exp;
	double c;
};

// to find nth root of Y, find positive real root of p(x) = x^n - Y
static double polynomial(double x, void *params) {
	const struct polynomial_params *p = params;

	double ret = x;
	for (int i = 0; i < p->exp-1; i++)
		ret *= x;
	return ret - p->c;
}

static double dpolynomial(double x, void *params) {
	const struct polynomial_params *p = params;

	double ret = p->exp;
	for (int i = 0; i < p->exp-1; i++)
		ret *= x;
	return ret;
}

static void fdfpolynomial(double x, void *params, double *f, double *df) {
	const struct polynomial_params *p = params;
	double y = x;

	for (int i = 0; i < p->exp-2; i++)
		y *= x;
	*df = y * p->exp;
	*f = y * x - p->c;
}

static long nth_root(int n, long x) {
	if (n == 1)
		return x;

	gsl_root_fdfsolver *s = gsl_root_fdfsolver_alloc(gsl_root_fdfsolver_newton);
	if (!s)
		return -ENOMEM;
	struct polynomial_params p = {
		.exp = n,
		.c = x,
	};
	gsl_function_fdf fdf = {
		.f = polynomial,
		.df = dpolynomial,
		.fdf = fdfpolynomial,
		.params = &p,
	};
	double root, root_old = p.c/5+1;
	gsl_root_fdfsolver_set(s, &fdf, root_old);

	int i;
	for (i = 0; i < 10000; i++) {
		int status = gsl_root_fdfsolver_iterate(s);
		if (status != GSL_SUCCESS) {
			fprintf(stderr, "math error: %s\n", gsl_strerror(status));
			gsl_root_fdfsolver_free(s);
			return -1;
		}
		root = gsl_root_fdfsolver_root(s);
		status = gsl_root_test_delta(root, root_old, 0.1, 0);
		if (status == GSL_SUCCESS)
			break;
		if (status != GSL_CONTINUE) {
			fprintf(stderr, "math error: %s\n", gsl_strerror(status));
			gsl_root_fdfsolver_free(s);
			return -1;
		}
		root_old = root;
	}
	if (i == 10000) {
		// should not happen...
		fprintf(stderr, "math error: couldn't find nth root quickly enough. Something is wrong\n");
		gsl_root_fdfsolver_free(s);
		return -1;
	}
	gsl_root_fdfsolver_free(s);
	return root;
}

static int get_bucket_shape(int num_dimensions, const long *left_edges, const long *right_edges,
			    int *num_buckets, long *edge_length, int *dimension_num_buckets) {
	// TODO: instead of hardcoding this totally arbitrary value,
	// would be good to start with a small number of big buckets
	// and split the good ones into smaller ones as we go along
#define MAX_BUCKETS 100000

	long bucket_volume = 1;
	for (int i = 0; i < num_dimensions; i++) {
		long x = bucket_volume * (right_edges[i] - left_edges[i]);
		if (bucket_volume != x / (right_edges[i] - left_edges[i])) {
			// TODO: handle this. unless the above TODO gets done, in which case it doesnt matter
			fprintf(stderr, "Multiplication overflow. Too many k_race_targets given\n");
			return -1;
		}
		bucket_volume = x;
	}
	bucket_volume /= MAX_BUCKETS;
	bucket_volume++;

	*edge_length = nth_root(num_dimensions, bucket_volume);
	if (*edge_length < 0)
		return -*edge_length;
	if (*edge_length < 100)
		*edge_length = 100;

	*num_buckets = 1;
	for (int i = 0; i < num_dimensions; i++) {
		// round up division
		dimension_num_buckets[i] = (right_edges[i] - left_edges[i] + *edge_length - 1) / *edge_length;
		*num_buckets *= dimension_num_buckets[i];
	}
	return 0;
}

// splits the possible params into different buckets, and then treats
// the problem like a multi armed bandit
struct sampler *alloc_learning_sampler(int num_funcs, long *durations,
				       float explore_probability) {
	struct learning_sampler *ls = malloc(sizeof(*ls));
	if (!ls)
		return NULL;

	int err = ENOMEM;
	int num_dimensions = num_funcs - 1;

	ls->explore_probability = explore_probability;
	ls->found_something = 0;

	long *left_edges, *right_edges;
	if (get_param_boundaries(num_dimensions, durations,
				 &left_edges, &right_edges))
		goto out_free_ls;

	int *dimension_num_buckets = malloc(sizeof(int) * num_dimensions);
	if (!dimension_num_buckets) {
		free(left_edges);
		free(right_edges);
		goto out_free_ls;
	}

	long edge_length;
	int num_buckets;
	err = get_bucket_shape(num_dimensions, left_edges, right_edges,
			       &num_buckets, &edge_length, dimension_num_buckets);
	if (err) {
		free(dimension_num_buckets);
		free(left_edges);
		free(right_edges);
		goto out_free_ls;
	}

	ls->buckets = malloc(sizeof(struct bucket) * num_buckets);
	if (!ls->buckets) {
		free(dimension_num_buckets);
		free(left_edges);
		free(right_edges);
		goto out_free_ls;
	}
	memset(ls->buckets, 0, sizeof(struct bucket) * num_buckets);

	ls->ordered_buckets = g_tree_new(bucket_cmp);
	ls->params = malloc(sizeof(long) * num_dimensions);
	if (!ls->params) {
		free(dimension_num_buckets);
		free(left_edges);
		free(right_edges);
		goto out_free_tree;
	}

	for (int i = 0; i < num_buckets; i++) {
		struct bucket *b = &ls->buckets[i];
		b->left_edges = malloc(sizeof(long) * num_dimensions);
		b->right_edges = malloc(sizeof(long) * num_dimensions);
		if (!b->left_edges || !b->right_edges) {
			free(left_edges);
			free(right_edges);
			free(dimension_num_buckets);
			goto out_free_buckets;
		}
		int q = 1;
		for (int j = 0; j < num_dimensions; j++) {
			int idx = i / q % dimension_num_buckets[j];
			b->left_edges[j] = left_edges[j] + edge_length * idx;
			b->right_edges[j] = b->left_edges[j] + edge_length;
			q *= dimension_num_buckets[j];
		}
		g_tree_insert(ls->ordered_buckets, b, NULL);
	}

	free(dimension_num_buckets);
	free(left_edges);
	free(right_edges);

	struct sampler *s = alloc_sampler(num_dimensions, learning_next_params,
					  free_learning_sampler, learning_report, ls);
	if (!s)
		goto out_free_buckets;
	return s;

out_free_buckets:
	for (int i = 0; i < num_buckets; i++) {
		struct bucket *b = &ls->buckets[i];
		if (b->left_edges)
			free(b->left_edges);
		if (b->right_edges)
			free(b->right_edges);
	}
	free(ls->params);
out_free_tree:
	g_tree_unref(ls->ordered_buckets);
	free(ls->buckets);
out_free_ls:
	free(ls);
	if (err == ENOMEM)
		fprintf(stderr, "%s: OOM\n", __func__);
	return NULL;
}

struct random_sampler {
	long *left_edges;
	long *right_edges;
	long *params;
};

static long *random_next_params(struct sampler *s) {
	struct random_sampler *rs = s->private;

	random_point(s->num_params, rs->left_edges,
		     rs->right_edges, rs->params);
	return rs->params;
}

static void random_report(struct sampler *s, int foo, int bar) {}

static void random_destroy(struct sampler *s) {
	struct random_sampler *rs = s->private;
	free(rs->params);
	free(rs->left_edges);
	free(rs->right_edges);
	free(rs);
	free(s);
}

struct sampler *alloc_random_sampler(int num_funcs, long *durations) {
	struct random_sampler *rs = malloc(sizeof(*rs));
	if (!rs)
		return NULL;
	int num_dimensions = num_funcs - 1;

	if (get_param_boundaries(num_dimensions, durations,
				 &rs->left_edges, &rs->right_edges)) {
		free(rs);
		return NULL;
	}

	rs->params = malloc(num_dimensions * sizeof(long));
	if (!rs->params)
		goto free_edges;

	struct sampler *s = alloc_sampler(num_dimensions, random_next_params,
					  random_destroy, random_report, rs);
	if (!s)
		goto free_params;
	return s;

free_params:
	free(rs->params);
free_edges:
	free(rs->left_edges);
	free(rs->right_edges);
	free(rs);
	return NULL;
}
