// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2020 Marcelo Diop-Gonzalez

#define _GNU_SOURCE

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "k-race.h"
#include "stats.h"
#include "trace.h"

enum opts {
	opt_config_file = 200,
};

static struct option long_opts[] = {
	{"config-file", required_argument, 0, opt_config_file},
	{"out-file", required_argument, 0, 'o'},
	{"explore-probability", required_argument, 0, 'e'},
	{"no-trace", no_argument, 0, 'n'},
	{0, 0, 0, 0},
};

int k_race_parse_options(struct k_race_options *opts,
			 int argc, char **argv) {
	int opt;
	int explore_set = 0;

	opts->notrace = 0;
	opts->config_file = "config.json";
	opts->out_file = NULL;
	opts->explore_probability = 0.1;

	while ((opt = getopt_long(argc, argv, "e:no:", long_opts, NULL)) != -1) {
		char *end;
		switch (opt) {
		case '?':
			return -1;
		case 'e':
			explore_set = 1;
			opts->explore_probability = strtof(optarg, &end);
			if (*end || opts->explore_probability > 1 ||
			    opts->explore_probability < 0) {
				fprintf(stderr, "Bad --explore_probability argument: %s\n", optarg);
				return -1;
			}
			break;
		case 'n':
			opts->notrace = 1;
			break;
		case 'o':
			opts->out_file = optarg;
			break;
		case opt_config_file:
			opts->config_file = optarg;
			break;
		}
	}

	if (explore_set && opts->notrace) {
		fprintf(stderr, "--explore_probability does nothing with --no-trace\n");
		return -1;
	}
	if (opts->out_file && opts->notrace) {
		fprintf(stderr, "--out-file and --no-trace both given, but there is no output with --no-trace\n");
		return -1;
	}
	if (!opts->out_file)
		opts->out_file = "out.csv";
	return 0;
}

struct worker {
	struct worker_context *ctx;
	struct k_race_target target;
	long *duration;
	struct timespec sleep_time;
	pthread_t thread;
	pid_t pid;
};

struct worker_context {
	int num_workers;
	struct worker *workers;
	long *durations;
	void *user_context;
	struct k_race_callbacks callbacks;
	pthread_barrier_t barrier;
	unsigned int samples;
	int round_finished;
	int round_pre;
	int start;
	int finished;
	int stop;
	int error;
	pthread_cond_t wait_start;
	pthread_cond_t wait_end;
	pthread_mutex_t mutex;
};

static int set_sched_opts(pthread_attr_t *attr,
			  struct k_race_sched_config *config) {
	int err = pthread_attr_setschedpolicy(attr, config->sched_policy);
	if (err) {
		fprintf(stderr, "pthread_attr_setschedpolicy(): %s\n",
			strerror(err));
		return err;
	}
	err = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
	if (err) {
		fprintf(stderr, "pthread_attr_setinheritsched(): %s\n",
			strerror(err));
		return err;
	}
	err = pthread_attr_setschedparam(attr, &config->sched_param);
	if (err) {
		fprintf(stderr, "pthread_attr_setschedparam(): %s\n",
			strerror(err));
		return err;
	}
	err = pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &config->cpus);
	if (err) {
		fprintf(stderr, "pthread_attr_setaffinity_np(): %s\n",
			strerror(err));
		return err;
	}
	return 0;
}

static int dummy_func(void *user, void *arg) { return 0; }

static void stop_workers(struct worker_context *ctx) {
	// can't exit yet because other threads might be
	// in pthread_barrier_wait(). would take extra synchronization
	// to exit now, so forget it and just set all funcs
	// to dummy_func and finish getting the needed samples
	for (int i = 0; i < ctx->num_workers; i++)
		ctx->workers[i].target.func = dummy_func;
	pthread_mutex_lock(&ctx->mutex);
	ctx->stop = 1;
	pthread_cond_broadcast(&ctx->wait_start);
	pthread_cond_broadcast(&ctx->wait_end);
	pthread_mutex_unlock(&ctx->mutex);
}

