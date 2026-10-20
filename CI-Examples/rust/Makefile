# Copyright (C) 2023 Gramine contributors
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause

# Build the Rust HTTP server as follows:
#
# - make               -- create non-SGX manifest
# - make SGX=1         -- create SGX manifest
# - make VBS=1         -- build the server for a Windows VBS enclave (a static PIE against the vbs
#                         glibc, rustup's stable toolchain), pack and sign it (in WSL; see
#                         dev docs: vbs-enclave/manual.md); make VBS=1 check runs it and asks it for a page

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SELF_EXE = target/release/rust-hyper-http-server

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): the server built by cargo for the host triple, with
# std built from source (-Zbuild-std) and -Zfixed-x18 (x18 is the enclave's TEB register), linked as
# a static PIE against the vbs glibc (vbs-linker.sh)
VBS_APP = rust-hyper-http-server
VBS_DEPS = vbs-build/rust-hyper-http-server
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
# GRAMINE is gramine-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

ifeq ($(VBS),1)
RUST_TARGET = $(shell uname -m)-unknown-linux-gnu
RUST_TARGET_ENV = $(shell echo $(RUST_TARGET) | tr 'a-z-' 'A-Z_')
# -Zfixed-x18 (x18 is the enclave's TEB) needs the nightly flags on the stable toolchain
# (RUSTC_BOOTSTRAP=1) and std built from source (-Zbuild-std, rustup component rust-src)
RUST_VBS_FLAGS = -C target-feature=+crt-static
ifeq ($(shell uname -m),aarch64)
RUST_VBS_FLAGS += -Zfixed-x18
endif
# no shared libgcc_s in the image: vbs-linker.sh maps rustc's -lgcc_s to the static libgcc_eh
RUST_VBS_LINK = -C link-arg=-static-libgcc

vbs-build/rust-hyper-http-server: Cargo.toml $(wildcard src/*.rs) | $(VBS_RUNTIME_STAMP)
	env SYSROOT=$(VBS_SYSROOT) RUST_VBS_CC_STATIC=$(VBS_DIR)/cc-static-pie.sh \
		CARGO_TARGET_$(RUST_TARGET_ENV)_LINKER=$(CURDIR)/vbs-linker.sh \
		RUSTC_BOOTSTRAP=1 RUSTFLAGS="$(RUST_VBS_FLAGS) $(RUST_VBS_LINK)" \
		cargo build -Zbuild-std --release --target $(RUST_TARGET) --target-dir vbs-build/target
	cp vbs-build/target/$(RUST_TARGET)/release/rust-hyper-http-server $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

.PHONY: start-gramine-server
start-gramine-server: all
	$(GRAMINE) rust-hyper-http-server

ifneq ($(VBS),1)
# the server under $(GRAMINE), curl on the host's loopback (check.sh)
.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./check.sh
endif

.PHONY: clean
clean:
	$(RM) -rf *.token *.sig *.manifest.sgx *.manifest result-* OUTPUT

.PHONY: distclean
distclean: clean
	$(RM) -rf target/ Cargo.lock
