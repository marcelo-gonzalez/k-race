// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2020 Marcelo Diop-Gonzalez

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <traceevent/event-parse.h>
#include <traceevent/kbuffer.h>
#include <tracefs/tracefs.h>
#include <unistd.h>

#include "config.h"
#include "trace.h"

static FILE *tracing_on;
static int page_size;

static int read_file(FILE *file, char **out) {
	int n;
	int pos = 0;
	int size = page_size;
	char *buf = malloc(size);
	if (!buf)
		return -ENOMEM;
	while ((n = fread(buf+pos, 1, page_size, file)) > 0) {
		pos += n;
		if (pos >= size) {
			size *= 2;
			char *b = realloc(buf, size);
			if (!b) {
				free(buf);
				return -ENOMEM;
			}
			buf = b;
		}
	}
	*out = buf;
	return pos;
}

static int set_tracer(const char *tracer) {
	char *path = tracefs_get_tracing_file("current_tracer");
	if (!path)
		return ENOENT; // could also be ENOMEM, but maybe less likely
	FILE *current_tracer;
	int err = 0;
	current_tracer = fopen(path, "w");
	if (!current_tracer) {
		err = errno;
		perror("opening current_tracer");
		free(path);
		return err;
	}
	free(path);
	if(fputs(tracer, current_tracer) == EOF)
		err = errno;
	if (fclose(current_tracer) == EOF)
		err = errno;
	if (err) {
		fprintf(stderr, "setting current_tracer to %s: %s\n", tracer, strerror(err));
	}
	return err;
}

#define KPROBE_LENGTH 65

struct race_point {
	char kprobe_type;
	char kprobe_name[KPROBE_LENGTH];
	char kprobe[KPROBE_LENGTH];
	unsigned long long event_id;
	int opens;
	int triggers;
	int closes;
};

struct race_event {
	unsigned long long time;
	unsigned long long pid;
	struct race_point *point;
};

static void clear_kprobe(FILE *events, const char *name) {
	char *filename;
	if (asprintf(&filename, "events/kprobes/%s/enable", name) == -1)
		return;
	char *path = tracefs_get_tracing_file(filename);
	if (!path) {
		free(filename);
		return;
	}
	free(filename);
	FILE *enable = fopen(path, "w");
	if (!enable) {
		tracefs_put_tracing_file(path);
		return;
	}
	tracefs_put_tracing_file(path);
	fputc('0', enable);
	fclose(enable);

	int close = 0;
	if (!events) {
		path = tracefs_get_tracing_file("kprobe_events");
		if (!path)
			return;
		events = fopen(path, "a+");
		if (!events) {
			tracefs_put_tracing_file(path);
			return;
		}
		tracefs_put_tracing_file(path);
		close = 1;
	}
	fprintf(events, "-:%s\n", name);
	if (close)
		fclose(events);
}

static int num_kprobes;
static char **kprobes;

static int add_kprobe(FILE *kprobe_events, struct race_point *p,
		      struct tep_handle *tep, int idx) {
	int err;

	if (!(fprintf(kprobe_events, "%c:%s %s\n",
		      p->kprobe_type, p->kprobe_name, p->kprobe) > 0 &&
	      fflush(kprobe_events) != EOF)) {
		fprintf(stderr, "adding kprobe \"%c:%s %s\": %m\n",
			p->kprobe_type, p->kprobe_name, p->kprobe);
		return errno;
	}

	char **k = realloc(kprobes, (num_kprobes + 1) * sizeof(char *));
	if (!k)
		return ENOMEM;
	kprobes = k;
	kprobes[num_kprobes++] = p->kprobe_name;

	char *filename;
	if (asprintf(&filename, "events/kprobes/%s/format", p->kprobe_name) == -1) {
		err = ENOMEM;
		goto out_err;
	}
	char *path = tracefs_get_tracing_file(filename);
	if (!path) {
		free(filename);
		err = ENOMEM;
		goto out_err;
	}
	free(filename);
	FILE *f = fopen(path, "r");
	if (!f) {
		err = errno;
		free(path);
		goto out_err;
	}
	free(path);
	char *buf;
	int sz = read_file(f, &buf);
	if (sz < 0) {
		fclose(f);
		err = -sz;
		goto out_err;
	}
	fclose(f);
	err = tep_parse_event(tep, buf, sz, "kprobes");
	free(buf);
	if (err) {
		goto out_err;
	}

	struct tep_event *ev = tep_find_event_by_name(tep, "kprobes", p->kprobe_name);
	p->event_id = ev->id;
	if (asprintf(&filename, "events/kprobes/%s/enable", p->kprobe_name) == -1)
		return ENOMEM;
	path = tracefs_get_tracing_file(filename);
	if (!path) {
		free(filename);
		err = ENOMEM;
		goto out_err;
	}
	free(filename);
	f = fopen(path, "w");
	if (!f) {
		err = errno;
		tracefs_put_tracing_file(path);
		goto out_err;
	}
	tracefs_put_tracing_file(path);
	if (fputc('1', f) == EOF) {
		fclose(f);
		err = EINVAL; // ?
		goto out_err;
	}
	if (fclose(f) == EOF) {
		err = errno;
		goto out_err;
	}
	return 0;

out_err:
	clear_kprobe(NULL, p->kprobe_name);
	return err;
}

