# Copyright (C) 2024 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

THIS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
NODEJS_DIR ?= /usr/bin

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

NODE_VERSION = 22.23.3
NODE_TARBALL = node-v$(NODE_VERSION)-linux-x64.tar.xz
NODE_SHA256 = df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): the official Node.js
# linux-x64 binary (not rebuilt), with the host's libstdc++ and GCC runtime, packed into the image.
# V8 runs with --jitless (no executable memory in a VBS enclave), see vbs-check.sh
VBS_APP = nodejs
VBS_DEPS = vbs-build/node vbs-build/libstdc++.so.6 vbs-build/libgcc_s.so.1 helloworld.js heavy.js
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
node-$(NODE_VERSION)-linux-x64.tar.xz:
	../common_tools/download --output $@ --sha256 $(NODE_SHA256) \
		--url https://nodejs.org/dist/v$(NODE_VERSION)/$(NODE_TARBALL)

vbs-build/node: node-$(NODE_VERSION)-linux-x64.tar.xz
	mkdir -p vbs-build
	tar -C vbs-build -xJf $< node-v$(NODE_VERSION)-linux-x64/bin/node
	mv vbs-build/node-v$(NODE_VERSION)-linux-x64/bin/node $@
	rm -rf vbs-build/node-v$(NODE_VERSION)-linux-x64

# libstdc++ and the GCC runtime: not in the vbs sysroot, so the host's copies of the same runtime
vbs-build/libstdc++.so.6:
	mkdir -p vbs-build
	cp -L "$(shell $(CC) -print-file-name=libstdc++.so.6)" $@

vbs-build/libgcc_s.so.1:
	mkdir -p vbs-build
	cp -L "$(shell $(CC) -print-file-name=libgcc_s.so.1)" $@

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
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT *.manifest.vbs

.PHONY: distclean
distclean: clean
