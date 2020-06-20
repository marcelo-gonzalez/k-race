dir = $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

EXAMPLES = $(patsubst %, %test, $(dir $(wildcard examples/*/target.c)))

CFLAGS = -Wall -fpic -I/usr/include/json-c $(EXTRA_CFLAGS)
# TODO: use CMake or something, because this is not correct on every computer
CFLAGS += -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include  -I/usr/lib/arm-linux-gnueabihf/glib-2.0/include
LDLIBS = -ltracefs -ltraceevent -ldl -ljson-c -lglib-2.0
LDLIBS += -lgsl -lgslcblas -lm

obj = config.o main.o trace.o stats.o

.PHONY: clean examples install

libk-race.so: $(obj)
	$(CC) -shared -o libk-race.so $(obj) $(LDLIBS)

prefix ?= /usr/local

install:
	if [ ! -d $(prefix)/include/k-race ]; then \
		install -m 0755 -d $(prefix)/include/k-race; \
	fi; \
	if [ ! -d $(prefix)/lib ]; then \
		install -m 0755 -d $(prefix)/lib; \
	fi; \
	install -m 0755 libk-race.so $(prefix)/lib; \
	install -m 0644 k-race.h $(prefix)/include/k-race;

.SECONDEXPANSION:
$(EXAMPLES): $$(dir $$@)target.c
	$(CC) -I $(dir) -L $(dir) -Wall $(EXTRA_CFLAGS) -o $@ $(dir $@)target.c -lk-race

examples: $(EXAMPLES)

config.o: config.h
trace.o: config.h trace.h
main.o: config.h k-race.h trace.h
stats.o: stats.h

clean:
	rm *.o libk-race.so examples/*/test