static inline void pre_round(struct worker_context *ctx) {
	if (ctx->callbacks.pre && !ctx->stop &&
	    __atomic_add_fetch(&ctx->round_pre, 1, __ATOMIC_RELAXED) == ctx->num_workers) {
		__atomic_store_n(&ctx->round_pre, 0, __ATOMIC_RELAXED);
		int err = ctx->callbacks.pre(ctx->user_context);
		if (err) {
			fprintf(stderr, "Pre callback failed\n");
			ctx->error = -1;
			stop_workers(ctx);
		}
	}

	pthread_barrier_wait(&ctx->barrier);
}

static inline void post_round(struct worker_context *ctx) {
	if (!ctx->callbacks.post || ctx->stop ||
	    __atomic_add_fetch(&ctx->round_finished, 1, __ATOMIC_RELAXED) < ctx->num_workers)
		return;

	__atomic_store_n(&ctx->round_finished, 0,
			 __ATOMIC_RELAXED);
	int err = ctx->callbacks.post(ctx->user_context);
	if (err) {
		fprintf(stderr, "Post callback failed\n");
		ctx->error = -1;
		stop_workers(ctx);
	}
}

static int measure_duration(struct worker_context *ctx,
			    struct worker *worker) {
	int error = 0;
	long first = 0, second = 0, third = 0;

	for (int i = 0; i < 100; i++) {
		struct timespec start, end;

		// TODO: consider doing this as we go in worker_func().
		// If the durations depend somehow on the offsets
		// themselves, then we will not find that out here. e.g.
		//
		// func1() {
		//   close(fd);
		// }
		//
		// func2() {
		//   int n = write(fd);
		//   if (n > 0)
		//     read(fd);
		// }

		pre_round(ctx);
		clock_gettime(CLOCK_MONOTONIC, &start);
		int err = worker->target.func(ctx->user_context,
					      worker->target.arg);
		clock_gettime(CLOCK_MONOTONIC, &end);
		if (__builtin_expect(err, 0)) {
			ctx->error = -1;
			fprintf(stderr, "User funcion returned error: %d\n", err);
			stop_workers(ctx);
		}
		post_round(ctx);

		long duration = 1000000000 * (end.tv_sec - start.tv_sec);
		if (end.tv_nsec > start.tv_nsec)
			duration += end.tv_nsec - start.tv_nsec;
		else
			duration -= start.tv_nsec - end.tv_nsec;
		if (duration > first) {
			third = second;
			second = first;
			first = duration;
		} else if (duration > second) {
			third = second;
			second = duration;
		} else if (duration > third)
			third = duration;
	}
	*worker->duration = third;
	return error;
}

static inline int wait_start(struct worker_context *ctx) {
	pthread_mutex_lock(&ctx->mutex);
	while (!ctx->start && !ctx->stop)
		pthread_cond_wait(&ctx->wait_start, &ctx->mutex);
	pthread_mutex_unlock(&ctx->mutex);
	return !ctx->stop;
}

static inline void workers_finished(struct worker_context *ctx) {
	pthread_mutex_lock(&ctx->mutex);
	// OK because this function is only ever called after
	// a pthread_barrier_wait() has happened since the last
	// wait_start(), so nobody is still in that function
	ctx->start = 0;
	if (++ctx->finished == ctx->num_workers)
		pthread_cond_signal(&ctx->wait_end);
	pthread_mutex_unlock(&ctx->mutex);
}

static void *worker_func(void *p) {
	struct worker *worker = p;
	struct worker_context *ctx = worker->ctx;

	worker->pid = syscall(__NR_gettid);

	if (!wait_start(ctx))
		return NULL;

	int err = measure_duration(ctx, worker);
	if (err)
		return NULL;

	workers_finished(ctx);

	while (1) {
		if (!wait_start(ctx))
			return NULL;

		for (int i = 0; i < ctx->samples; i++) {
			pre_round(ctx);
			nanosleep(&worker->sleep_time, NULL);
			int err = worker->target.func(ctx->user_context,
						      worker->target.arg);
			if (__builtin_expect(err, 0)) {
				ctx->error = -1;
				fprintf(stderr, "User funcion returned error: %d\n", err);
				stop_workers(ctx);
			}
			post_round(ctx);
		}
		workers_finished(ctx);
	}
	return NULL;
}

static int join_workers(struct worker_context *ctx) {
	int ret = 0;
	for (int i = 0; i < ctx->num_workers; i++) {
		if (!ctx->workers[i].thread)
			continue;

		int err = pthread_join(ctx->workers[i].thread, NULL);
		if (err) {
			fprintf(stderr, "pthread_join: %s\n", strerror(err));
			ret = err;
		}
	}
	return ret;
}