static void clear_kprobes(void) {
	char *path = tracefs_get_tracing_file("kprobe_events");
	if (!path)
		return;
	FILE *events = fopen(path, "a+");
	tracefs_put_tracing_file(path);
	if (!events) {
		return;
	}
	for (int i = 0; i < num_kprobes; i++) {
		clear_kprobe(events, kprobes[i]);
	}
	fclose(events);
	free(kprobes);
	num_kprobes = 0;
	kprobes = NULL;
}

struct race_data {
	struct race_status {
		int open;
		unsigned long long pid;
	} *statuses;
	int count;
	int triggers;
};

struct tracer {
	int num_targets;
	struct race_data race;
	struct race_event *current_events;
	int num_race_points;
	struct race_point *race_points;
	cpu_set_t cpus;
	void *pages;
	int *finished;
	struct kbuffer **kbufs;
	struct tep_handle *event_parser;
	struct tep_format_field *common_type;
	struct tep_format_field *common_pid;
};

static int register_kprobes(struct tracer *tr) {
	int err = set_tracer("nop");
	if (err)
		return err;
	char *path = tracefs_get_tracing_file("kprobe_events");
	if (!path) {
		fprintf(stderr, "can't get \"kprobe_events\" ftrace file\n");
		return -1;
	}
	FILE *events = fopen(path, "w");
	if (!events) {
		err = errno;
		fprintf(stderr, "opening %s: %m\n", path);
		tracefs_put_tracing_file(path);
		return err;
	}
	tracefs_put_tracing_file(path);

	for (int i = 0; i < tr->num_race_points; i++) {
		struct race_point *p = &tr->race_points[i];
		err = add_kprobe(events, p, tr->event_parser, i);
		if (err)
			goto out_err;
	}

	fclose(events);
	return 0;
out_err:
	clear_kprobes();
	fclose(events);
	return err;
}

static int initialize_parser(struct tracer *tr) {
	FILE *file;
	char *path;
	char *buf;
	int size;
	int err = -1;

	path = tracefs_get_tracing_file("events/header_page");
	if (!path) {
		fprintf(stderr, "can't get events/header_page path\n");
		return -1;
	}
	file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "can't open %s: %m\n", path);
		goto put_path;
	}
	size = read_file(file, &buf);
	if (size < 0) {
		fprintf(stderr, "reading from %s: %s\n", path, strerror(-size));
		goto close_file;
	}
	err = tep_parse_header_page(tr->event_parser, buf, size, sizeof(long));
	if (err) {
		char err_msg[200];
		tep_strerror(tr->event_parser, err, err_msg, sizeof(err_msg));
		fprintf(stderr, "error parsing info at %s: %s\n", path, err_msg);
	}
	free(buf);
close_file:
	fclose(file);
put_path:
	tracefs_put_tracing_file(path);
	return err;
}

int tracer_add_pid(struct tracer *tr, pid_t pid) {
	tr->num_targets++;
	struct race_status *s = realloc(tr->race.statuses,
					sizeof(struct race_status) * tr->num_targets);
	if (!s)
		return ENOMEM;
	memset(s+tr->num_targets-1, 0, sizeof(*s));

	s[tr->num_targets-1].pid = pid;
	tr->race.statuses = s;
	return 0;
}

