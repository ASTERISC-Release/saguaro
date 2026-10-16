# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SRCDIR = src
BUSYBOX_SHA256 ?= c35d87f1d04b2b153d33c275c2632e40d388a88f19a9e71727e0bbbff51fe689
BUSYBOX_SRC ?= busybox-1.32.0.tar.bz2
BUSYBOX_MIRRORS ?= \
    https://busybox.net/downloads \
    https://packages.gramineproject.io/distfiles

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

RA_TYPE ?= none
RA_CLIENT_SPID ?=
RA_CLIENT_LINKABLE ?= 0

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): BusyBox built with the vbs
# gcc (against the vbs glibc) in vbs-build/, packed by vbs.manifest.template
VBS_APP = busybox
VBS_DEPS = vbs-build/busybox helper-files/data.txt
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: busybox busybox.manifest
ifeq ($(SGX),1)
all: busybox.manifest.sgx busybox.sig
endif
endif

$(SRCDIR)/Makefile:
	../common_tools/download --output busybox.tar.bz2 --sha256 $(BUSYBOX_SHA256) \
		$(foreach mirror,$(BUSYBOX_MIRRORS),--url $(mirror)/$(BUSYBOX_SRC))
	mkdir $(SRCDIR)
	tar -C $(SRCDIR) --strip-components=1 -xf busybox.tar.bz2

$(SRCDIR)/.config: $(SRCDIR)/Makefile
	$(MAKE) -C $(SRCDIR) clean
	$(MAKE) -C $(SRCDIR) defconfig
	# Enable usage of Busybox's built-in applets
	sed -e 's/.*CONFIG_FEATURE_SH_STANDALONE.*/CONFIG_FEATURE_SH_STANDALONE=y/' \
		-i $(SRCDIR)/.config
	# Busybox 1.32's tc/arp/arping applets do not compile against current (>= ~6.x) Linux kernel
	# headers (missing struct tc_cbq_wrropt etc.), on any architecture. The test does not use them.
	sed -e 's/^CONFIG_\(TC\|ARP\|ARPING\)=y/# CONFIG_\1 is not set/' \
		-i $(SRCDIR)/.config

$(SRCDIR)/busybox: $(SRCDIR)/.config
	$(MAKE) -C $(SRCDIR)

busybox.manifest: busybox.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dra_type=$(RA_TYPE) \
		-Dra_client_spid=$(RA_CLIENT_SPID) \
		-Dra_client_linkable=$(RA_CLIENT_LINKABLE) \
		$< > $@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
busybox.manifest.sgx busybox.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: busybox.manifest busybox
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

# Copy Busybox binary to our root directory for simplicity.
busybox: $(SRCDIR)/busybox
	cp $(SRCDIR)/busybox busybox

ifeq ($(VBS),1)
# BusyBox for the enclave: the same configuration as above, built with the vbs gcc in vbs-build/
busybox.tar.bz2:
	../common_tools/download --output $@ --sha256 $(BUSYBOX_SHA256) \
		$(foreach mirror,$(BUSYBOX_MIRRORS),--url $(mirror)/$(BUSYBOX_SRC))

vbs-build/src/.config: busybox.tar.bz2 | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/src && mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xjf busybox.tar.bz2
	$(MAKE) -C vbs-build/src defconfig
	sed -e 's/.*CONFIG_FEATURE_SH_STANDALONE.*/CONFIG_FEATURE_SH_STANDALONE=y/' \
		-i vbs-build/src/.config
	sed -e 's/^CONFIG_\(TC\|ARP\|ARPING\)=y/# CONFIG_\1 is not set/' \
		-i vbs-build/src/.config

# the applets that need no fork (a VBS enclave has one process): echo, cat, ls, grep, sh builtins
vbs-build/busybox: vbs-build/src/.config
	$(MAKE) -C vbs-build/src CC=$(VBS_GCC)
	cp vbs-build/src/busybox $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
else ifeq ($(SGX),)
GRAMINE = gramine-direct
else
GRAMINE = gramine-sgx
endif

ifneq ($(VBS),1)
.PHONY: check
check: all
	$(GRAMINE) busybox echo "Hello" > OUTPUT
	@grep -q "Hello" OUTPUT && echo "[ Success 1/1 ]"
	@rm OUTPUT
endif

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest busybox OUTPUT

.PHONY: distclean
distclean: clean
	$(RM) -r $(SRCDIR) busybox.tar.bz2
