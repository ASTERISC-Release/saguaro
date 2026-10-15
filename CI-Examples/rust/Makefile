# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SELF_EXE = target/release/rust-hyper-http-server

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): the server built by cargo
# for the vbs glibc (the vbs gcc is the linker), in vbs-build/, packed by vbs.manifest.template
VBS_APP = rust-hyper-http-server
VBS_DEPS = vbs-build/rust-hyper-http-server vbs-build/libgcc_s.so.1
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: $(SELF_EXE) rust-hyper-http-server.manifest
ifeq ($(SGX),1)
all: rust-hyper-http-server.manifest.sgx rust-hyper-http-server.sig
endif
endif

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

# Note that we're compiling in release mode regardless of the DEBUG setting passed
# to Make, as compiling in debug mode results in an order of magnitude's difference in
# performance that makes testing by running a benchmark with ab painful. The primary goal
# of the DEBUG setting is to control Gramine's loglevel.
-include $(SELF_EXE).d # See also: .cargo/config.toml
$(SELF_EXE): Cargo.toml
	cargo build --release

rust-hyper-http-server.manifest: rust-hyper-http-server.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dself_exe=$(SELF_EXE) \
		$< $@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
rust-hyper-http-server.manifest.sgx rust-hyper-http-server.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: rust-hyper-http-server.manifest $(SELF_EXE)
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

ifeq ($(VBS),1)
# GRAMINE is gramine-vbs (Makefile.vbs); vbs-check.sh runs the server and a request
vbs-build/rust-hyper-http-server: Cargo.toml src/main.rs | $(VBS_RUNTIME_STAMP)
	CARGO_TARGET_DIR=vbs-build/target \
	CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=$(VBS_GCC) \
	CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=$(VBS_GCC) \
		cargo build --release
	cp vbs-build/target/release/rust-hyper-http-server $@

# glibc's unwinder (libgcc_s.so.1, linked by Rust's std) is not in the vbs sysroot: the host's copy
# of the same GCC runtime is packed
vbs-build/libgcc_s.so.1:
	mkdir -p vbs-build
	cp -L "$(shell $(CC) -print-file-name=libgcc_s.so.1)" $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

.PHONY: start-gramine-server
start-gramine-server: all
	$(GRAMINE) rust-hyper-http-server

.PHONY: clean
clean:
	$(RM) -rf *.token *.sig *.manifest.sgx *.manifest *.manifest.vbs result-* OUTPUT

.PHONY: distclean
distclean: clean
	$(RM) -rf target/ Cargo.lock
