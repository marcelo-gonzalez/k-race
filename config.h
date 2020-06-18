#ifndef CONFIG_H
#define CONFIG_H

#include <json.h>
#include <sched.h>

struct k_race_point {
	const char *description;
	int opens;
	int triggers;
	int closes;
};

struct k_race_config {
	const char *name;
	int num_race_points;
	struct k_race_point *race_points;
	int num_funcs;
	struct k_race_sched_config {
		int sched_policy;
		struct sched_param sched_param;
		cpu_set_t cpus;
	} *sched_config;
	int num_comms;
	const char **comms;
	json_object *json_config;
};

struct k_race_config *k_race_config_parse(int num_funcs, const char *filename);
void k_race_config_free(struct k_race_config *config);

#endif
