# Copyright (C) 2024 Gramine contributors
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause

# Build Node.js as follows:
#
# - make               -- create non-SGX manifest (system node)
# - make SGX=1         -- create SGX manifest
# - make VBS=1         -- pack the official Node.js linux-arm64 binary for a Windows VBS enclave, and
#                         sign it (in WSL; see Documentation/vbs.md); make VBS=1 check runs
#                         helloworld.js and heavy.js with --jitless in the enclave

THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
NODEJS_DIR ?= /usr/bin

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): the official node binary (not built with the vbs
# glibc: packed as it is, see Documentation/devel/vbs-enclave/apps-trials-arm64.md), --jitless
VBS_APP = nodejs
VBS_DEPS = vbs-build/node vbs-build/lib/libstdc++.so.6 vbs-build/lib/libgcc_s.so.1 helloworld.js heavy.js
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: nodejs.manifest
ifeq ($(SGX),1)
all: nodejs.manifest.sgx nodejs.sig
endif
endif

nodejs.manifest: nodejs.manifest.template helloworld.js
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dnodejs_dir=$(NODEJS_DIR) \
		-Dnodejs_usr_share_dir=$(wildcard /usr/share/nodejs) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# for details on this workaround see
# https://github.com/gramineproject/gramine/blob/e8735ea06c/CI-Examples/helloworld/Makefile
nodejs.manifest.sgx nodejs.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: nodejs.manifest
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

ifeq ($(VBS),1)
# GRAMINE is gramine-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

ifeq ($(VBS),1)
NODEJS_VERSION = 24.22.0
NODEJS_ARM64_SHA256 = fef00bb4d36e1c50104a9775a10359beee98f162b4104dd12472c75065c6a143
NODEJS_TARBALL = node-v$(NODEJS_VERSION)-linux-arm64.tar.xz

$(NODEJS_TARBALL):
	../common_tools/download --output $@ --sha256 $(NODEJS_ARM64_SHA256) \
		--url https://nodejs.org/dist/v$(NODEJS_VERSION)/$(NODEJS_TARBALL)

# the official binary, as it is (dynamic, glibc 2.39); the tarball's bin/node
vbs-build/node: $(NODEJS_TARBALL)
	mkdir -p vbs-build
	tar -C vbs-build -xJf $(NODEJS_TARBALL) --strip-components=2 node-v$(NODEJS_VERSION)-linux-arm64/bin/node
	touch $@

# the host's C++ runtime and unwinder (Ubuntu 24.04 gcc 13), with 4 KiB PT_LOAD alignment
vbs-build/lib/libstdc++.so.6:
	mkdir -p vbs-build/lib
	python3 patch-page-align.py $$(gcc -print-file-name=libstdc++.so.6) $@

vbs-build/lib/libgcc_s.so.1:
	mkdir -p vbs-build/lib
	python3 patch-page-align.py $$(gcc -print-file-name=libgcc_s.so.1) $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
else
.PHONY: check
check: all
	$(GRAMINE) ./nodejs helloworld.js > OUTPUT
	@grep -q "Hello World" OUTPUT && echo "[ Success 1/1 ]"
	@rm OUTPUT
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT

.PHONY: distclean
distclean: clean
