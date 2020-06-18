// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2020 Marcelo Diop-Gonzalez

#define _GNU_SOURCE
#include <errno.h>
#include <json.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

enum race_effect {
	RACE_OPEN,
	RACE_TRIGGER,
	RACE_CLOSE,
};

static void update_point(struct k_race_config *cfg,
			 const char *desc, enum race_effect effect) {
	struct k_race_point *point = &cfg->race_points[cfg->num_race_points];
	point->description = desc;
	cfg->num_race_points++;
	for (int i = 0; i < cfg->num_race_points-1; i++) {
		struct k_race_point *p = &cfg->race_points[i];
		if (!strcmp(point->description, p->description)) { // TODO: +0x5 vs +5
			point = &cfg->race_points[i];
			cfg->num_race_points--;
			break;
		}
	}

	switch (effect) {
	case RACE_OPEN:
		point->opens = 1;
		break;
	case RACE_TRIGGER:
		point->triggers = 1;
		break;
	case RACE_CLOSE:
		point->closes = 1;
		break;
	}
}

static int get_string_array(json_object *config, const char *key,
			    int *n, const char ***dst) {
	json_object *arr;

	json_object_object_get_ex(config, key, &arr);
	if (!arr) {
		*n = 0;
		return 0;
	}
	if (json_object_is_type(arr, json_type_string)) {
		*n = 1;
		*dst = malloc(sizeof(**dst));
		if (!*dst)
			return ENOMEM;
		**dst = json_object_get_string(arr);
		return 0;
	}
	if (!json_object_is_type(arr, json_type_array)) {
		fprintf(stderr, "config field \"%s\" should be a string or an array of strings\n", key);
		return EINVAL;
	}

	*n = json_object_array_length(arr);
	if (*n < 1)
		return 0;

	*dst = malloc(sizeof(**dst) * (*n));
	if (!*dst)
		return ENOMEM;
	for (int i = 0; i < *n; i++) {
		struct json_object *elem = json_object_array_get_idx(arr, i);
		if (!json_object_is_type(elem, json_type_string)) {
			fprintf(stderr, "config field \"%s\" should be a string or an array of strings\n", key);
			free(*dst);
			return EINVAL;
		}
		(*dst)[i] = json_object_get_string(elem);
	}
	return 0;
}

static int add_race_points(struct k_race_config *cfg,
			   const char *key,
			   enum race_effect effect) {
	const char **descriptions;
	int n;
	int err = get_string_array(cfg->json_config, key, &n, &descriptions);
	if (err)
		return err;
	if (n < 1) {
		fprintf(stderr, "please specify at least one symbol in %s\n", key);
		return EINVAL;
	}
	struct k_race_point *p = realloc(cfg->race_points,
					 sizeof(struct k_race_point) *
					 (cfg->num_race_points + n));
	if (!p) {
		free(descriptions);
		return ENOMEM;
	}
	memset(p + cfg->num_race_points, 0,
	       sizeof(*p) * n);
	cfg->race_points = p;
	for (int i = 0; i < n; i++) {
		update_point(cfg, descriptions[i], effect);
	}
	free(descriptions);
	return 0;
}

static int parse_race_config(struct k_race_config *cfg) {
	int err;

	err = add_race_points(cfg, "opened_by", RACE_OPEN);
	if (err)
		goto out_free;
	err = add_race_points(cfg, "triggered_by", RACE_TRIGGER);
	if (err)
		goto out_free;
	err = add_race_points(cfg, "closed_by", RACE_CLOSE);
	if (err)
		goto out_free;
	return 0;

out_free:
	if (cfg->race_points)
		free(cfg->race_points);
	return err;
}

static int parse_sched_policy(json_object *sched_config,
			      struct k_race_sched_config *cfg) {
	json_object *policy_config;
	json_object_object_get_ex(sched_config, "policy", &policy_config);
	if (!policy_config) {
		cfg->sched_policy = SCHED_OTHER;
		cfg->sched_param.sched_priority = 0;
		return 0;
	}
	if (json_object_get_type(policy_config) == json_type_int) {
		cfg->sched_policy = json_object_get_int(policy_config);
		if (cfg->sched_policy == SCHED_OTHER)
			cfg->sched_param.sched_priority = 0;
		else
			cfg->sched_param.sched_priority = 1;
		return 0;
	}
	if (json_object_get_type(policy_config) == json_type_string) {
		const char *str = json_object_get_string(policy_config);
		if (!strcmp(str, "SCHED_OTHER")) {
			cfg->sched_policy = SCHED_OTHER;
			cfg->sched_param.sched_priority = 0;
			return 0;
		} else if (!strcmp(str, "SCHED_FIFO")) {
			cfg->sched_policy = SCHED_FIFO;
			cfg->sched_param.sched_priority = 1;
			return 0;
		} else if (!strcmp(str, "SCHED_RR")) {
			cfg->sched_policy = SCHED_RR;
			cfg->sched_param.sched_priority = 1;
			return 0;
		} else {
			fprintf(stderr, "sched policy \"%s\" unrecognized\n", str);
			return EINVAL;
		}
	}
	fprintf(stderr, "sched policy config \"%s\" has bad type\n",
		json_object_get_string(policy_config));
	return EINVAL;
}

