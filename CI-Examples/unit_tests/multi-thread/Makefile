# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

CC = gcc
TLS_CC = musl-gcc
CFLAGS = -Wall -Wextra -O2 -rdynamic -g -std=c11 -pthread -D_FORTIFY_SOURCE=0
LDFLAGS = 

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
CFLAGS += -g
else
SAGUARO_LOG_LEVEL = error
CFLAGS += -O3
endif

.PHONY: all
all: thread-stress thread-rwlock thread-tls thread-stress.manifest thread-rwlock.manifest thread-tls.manifest

thread-stress: thread-stress.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
thread-stress.o: thread-stress.c
thread-stress.manifest: thread-stress.manifest.template
	gramine-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dmultiarch=$(shell $(CC) -dumpmachine) \
		$< $@
	gramine-manifest-check $@

thread-rwlock: thread-rwlock.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
thread-rwlock.o: thread-rwlock.c
thread-rwlock.manifest: thread-rwlock.manifest.template
	gramine-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dmultiarch=$(shell $(CC) -dumpmachine) \
		$< $@
	gramine-manifest-check $@

# A static executable must be position-independent on aarch64: the guest's memory starts at 0x40000000,
# where a non-PIE executable would be linked.
ifeq ($(shell uname -m),aarch64)
STATIC_FLAG = -static-pie
else
STATIC_FLAG = -static
endif

thread-tls: thread-tls.o
	$(CC) $(STATIC_FLAG) $(CFLAGS) -o $@ $^ $(LDFLAGS)
thread-tls.o: thread-tls.c
thread-tls.manifest: thread-tls.manifest.template
	gramine-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dmultiarch=$(shell $(CC) -dumpmachine) \
		$< $@
	gramine-manifest-check $@

# gramine-sgx-sign generates both a .sig file and a .manifest.sgx file. This is somewhat
# hard to express properly in Make. The simple solution would be to use
# "Rules with Grouped Targets" (`&:`), however make on Ubuntu <= 20.04 doesn't support it.
#
# Simply using a normal rule with "two targets" is equivalent to creating separate rules
# for each of the targets, and when using `make -j`, this might cause two instances
# of gramine-sgx-sign to get launched simultaneously, potentially breaking the build.
#
# As a workaround, we use a dummy intermediate target, and mark both files as depending on it, to
# get the dependency graph we want. We mark this dummy target as .INTERMEDIATE, which means
# that make will consider the source tree up-to-date even if the sgx_sign file doesn't exist,
# as long as the other dependencies check out. This is in contrast to .PHONY, which would
# be rebuilt on every invocation of make.
helloworld.sig helloworld.manifest.sgx: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: helloworld.manifest helloworld
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx
	gramine-manifest-check $<.sgx

ifeq ($(SGX),)
GRAMINE = saguaro-direct
else
GRAMINE = saguaro-sgx
endif

.PHONY: check
check: all
	$(GRAMINE) thread-tls
	$(GRAMINE) thread-rwlock
	$(GRAMINE) thread-stress
	@echo "[ Success ]"

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest *.o thread-stress thread-rwlock thread-tls OUTPUT

.PHONY: distclean
distclean: clean
