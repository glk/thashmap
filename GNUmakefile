CFLAGS:= -std=gnu99 -Wall -Wno-unused -g -I.

TARGETS:= thashmap-bench thashmap-test

all: ${TARGETS}

.PHONY: clean
clean:
	rm -f ${TARGETS}

thashmap-bench thashmap-test: thashmap.c thashmap.h

thashmap-bench: CFLAGS:=${CFLAGS} -O2
thashmap-bench: bench/thashmap-bench.c

thashmap-test: CFLAGS:=${CFLAGS} -DTHASHMAP_DEBUG -O0
thashmap-test: test/thashmap-test.c

thashmap-bench thashmap-test:
	${CC} ${CFLAGS} -o $@ $(filter %.c,$^)
