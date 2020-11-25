# A kernel race condition reproducer

This is a library that tries to get a user-specified kernel race
condition to happen by running user-defined functions in different
threads with many different timings. To get a sense of how
it could be used, we can look at the code in `examples/ext4-sync`.

Linux kernel commit 08adf452e628b0e2ce (ext4: fix race between
ext4_sync_parent() and rename()) fixed a race condition in which it
was possible to get a null pointer dereference after an unfortunate
sequence of events involving a rename() and an rmdir(). In a pinch, we
might try to trigger this by having two threads loop calling the
involved functions, and a small reproducer is included in the above
commit's message. To do so and to wait for a couple minutes while
nothing happens can make you feel pretty blind to what's going on, and
to whether you're even getting close to triggering the thing at
all. So using the code in this here repository, we'd split the
reproducer out into functions that trigger the race, and let the
library try different timings and measure what's happened.

Before calling the functions that actually trigger the race, we set
some things up, making a new directory and opening a file:

```

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
```

Then the functions that will actually trigger the race:

```
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
```

Next we stare at `objdump` output to find the racy instructions,
and we come up with a JSON config file that looks like this:

```
{
    // ->d_parent read
    "opened_by": "ext4_sync_file+0x293",
    // d_move() call
    "triggered_by": "vfs_rename+0x382",
    // ->d_parent->d_inode read
    "closed_by": "ext4_sync_file+0x2a5",
}
```

In `main()`, we pass the above functions to `k_race_loop()`.  This
function places kprobes at the places indicated above and does the
following over and over in one thread for each of the two functions
defined above:

```
pthread_barrier_wait();
nanosleep(&some_amount);
f(user_pointer, user_arg);
```

The data is output to a file named `out.dat` by default, and `examine.py`
can be used to examine the output.

```console
hero@foo.bar:~/kernel-race$ sudo ./examples/ext4-race/test --config-file examples/ext4-race/config.json
^C
hero@foo.bar:~/kernel-race$ ./examine.py cat out.dat
    offset_0  counts  triggers
0    1342273     200  0.000000
1    1411458     200  0.000000
2    3306398     200  0.000000
3    1677705     200  0.000000
4    3099204     200  0.000000
5    2057901     200  0.000000
6    3459935     200  0.000000
7    3488149     200  0.000000
8    2029730     200  0.000000
..... (many lines of zeroes before something is found) .....
49   2162093     200  0.000000
50    313130     199  0.346734
51    313193     200  0.500000
52    313200     200  0.500000
53    313174     200  0.265000
54   1863066     200  0.000000
55    313201     200  0.020000
56    313196     200  0.030000
57    313185     200  0.045000
58    313119     200  0.020000
59    313121     200  0.020000
60    313166     200  0.040000
61    313165     200  0.100000
62    313157     200  0.060000
```

The offset indicates the difference in start times between the two
functions in nanoseconds, the `count` field indicates the number of
times `"opened_by"` followed by `"closed_by"` was found, and the
`triggers` field indicates the number of times `"triggered_by"`
occurred between the two, divided by `count` ("occurred between" is
kind of dubious since we're looking at timestamps from different CPUs, but close
enough for our purposes). Use `./examine.py plot out.dat` to view a plot.

In this example, the race is triggered much more quickly than with
simple loops, and we get to see how often we got close to triggering
it.

## Dependencies
```console
hero@foo.bar:~$ sudo apt-get install libgsl-dev libglib2.0-dev libjson-c-dev
```

In addition to these, we need libtraceevent and libtracefs, which can
be gotten from
[here](https://git.kernel.org/pub/scm/libs/libtrace/).

`examine.py` dependencies:
```console
hero@foo.bar:~$ pip3 install matplotlib pandas
```

## Known Problems
* The JSON config as it's currently defined doesn't allow very
expressive race definitions. The ext4 race described here is a good
example. The config defines the race to occur when `d_move()` is
called by `vfs_rename()`, but that's just a proxy for what we really
need to happen. If a more expressive config were implemented, we could
say that the race happens when `d_move()` switches `->d_parent` AND
`->d_parent->d_inode` is set to null by `rmdir()`, both happening between
`"opened_by"` and `"triggered_by"` given in the config above.

* Currently this only measures the number of times "triggered_by" in
  the config file happens between "opened_by" and "closed_by". It
  doesn't know how to move towards some set of parameters that almost
  worked. This means that as it's currently implemented, if the
  race definition you give in the config file is very tight, this will
  spend a long time totally blind before it starts to find what sleep
  times work best.