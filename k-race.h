#ifndef K_RACE_H
#define K_RACE_H

struct k_race_target {
	int (*func)(void *user, void *arg);
	void *arg;
};

struct k_race_callbacks {
	// If not NULL, called before each round.
	// return nonzero on error to abort.
	int (*pre)(void *user);
	// If not NULL, called after each round.
	// return nonzero on error to abort.
	int (*post)(void *user);
};

struct k_race_options {
	// Don't add kprobes or do any kind of tracing. Also means
	// we can't be smart at all about what offsets between functions
	// to try.
	int notrace;
	const char *config_file;
	// must be between 0 and 1, and controls the percentage of the
	// time we try parameters that have been good so far vs random
	// parameters.  see "Epsilon-greedy" here
	// https://en.wikipedia.org/wiki/Multi-armed_bandit#Approximate_solutions. Note
	// that the precision in estimating what parameters work best
	// is exponentially bad in num_targets.
	float explore_probability;
};

int k_race_parse_options(struct k_race_options *opts,
			 int argc, char **argv);

int k_race_loop(struct k_race_options *opts,
		int num_targets, struct k_race_target *targets,
		struct k_race_callbacks *callbacks, void *user);

#endif