static int add_comms(struct tracer *tr, int num_comms, const char **comms) {
	if (num_comms < 1)
		return 0;

	DIR *dir = opendir("/proc/");
	struct dirent *d;
	if (!dir) {
		fprintf(stderr, "can't open /proc directory: %m\n");
		return -1;
	}

	char comm[100];
	comm[0] = '\0';
	int needed = num_comms;
	int *comm_found = malloc(sizeof(int) * needed);
	if (!comm_found) {
		closedir(dir);
		fprintf(stderr, "%s: OOM\n", __func__);
		return ENOMEM;
	}
	memset(comm_found, 0, sizeof(int) * needed);

	while ((d = readdir(dir))) {
		FILE *file;
		char path[300];
		char *end;
		pid_t pid = strtol(d->d_name, &end, 10);
		if (*end)
			continue;

		sprintf(path, "/proc/%s/comm", d->d_name);
		file = fopen(path, "r");
		if (!file) {
			fprintf(stderr, "can't open %s\n", path);
			continue;
		}
		fgets(comm, 100, file);
		fclose(file);
		int n = strlen(comm)-1;
		while (n >= 0 && comm[n] == '\n')
			comm[n--] = '\0';
		for (int i = 0; i < num_comms; i++) {
			if (strcmp(comms[i], comm))
				continue;

			if (comm_found[i])
				break;

			comm_found[i] = 1;
			tracer_add_pid(tr, pid);
			needed--;
			break;
		}
		if (!needed)
			break;
	}
	closedir(dir);
	for (int i = 0; i < num_comms; i++) {
		int unique = 1;
		for (int j = i-1; j >= 0; j--) {
			if (!strcmp(comms[i], comms[j])) {
				unique = 0;
				break;
			}
		}
		if (unique && !comm_found[i]) {
			fprintf(stderr, "can't find process %s\n", comms[i]);
			free(comm_found);
			return ENOENT;
		}
	}
	free(comm_found);
	return 0;
}

static int *trace_fds;
static int num_cpus;


static void clear_buffers(void) {
	char *buf = malloc(page_size);
	if (!buf) {
		fprintf(stderr, "%s: OOM\n", __func__);
		return;
	}
	for (int i = 0; i < num_cpus; i++) {
		int n;
		do {
			n = read(trace_fds[i], buf, page_size);
		} while (n > 0);
	}
	free(buf);
}

static struct sigaction sigint_old;

void sigint_handler(int sig) {
	// TODO: this handler can race in a bunch of places, should fix
	disable_tracing();
	clear_kprobes();

	if (trace_fds) {
		clear_buffers();
		for (int i = 0; i < num_cpus; i++)
			if (trace_fds[i] > 0)
				close(trace_fds[i]);
	}
	enable_tracing();
	exit(0);
}

void free_percpu(struct tracer *tr) {
	for (int i = 0; i < num_cpus; i++) {
		kbuffer_free(tr->kbufs[i]);
	}
	free(tr->kbufs);
	free(tr->pages);
	free(tr->finished);
}

int ftrace_exit(void) {
	if (trace_fds) {
		for (int i = 0; i < num_cpus; i++)
			if (trace_fds[i] > 0)
				close(trace_fds[i]);
	}
	int *tmp = trace_fds;
	trace_fds = NULL; // try not to race with signal handler
	free(tmp);
	int err = set_tracer("nop");
	if (err)
		return err;
	clear_kprobes();
	enable_tracing();
	if (fclose(tracing_on) == EOF) {
		err = errno;
		fprintf(stderr, "writing to tracing_on: %m\n");
	}
	sigaction(SIGINT, &sigint_old, NULL);
	return err;
}

