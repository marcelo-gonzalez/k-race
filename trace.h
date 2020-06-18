#ifndef TRACE_H
#define TRACE_H

#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <traceevent/event-parse.h>
#include <traceevent/kbuffer.h>

#include "config.h"

struct tracer;

struct tracer *alloc_tracer(struct k_race_config *config);
void free_tracer(struct tracer *clr);

int ftrace_init(struct tracer *clr);
int tracer_add_pid(struct tracer *clr, pid_t pid);
int ftrace_exit(void);
int tracer_collect_stats(struct tracer *clr, int *entries,
			 int *counts, int *triggers);

int ftrace_overrun(unsigned int *overrun);

int disable_tracing(void);
int enable_tracing(void);

#endif
