#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "vchiq-ioctl.h"
#include <k-race/k-race.h>

#define BUF_SIZE 200 * (1 << 10)

#define NUM_INSTANCES 200

struct worker_arg {
	char *buf;
	int fd;
};

int num_targets = 2;

static int pre(void *user) {
	struct worker_arg *args = user;
	for (int i = 0; i < num_targets; i++) {
		struct worker_arg *arg = &args[i];
		arg->fd = open("/dev/vchiq", 0);
		if (arg->fd < 0) {
			fprintf(stderr, "can't open /dev/vchiq: %m\n");
			return 1;
		}
	}
	return 0;
}

static int num_instances(const char *vchiq_output) {
	int ret = 0;
	while (1) {
		vchiq_output = strstr(vchiq_output, "completions");
		if (!vchiq_output)
			break;
		vchiq_output++;
		ret++;
	}
	return ret;
}

static int post(void *user) {
	struct worker_arg *args = user;
	for (int i = 0; i < num_targets; i++) {
		struct worker_arg *wa = &args[i];
		int found = num_instances(wa->buf);
		if (found != NUM_INSTANCES) {
			printf("BUG!! instances: %d\n", found);
			return 1;
		}
		close(wa->fd);
	}
	return 0;
}

struct k_race_callbacks callbacks = {
	.pre = pre,
	.post = post,
};

static int vchiq_read(void *ctx, void *arg) {
	struct worker_arg *wa = arg;
	return read(wa->fd, wa->buf, BUF_SIZE) <= 0;
}

struct k_race_target targets[] = {
	{
		.func = vchiq_read,
	},
	{
		.func = vchiq_read,
	},
};

int main(int argc, char **argv) {
	for (int i = 0; i < NUM_INSTANCES; i++) {
		int fd = open("/dev/vchiq", O_RDONLY);
		if (fd == -1) {
			perror("open");
			return 1;
		}
		struct vchiq_create_service s = { .params.fourcc = i };
		int err = ioctl(fd, VCHIQ_IOC_CREATE_SERVICE, &s);
		if (err) {
			perror("ioctl");
			return 1;
		}
	}

	struct worker_arg *args = malloc(sizeof(*args) * 2);
	if (!args)
		return 1;
	for (int i = 0; i < num_targets; i++) {
		struct worker_arg *arg = &args[i];
		arg->buf = malloc(BUF_SIZE);
		if (!arg->buf)
			return 1;
		targets[i].arg = &args[i];
	}

	struct k_race_options opts;
	if (k_race_parse_options(&opts, argc, argv))
		return 1;

	return k_race_loop(&opts, num_targets, targets,
			   &callbacks, args) != 0;
}
