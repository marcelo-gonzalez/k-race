#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "k-race.h"

// here a filesystem with no journal is mounted at /mnt

int pre(void *user) {
	int *fd = user;

	int err = mkdir("/mnt/dir1", 0700);
	if (err && errno != EEXIST) {
		perror("mkdir(dir1)");
		return -1;
	}
	*fd = open("/mnt/dir1/file", O_RDWR|O_CREAT|O_SYNC);
	if (*fd == -1) {
		perror("open");
		return -1;
	}
	return 0;
}

int post(void *user) {
	return close(*(int *)user);
}

struct k_race_callbacks callbacks = {
	.pre = pre,
	.post = post,
};

int do_write(void *user, void *arg) {
	int fd = *(int *)user;

	write(fd, "X", 1);
	return 0;
}

int do_rename(void *user, void *arg) {
	rename("/mnt/dir1/file", "/mnt/dir2/file");
	rmdir("/mnt/dir1");
	return 0;
}

struct k_race_target targets[] = {
	{
		.func = do_rename,
	},
	{
		.func = do_write,
	},
};

int main(int argc, char **argv) {
	struct k_race_options opts;
	if (k_race_parse_options(&opts, argc, argv))
		return 1;

	int err = mkdir("/mnt/dir2", 0700);
	if (err && errno != EEXIST) {
		perror("mkdir(dir2)");
		return 1;
	}
	
	int fd;
	return k_race_loop(&opts, 2, targets,
			   &callbacks, &fd) != 0;
}