static int open_trace_fds(cpu_set_t *cpus) {
	int err = 0;
	DIR *dir;
	struct dirent *d;
	char *path = tracefs_get_tracing_file("per_cpu");
	if (!path) {
		fprintf(stderr, "%s: can't get per_cpu ftrace file\n", __func__);
		return -1;
	}
	dir = opendir(path);
	if (!dir) {
		err = errno;
		fprintf(stderr, "error opening per_cpu: %m\n");
		return err;
	}

	int i = 0;
	while ((d = readdir(dir))) {
		char *filename;
		int cpu;

		if (sscanf(d->d_name, "cpu%d", &cpu) < 1)
			continue;
		if (asprintf(&filename, "%s/%s/trace_pipe_raw", path, d->d_name) == -1)
		{
			err = ENOMEM;
			break;
		}

		int fd = open(filename, O_RDONLY | O_NONBLOCK);
		free(filename);
		if (fd < 0) {
			fprintf(stderr, "error opening %s: %m\n", filename);
			continue;
		}
                /* is this correct? here there is an assumption that the values of
                 * online cpus you get from the cpu_set_t returned by pthread_getaffinity_np
                 * are the same as the values in the kernel internal struct cpuset. need
                 * to verify that */
		if (CPU_ISSET(cpu, cpus)) {
			trace_fds[i++] = fd;
		} else {
			close(fd);
		}
	}

	closedir(dir);
	tracefs_put_tracing_file(path);
	return err;
}

static int alloc_percpu(struct tracer *tr) {
	int err = 0;

	tr->pages = malloc(page_size * num_cpus);
	if (!tr->pages)
		return ENOMEM;
	tr->finished = malloc(sizeof(int) * num_cpus);
	if (!tr->finished) {
		err = ENOMEM;
		goto free_pages;
	}
	tr->kbufs = malloc(sizeof(*tr->kbufs) * num_cpus);
	if (!tr->kbufs) {
		err = ENOMEM;
		goto free_finished;
	}
	memset(tr->finished, 0, sizeof(int) * num_cpus);
	memset(tr->pages, 0, page_size * num_cpus);
	memset(tr->kbufs, 0, sizeof(*tr->kbufs) * num_cpus);

	enum kbuffer_endian end = tep_is_local_bigendian(tr->event_parser) ?
		KBUFFER_ENDIAN_BIG : KBUFFER_ENDIAN_LITTLE;
	enum kbuffer_long_size sz = sizeof(long) == 8 ?
		KBUFFER_LSIZE_8 : KBUFFER_LSIZE_4;
	for (int i = 0; i < num_cpus; i++) {
		tr->kbufs[i] = kbuffer_alloc(sz, end);
		if (!tr->kbufs[i]) {
			err = ENOMEM;
			goto free_kbufs;
		}
	}
	return 0;

free_kbufs:
	for (int i = 0; i < num_cpus; i++)
		if (tr->kbufs[i])
			free(tr->kbufs[i]);
	free(tr->kbufs);
free_finished:
	free(tr->finished);
free_pages:
	free(tr->pages);
	return err;
}

static int copy_race_points(struct tracer *tr,
			    struct k_race_config *config) {
	tr->num_race_points = config->num_race_points;
	tr->race_points = malloc(sizeof(*tr->race_points) *
				 tr->num_race_points);
	if (!tr->race_points) {
		fprintf(stderr, "%s: OOM\n", __func__);
		return ENOMEM;
	}

	for (int i = 0; i < tr->num_race_points; i++) {
		struct k_race_point *kp = &config->race_points[i];
		struct race_point *p = &tr->race_points[i];

		p->opens = kp->opens;
		p->triggers = kp->triggers;
		p->closes = kp->closes;

		sprintf(p->kprobe_name, "k_race_%d", i);

		int len = strlen(kp->description);
		if (len > KPROBE_LENGTH - 15) {
			fprintf(stderr, "%s too long\n", kp->description);
			free(tr->race_points);
			return ENAMETOOLONG;
		}

		if (len > 4 && !strncmp(kp->description+len-4, ":ret", 4)) {
			p->kprobe_type = 'r';
			memcpy(p->kprobe, kp->description, len - 4);
			p->kprobe[len - 4] = 0;
		} else {
			p->kprobe_type = 'p';
			strcpy(p->kprobe, kp->description);
		}
	}
	return 0;
}

struct tracer *alloc_tracer(struct k_race_config *config) {
	page_size = getpagesize();

	struct tracer *ret = malloc(sizeof(*ret));
	if (!ret)
		return NULL;
	memset(ret, 0, sizeof(*ret));

	ret->event_parser = tep_alloc();
	if (!ret->event_parser)
		goto free_tr;
	int err = initialize_parser(ret);
	if (err)
		goto free_tep;

	for (int i = 0; i < config->num_funcs; i++) {
		struct k_race_sched_config *cfg = &config->sched_config[i];

		int n = 0;
		for (int j = 0; n < CPU_COUNT(&cfg->cpus); j++) {
			if (CPU_ISSET(j, &cfg->cpus)) {
				CPU_SET(j, &ret->cpus);
				n++;
			}
		}
	}
	num_cpus = CPU_COUNT(&ret->cpus);