static int parse_cpus(json_object *sched_config, struct k_race_sched_config *cfg) {
	json_object *cpus;
	json_object_object_get_ex(sched_config, "cpus", &cpus);
	if (!cpus) {
		pthread_getaffinity_np(pthread_self(),
				       sizeof(cpu_set_t), &cfg->cpus);
		return 0;
	}
	if (json_object_get_type(cpus) != json_type_array) {
		fprintf(stderr, "\"cpus\" field should be an array of ints\n");
		return EINVAL;
	}
	int n = json_object_array_length(cpus);
	if (n == 0) {
		pthread_getaffinity_np(pthread_self(),
				       sizeof(cpu_set_t), &cfg->cpus);
		return 0;
	}

	for (int i = 0; i < n; i++) {
		json_object *jcpu = json_object_array_get_idx(cpus, i);
		if (json_object_get_type(jcpu) != json_type_int) {
			fprintf(stderr, "\"cpus\" field should be an array of ints\n");
			return EINVAL;
		}
		int cpu = json_object_get_int(jcpu);
		if (cpu < 0 || cpu >= CPU_SETSIZE) {
			fprintf(stderr, "invalid cpu number: %d\n", cpu);
			return EINVAL;
		}
		CPU_SET(cpu, &cfg->cpus);
	}
	return 0;
}

struct k_race_config *k_race_config_parse(int num_funcs, const char *filename) {
	struct k_race_config *cfg = malloc(sizeof(*cfg));
	if (!cfg)
		return NULL;
	memset(cfg, 0, sizeof(*cfg));
	cfg->num_funcs = num_funcs;

	json_object *config = json_object_from_file(filename);
	if (!config)
		// TODO: old json-c versions print out the error, and new
		// versions have the function json_util_get_last_err() that
		// needs to get called here. figure out what to do about that.
		goto out_free_cfg;

	cfg->json_config = config;

	cfg->sched_config = malloc(num_funcs * sizeof(*cfg->sched_config));
	memset(cfg->sched_config, 0, num_funcs * sizeof(*cfg->sched_config));

	int sched_config_length = 0;
	json_object *sched_config;
	json_object_object_get_ex(config, "sched", &sched_config);
	if (sched_config && json_object_get_type(sched_config) !=
	    json_type_array) {
		fprintf(stderr, "\"sched\" config element must refer"
			" to an array, got:\n%s\n",
			json_object_get_string(sched_config));
		goto out_free_sched;
	} else if (sched_config)
		sched_config_length = json_object_array_length(sched_config);

	if (sched_config_length > num_funcs) {
		fprintf(stderr, "\"sched\" config element has more"
			" elements than functions given. Truncating\n");
	}

	for (int i = 0; i < num_funcs; i++) {
		struct k_race_sched_config *c = &cfg->sched_config[i];
		json_object *sched = NULL;
		if (i < sched_config_length)
			sched = json_object_array_get_idx(sched_config, i);

		int err = parse_cpus(sched, c);
		if (err)
			goto out_free_sched;
		err = parse_sched_policy(sched, c);
		if (err)
			goto out_free_sched;
	}

	json_object *jname;
	json_object_object_get_ex(config, "name", &jname);
	if (jname && !json_object_is_type(jname, json_type_string))
		fprintf(stderr, "config \"name\" field not a string\n");
	if (!jname || !json_object_is_type(jname, json_type_string))
		cfg->name = "race";
	else
		cfg->name = json_object_get_string(jname);

	int err = get_string_array(cfg->json_config, "comms",
				   &cfg->num_comms, &cfg->comms);
	if (err)
		goto out_free_sched;

	err = parse_race_config(cfg);
	if (err)
		goto out_free_race;
	return cfg;

out_free_race:
	if (cfg->race_points)
		free(cfg->race_points);
	free(cfg->comms);
out_free_sched:
	free(cfg->sched_config);
	json_object_put(cfg->json_config);
out_free_cfg:
	free(cfg);
	return NULL;
}

void k_race_config_free(struct k_race_config *config) {
	json_object_put(config->json_config);
	if (config->comms)
		free(config->comms);
	free(config->race_points);
	free(config->sched_config);
	free(config);
}
