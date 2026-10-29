CC      ?= cc
CFLAGS  ?= -Os -pipe
# PID 1 runs before /usr may be mounted, so link statically by default.
# Build dynamically with: make LDFLAGS=
LDFLAGS ?= -static
WARN     = -std=c17 -Wall -Wextra -Wshadow
HARDEN   = -D_FORTIFY_SOURCE=2 -fstack-protector-strong

SRC = src/vinit.c src/service.c src/config.c src/control.c

all: vinit vinitctl

vinit: $(SRC) src/vinit.h
	$(CC) $(WARN) $(HARDEN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $(SRC)

vinitctl: cmd/vinitctl.c
	$(CC) $(WARN) $(HARDEN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ cmd/vinitctl.c

tests/svc-test: tests/svc-test.c
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ tests/svc-test.c

tests/config-test: tests/config-test.c src/config.c src/vinit.h
	$(CC) $(WARN) -g -fsanitize=address,undefined -o $@ tests/config-test.c src/config.c

tests/service-test: tests/service-test.c src/service.c src/config.c src/vinit.h
	$(CC) $(WARN) -g -fsanitize=address,undefined -o $@ tests/service-test.c src/service.c src/config.c

tests/event-test: tests/event-test.c src/vinit.c src/vinit.h
	$(CC) $(WARN) -g -fsanitize=address,undefined -o $@ tests/event-test.c

tests/client-test: tests/client-test.c cmd/vinitctl.c
	$(CC) $(WARN) -g -fsanitize=address,undefined -o $@ tests/client-test.c

rootfs: vinit vinitctl tests/svc-test
	scripts/mkrootfs.sh build/rootfs

initramfs: rootfs
	scripts/mkinitramfs.sh build/rootfs build/initramfs.cpio.gz

test-unit: tests/config-test tests/service-test tests/event-test tests/client-test
	tests/config-test
	tests/service-test
	tests/event-test
	tests/client-test

test: test-unit rootfs
	tests/run-ns.sh build/rootfs

test-qemu: initramfs
	tests/run-qemu.sh build/initramfs.cpio.gz

test-qemu-arm64:
	sh tests/run-arm64.sh

# Coverage-guided fuzzing of the parser and resolver; needs clang.
FUZZ_TIME ?= 60
fuzz: tests/fuzz-config.c src/config.c src/vinit.h
	clang -std=c17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
		-o tests/fuzz-config tests/fuzz-config.c src/config.c
	mkdir -p build/fuzz-corpus
	cp tests/services/*.conf build/fuzz-corpus/
	tests/fuzz-config -max_total_time=$(FUZZ_TIME) -max_len=4096 build/fuzz-corpus

clean:
	rm -rf vinit vinitctl tests/svc-test tests/config-test tests/service-test tests/event-test tests/client-test tests/fuzz-config build

.PHONY: all rootfs initramfs test-unit test test-qemu test-qemu-arm64 fuzz clean