	err = alloc_percpu(ret);
	if (err)
		goto free_tep;

	ret->current_events = malloc(sizeof(struct race_event) * num_cpus);
	if (!ret->current_events)
		goto free_pcpu;
	memset(ret->current_events, 0, sizeof(struct race_event) * num_cpus);

	err = copy_race_points(ret, config);
	if (err)
		goto free_events;

	err = add_comms(ret, config->num_comms, config->comms);
	if (err)
		goto free_statuses;

	return ret;

free_statuses:
	if (ret->race.statuses)
		free(ret->race.statuses);
	free(ret->race_points);
free_events:
	free(ret->current_events);
free_pcpu:
	free_percpu(ret);
free_tep:
	tep_free(ret->event_parser);
free_tr:
	free(ret);
	return NULL;

}

void free_tracer(struct tracer *tr) {
	tep_free(tr->event_parser);
	free_percpu(tr);
	free(tr->race.statuses);
	free(tr->race_points);
	free(tr->current_events);
	free(tr);
}

int ftrace_init(struct tracer *tr) {
	int err;
	char *path = tracefs_get_tracing_file("tracing_on");
	if (!path)
		return ENOMEM;
	tracing_on = fopen(path, "w");
	if (!tracing_on) {
		err = errno;
		tracefs_put_tracing_file(path);
		return err;
	}
	tracefs_put_tracing_file(path);
	disable_tracing();

	struct sigaction sa = { .sa_handler = sigint_handler };
	if (sigaction(SIGINT, &sa, &sigint_old) == -1) {
		err = errno;
		goto close_tracing_on;
	}

	err = register_kprobes(tr);
	if (err)
		goto restore_sighand;

	struct tep_event *ev = tep_get_first_event(tr->event_parser);
	tr->common_type = tep_find_common_field(ev, "common_type");
	tr->common_pid = tep_find_common_field(ev, "common_pid");

	trace_fds = malloc(sizeof(int) * num_cpus);
	if (!trace_fds) {
		err = ENOMEM;
		goto reset_ftrace;
	}
	err = open_trace_fds(&tr->cpus);
	if (err)
		goto close_fds;
	return 0;

close_fds:
	for (int i = 0; i < num_cpus; i++)
		if (trace_fds[i] > 0)
			close(trace_fds[i]);
	free(trace_fds);
reset_ftrace:
	clear_kprobes();
restore_sighand:
	sigaction(SIGINT, &sigint_old, NULL);
close_tracing_on:
	fclose(tracing_on);
	return err;
}

int enable_tracing(void) {
	if (fputc('1', tracing_on) == EOF ||
	    fflush(tracing_on) == EOF) {
		fprintf(stderr, "%s: write to tracing_on %m\n", __func__);
		return -1;
	}
	return 0;
}

int disable_tracing(void) {
	if (fputc('0', tracing_on) == EOF ||
	    fflush(tracing_on) == EOF) {
		fprintf(stderr, "%s: write to tracing_on %m\n", __func__);
		return -1;
	}
	return 0;
}

static inline void *cpu_page(struct tracer *tr, int idx) {
	return ((char *)tr->pages) + (idx * page_size);
}

static struct race_point *match_race_event(struct tracer *tr,
					   struct race_event *event,
					   struct kbuffer *kbuf,
					   void *ftrace_event) {
	unsigned long long event_id;
	tep_read_number_field(tr->common_type, ftrace_event, &event_id);
	tep_read_number_field(tr->common_pid, ftrace_event, &event->pid);

	for (int i = 0; 1; i++) {
		if (event->pid == tr->race.statuses[i].pid)
			break;

		if (i == tr->num_targets - 1)
			return NULL;
	}

	for (int i = 0; i < tr->num_race_points; i++) {
		struct race_point *p = &tr->race_points[i];
		if (p->event_id == event_id) {
			event->time = kbuffer_timestamp(kbuf);
			return p;
		}
	}
	return NULL;
}