static int run_workers(struct worker_context *ctx) {
	pthread_mutex_lock(&ctx->mutex);
	ctx->start = 1;
	ctx->finished = 0;
	ctx->error = 0;
	pthread_cond_broadcast(&ctx->wait_start);
	while (ctx->finished < ctx->num_workers && !ctx->stop)
		pthread_cond_wait(&ctx->wait_end, &ctx->mutex);
	pthread_mutex_unlock(&ctx->mutex);
	return ctx->error;
}

static int start_workers(struct worker_context *ctx,
			 struct k_race_config *config) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	for (int i = 0; i < ctx->num_workers; i++) {
		struct k_race_sched_config *cfg = &config->sched_config[i];
		struct worker *worker = &ctx->workers[i];

		int err = set_sched_opts(&attr, cfg);
		if (err) {
			stop_workers(ctx);
			pthread_attr_destroy(&attr);
			return err;
		}
		err = pthread_create(&worker->thread, &attr,
				     worker_func, worker);
		if (err) {
			fprintf(stderr, "pthread create error: %s\n", strerror(err));
			stop_workers(ctx);
			pthread_attr_destroy(&attr);
			return err;
		}
	}
	pthread_attr_destroy(&attr);

	return run_workers(ctx);
}

static void set_offsets(struct worker_context *ctx, const long *params) {
	long min = 0;
	ctx->durations[ctx->num_workers-1] = 0;
	for (int i = 0; i < ctx->num_workers-1; i++) {
		ctx->durations[i] = params[i];
		if (params[i] < min) {
			min = params[i];
		}
	}
	for (int i = 0; i < ctx->num_workers; i++) {
		ctx->durations[i] -= min;
		ctx->workers[i].sleep_time.tv_sec = ctx->durations[i] / 1000000000;
		ctx->workers[i].sleep_time.tv_nsec = ctx->durations[i] % 1000000000;
	}
}

static int create_workers(void *context, int n,
			  struct k_race_target *targets,
			  struct k_race_options *opts,
			  struct k_race_callbacks *callbacks,
			  struct worker_context *ctx) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->num_workers = n;
	ctx->durations = malloc(sizeof(long) * n);
	if (!ctx->durations)
		return ENOMEM;
	struct worker *workers = malloc(sizeof(struct worker) * n);
	if (!workers) {
		free(ctx->durations);
		return ENOMEM;
	}
	memset(workers, 0, sizeof(struct worker) * n);
	for (int i = 0; i < n; i++) {
		workers[i].target = targets[i];
		workers[i].ctx = ctx;
		workers[i].duration = &ctx->durations[i];
	}

	ctx->user_context = context;
	if (callbacks)
		memcpy(&ctx->callbacks, callbacks, sizeof(*callbacks));
	ctx->workers = workers;
	pthread_barrier_init(&ctx->barrier, NULL, ctx->num_workers);
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_cond_init(&ctx->wait_start, NULL);
	pthread_cond_init(&ctx->wait_end, NULL);
	return 0;
}

static void free_workers(struct worker_context *ctx) {
	free(ctx->workers);
	free(ctx->durations);
	pthread_barrier_destroy(&ctx->barrier);
	pthread_cond_destroy(&ctx->wait_start);
	pthread_cond_destroy(&ctx->wait_end);
	pthread_mutex_destroy(&ctx->mutex);
}

static void print_data_header(FILE *out, int num_params, const char *name) {
	for (int i = 0; i < num_params; i++) {
		fprintf(out, "offset %d, ", i);
	}
	fprintf(out, "%s count, ", name);
	fprintf(out, "%s triggers\n", name);
}

static void print_data(FILE *out, int n, long *params, int counts, int triggers) {
	for (int i = 0; i < n; i++)
		fprintf(out, "%ld, ", params[i]);

	float pct = 0;
	if (counts)
		pct = (float)triggers/(float)counts;
	fprintf(out, "%d, %f\n", counts, pct);
}

static int add_pids(struct tracer *tr, struct worker_context *ctx) {
	for (int i = 0; i < ctx->num_workers; i++) {
		int err = tracer_add_pid(tr, ctx->workers[i].pid);
		if (err)
			return err;
	}
	return 0;
}

