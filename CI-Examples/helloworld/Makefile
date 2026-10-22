# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

CFLAGS = -Wall -Wextra

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
CFLAGS += -g
else
SAGUARO_LOG_LEVEL = error
CFLAGS += -O3
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, dev docs: vbs-enclave/manual.md): the program built for the
# enclave (a static PIE against the vbs glibc) in vbs-build/, packed by vbs.manifest.template
VBS_APP = helloworld
VBS_DEPS = vbs-build/helloworld
VBS_CLEAN = vbs-build
include ../../scripts/vbs/Makefile.vbs
all: vbs-all

vbs-build/helloworld: helloworld.c | $(VBS_RUNTIME_STAMP)
	mkdir -p vbs-build
	$(VBS_CC_STATIC) $(CFLAGS) -o $@ $<
else
all: helloworld helloworld.manifest
ifeq ($(SGX),1)
all: helloworld.manifest.sgx helloworld.sig
endif
endif

helloworld: helloworld.o

helloworld.o: helloworld.c

helloworld.manifest: helloworld.manifest.template
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		$< $@
	saguaro-manifest-check $@

# saguaro-sgx-sign generates both a .sig file and a .manifest.sgx file. This is somewhat
# hard to express properly in Make. The simple solution would be to use
# "Rules with Grouped Targets" (`&:`), however make on Ubuntu <= 20.04 doesn't support it.
#
# Simply using a normal rule with "two targets" is equivalent to creating separate rules
# for each of the targets, and when using `make -j`, this might cause two instances
# of saguaro-sgx-sign to get launched simultaneously, potentially breaking the build.
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
	saguaro-sgx-sign \
		--manifest $< \
		--output $<.sgx
	saguaro-manifest-check $<.sgx

ifeq ($(VBS),1)
# SAGUARO is saguaro-vbs (Makefile.vbs)
else ifeq ($(SGX),)
SAGUARO = saguaro-direct
else
SAGUARO = saguaro-sgx
endif

.PHONY: check
check: all
	$(SAGUARO) helloworld > OUTPUT
	echo "Hello, world" | diff OUTPUT -
	@echo "[ Success ]"

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest helloworld.o helloworld OUTPUT

.PHONY: distclean
distclean: clean