static void *read_event(struct tracer *tr, int cpu, int *missed_events) {
	struct kbuffer *kbuf = tr->kbufs[cpu];
	int n = read(trace_fds[cpu], cpu_page(tr, cpu), page_size);
	if (n <= 0)
		return NULL;
	kbuffer_load_subbuffer(kbuf, cpu_page(tr, cpu));
	*missed_events = kbuffer_missed_events(kbuf);
	return kbuffer_read_event(kbuf, NULL);
}

static inline void next_event(struct kbuffer *kbuf, int *entries) {
	kbuffer_next_event(kbuf, NULL);
	(*entries)++;
}

static struct race_event *current_event(struct tracer *tr, int cpu, int *entries, int *missed_events) {
	struct race_event *re = &tr->current_events[cpu];
	if (re->point)
		return re;

	while (1) {
		void *event = kbuffer_read_event(tr->kbufs[cpu], NULL);
		if (!event)
			event = read_event(tr, cpu, missed_events);
		if (!event)
			return NULL;

		struct race_point *p = match_race_event(tr, re, tr->kbufs[cpu], event);
		if (p) {
			re->point = p;
			return re;
		}
		next_event(tr->kbufs[cpu], entries);
	}
}

static void mark_race_effects(struct tracer *tr, int cpu) {
	struct race_data *race = &tr->race;
	unsigned long long pid = tr->current_events[cpu].pid;
	struct race_point *point = tr->current_events[cpu].point;

	for (int i = 0; i < tr->num_targets; i++) {
		if (race->statuses[i].pid != pid && point->triggers &&
		    race->statuses[i].open)
			race->triggers++;
		if (race->statuses[i].pid != pid)
			continue;
		if (point->opens && !race->statuses[i].open) {
			race->statuses[i].open = 1;
			continue;
		}
		if (point->closes && race->statuses[i].open) {
			race->count++;
			race->statuses[i].open = 0;
		}
	}
}

static void consume_event(struct tracer *tr, int cpu, int *entries) {
	tr->current_events[cpu].point = NULL;
	next_event(tr->kbufs[cpu], entries);
}

int tracer_collect_stats(struct tracer *tr, int *entries,
			 int *count, int *triggers) {
	int missed_events = 0;
	*entries = 0;
	memset(tr->finished, 0, sizeof(int) * num_cpus);
	tr->race.count = 0;
	tr->race.triggers = 0;

	while (1) {
		unsigned long long earliest = ~0ULL;
		int cpu = -1;
		for (int i = 0; i < num_cpus; i++) {
			struct race_event *re;
			int missed = 0;
			if (tr->finished[i])
				continue;
			re = current_event(tr, i, entries, &missed);
			if (missed) {
				missed_events = 1;
			}
			if (!re) {
				tr->finished[i] = 1;
				continue;
			}
			if (re->time < earliest) {
				earliest = re->time;
				cpu = i;
			}
		}
		if (cpu == -1) {
			*count = tr->race.count;
			*triggers = tr->race.triggers;
			return missed_events;
		}
		mark_race_effects(tr, cpu);
		consume_event(tr, cpu, entries);
	}
}

int ftrace_overrun(unsigned int *dst) {
	int err = -1;
	DIR *dir;
	struct dirent *d;
	char *path = tracefs_get_tracing_file("per_cpu");
	if (!path) {
		fprintf(stderr, "%s can't get per_cpu dir\n", __func__);
		return -1;
	}
	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "%s can't open %s\n", __func__, path);
		goto out_put_path;
	}

	unsigned int overrun = 0;
	while ((d = readdir(dir))) {
		if (strncmp("cpu", d->d_name, 3))
			continue;
		char *stats;
		if (asprintf(&stats, "%s/%s/stats", path, d->d_name) == -1) {
			fprintf(stderr, "%s asprintf error\n", __func__);
			goto out_closedir;
		}
		FILE *file = fopen(stats, "r");
		if (!file) {
			fprintf(stderr, "%s: opening %s: %m\n", __func__, stats);
			free(stats);
			goto out_closedir;
		}
		free(stats);

		while (1) {
			unsigned int val;
			char field[100];
			if (fscanf(file, "%[^:]: %u ", field, &val) <= 0)
				break;
			if (!strcmp(field, "overrun"))
				overrun += val;
		}
		fclose(file);
	}
	err = 0;
	*dst = overrun;
out_closedir:
	closedir(dir);
out_put_path:
	tracefs_put_tracing_file(path);
	return err;
}
