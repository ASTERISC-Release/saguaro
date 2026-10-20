# Copyright (C) 2024 Gramine contributors
# Copyright (C) 2026 The ASTERISC Research Lab
# SPDX-License-Identifier: BSD-3-Clause

# Build iperf3 as follows:
#
# - make               -- create non-SGX manifest (gramine-direct)
# - make SGX=1         -- create SGX manifest
# - make VBS=1         -- build iperf3 for a Windows VBS enclave, pack and sign it (in WSL; see
#                         Documentation/vbs.md); make VBS=1 check runs the server in the enclave
#                         and a client on the loopback

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

IPERF_DIR = iperf
IPERF_SHA256 ?= cc740c6bbea104398cc3e466befc515a25896ec85e44a662d5f4a767b9cf713e
IPERF_SRC ?= iperf-3.16.tar.gz
IPERF_MIRRORS ?= https://github.com/esnet/iperf/releases/download/3.16

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): iperf3 built with the vbs gcc against the vbs
# glibc (no OpenSSL, no shared libiperf) in vbs-build/, packed by vbs.manifest.template
VBS_APP = iperf3
VBS_DEPS = vbs-build/iperf3 vbs-build/libgcc_s.so.1
VBS_CLEAN = vbs-build OUTPUT client.log
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: install/iperf3 install/libiperf.so.0 iperf3.manifest
ifeq ($(SGX),1)
all: iperf3.manifest.sgx iperf3.sig
endif
endif

$(IPERF_DIR)/configure:
	../common_tools/download --output $(IPERF_SRC) --sha256 $(IPERF_SHA256) \
		$(foreach mirror,$(IPERF_MIRRORS),--url $(mirror)/$(IPERF_SRC))
	mkdir $(IPERF_DIR)
	tar -C $(IPERF_DIR) --strip-components=1 -xf $(IPERF_SRC)

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# for details on this workaround see
# https://github.com/gramineproject/gramine/blob/e8735ea06c/CI-Examples/helloworld/Makefile
$(IPERF_DIR)/src/.libs/iperf3 $(IPERF_DIR)/src/.libs/libiperf.so.0: iperf_build
	@:

.INTERMEDIATE: iperf_build
iperf_build: $(IPERF_DIR)/configure
	cd $(IPERF_DIR) && ./configure
	$(MAKE) -C $(IPERF_DIR)

iperf3.manifest: iperf3.manifest.template install/iperf3 install/libiperf.so.0
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< > $@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# for details on this workaround see
# https://github.com/gramineproject/gramine/blob/e8735ea06c/CI-Examples/helloworld/Makefile
iperf3.sig iperf3.manifest.sgx: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: iperf3.manifest
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

# for simplicity, copy iperf3 executable into our install directory
install/iperf3: $(IPERF_DIR)/src/.libs/iperf3
	mkdir -p install
	cp $< $@

# for simplicity, copy libiperf library into our install directory
install/libiperf.so.0: $(IPERF_DIR)/src/.libs/libiperf.so.0
	mkdir -p install
	cp $< $@

ifeq ($(VBS),1)
$(IPERF_SRC):
	../common_tools/download --output $@ \
		--sha256 $(IPERF_SHA256) \
		$(foreach mirror,$(IPERF_MIRRORS),--url $(mirror)/$(IPERF_SRC))

# no OpenSSL (only the authentication feature needs it) and no shared library: one executable
vbs-build/src/configure: $(IPERF_SRC)
	mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xf $(IPERF_SRC)
	touch $@

vbs-build/src/Makefile: vbs-build/src/configure | $(VBS_RUNTIME_STAMP)
	# (-Wno-error=incompatible-pointer-types: iperf 3.16 does not build with the default errors of GCC 14 and later)
	cd vbs-build/src && ./configure CC=$(VBS_GCC) CFLAGS="-g -O2 -Wno-error=incompatible-pointer-types" --disable-shared --without-openssl

vbs-build/iperf3: vbs-build/src/Makefile
	$(MAKE) -C vbs-build/src CC=$(VBS_GCC)
	cp vbs-build/src/src/iperf3 $@

# the host's libgcc_s: glibc's pthread_cancel loads it (the vbs glibc does not ship it)
vbs-build/libgcc_s.so.1:
	mkdir -p vbs-build
	cp "$$(gcc -print-file-name=libgcc_s.so.1)" $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

ifneq ($(VBS),1)
# the server under $(GRAMINE), the native client on the host's loopback (check.sh)
GRAMINE ?= gramine-direct

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./check.sh
endif

################################## CLEANUP ####################################

.PHONY: clean
clean:
	$(RM) -r *.token *.sig *.manifest.sgx *.manifest install

.PHONY: distclean
distclean: clean
	$(RM) -r $(IPERF_DIR) *.tar.gz