static int adjust_samples(unsigned int *samples, unsigned int *overrun,
			  unsigned int entries) {
	unsigned int old_overrun = *overrun;
	int err = ftrace_overrun(overrun);
	if (err)
		return err;
	int s = *samples;
	// TODO: do something better than that
	*samples = entries * s/((*overrun - old_overrun + entries)*2);
	return 0;
}

static int experiment_loop(struct worker_context *ctx,
			   struct k_race_config *config,
			   float explore_probability, const char *out_file) {
	struct tracer *tr = alloc_tracer(config);
	if (!tr)
		return ENOMEM;
	int err = ftrace_init(tr);
	if (err)
		goto out_free_tracer;

	unsigned int overrun;
	err = ftrace_overrun(&overrun);
	if (err)
		goto out_ftrace_exit;

	err = start_workers(ctx, config);
	if (err)
		goto out_ftrace_exit;
	err = add_pids(tr, ctx);
	if (err)
		goto out_stop_workers;

	struct sampler *sampler = alloc_learning_sampler(ctx->num_workers, ctx->durations, explore_probability);
	if (!sampler)
		goto out_stop_workers;

	FILE *out = fopen(out_file, "w");
	if (!out) {
		fprintf(stderr, "opening %s: %m\n", out_file);
		goto out_destroy_sampler;
	}
	print_data_header(out, sampler->num_params, config->name);

	ctx->samples = 100;
	while (1) {
		unsigned int samples = 0;
		int counts = 0, triggers = 0;
		long *params = sampler->next_params(sampler);

		set_offsets(ctx, params);
		while (samples < 100) {
			err = enable_tracing();
			if (err)
				goto out_close_file;
			err = run_workers(ctx);
			if (err)
				goto out_close_file;
			err = disable_tracing();
			if (err)
				goto out_close_file;
			int entries, _counts, _triggers;
			int missed_events = tracer_collect_stats(tr, &entries, &_counts, &_triggers);
			if (!missed_events) {
				samples += ctx->samples;
				counts += _counts;
				triggers += _triggers;
			} else if (ctx->samples > 2) {
				err = adjust_samples(&ctx->samples, &overrun, entries);
				if (err)
					goto out_close_file;
				if (ctx->samples < 2) {
					fprintf(stderr, "ftrace buffers filling quickly. using 2 samples per run. might be losing data\n");
					ctx->samples = 2;
				}
			}
		}
		sampler->report(sampler, counts, triggers);
		print_data(out, sampler->num_params, params, counts, triggers);
	}

out_close_file:
	fclose(out);
out_destroy_sampler:
	sampler->destroy(sampler);
out_stop_workers:
	stop_workers(ctx);
out_ftrace_exit:
	ftrace_exit();
out_free_tracer:
	free_tracer(tr);
	return err;
}

static int notrace_loop(struct worker_context *ctx, struct k_race_config *config) {
	int err = start_workers(ctx, config);
	if (err)
		return err;

	struct sampler *sampler = alloc_random_sampler(ctx->num_workers, ctx->durations);
	if (!sampler) {
		stop_workers(ctx);
		return ENOMEM;
	}

	ctx->samples = 1000;
	while (1) {
		set_offsets(ctx, sampler->next_params(sampler));
		err = run_workers(ctx);
		if (err)
			break;
	}

	sampler->destroy(sampler);
	return err;
}

int k_race_loop(struct k_race_options *opts,
		int num_targets, struct k_race_target *targets,
		struct k_race_callbacks *callbacks, void *user) {
	int err = 0;
	int err2;
	struct worker_context ctx;

	if (num_targets < 2) {
		fprintf(stderr, "Must supply at least two targets\n");
		return EINVAL;
	}

	// TODO return err
	struct k_race_config *config = k_race_config_parse(num_targets,
							   opts->config_file);
	if (!config)
		return ENOMEM;

	if (create_workers(user, num_targets,
			   targets, opts, callbacks, &ctx))
		goto out_config_free;

	if (!opts->notrace)
		err = experiment_loop(&ctx, config, opts->explore_probability, opts->out_file);
	else
		err = notrace_loop(&ctx, config);

	err2 = join_workers(&ctx);
	if (!err)
		err = err2;
	free_workers(&ctx);

out_config_free:
	k_race_config_free(config);
	return err;
}
