# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` for the application examples: the VBS counterpart of `make SGX=1`
# (dev docs: vbs-enclave/manual.md, "Makefile flow: make VBS=1"). An example's Makefile includes this file
# under `ifeq ($(VBS),1)` and sets, before the include:
#
#   VBS_APP      the application's name: vbs.manifest.template is rendered into
#                $(VBS_APP).manifest.vbs, and the image is built in $(VBS_APP).vbs/
#   VBS_DEPS     files the image is made from (the application built for the enclave, scripts,
#                data): the image is rebuilt when one of them changes
#   VBS_DEFINES  extra -Dkey=value for the template (optional)
#   VBS_CLEAN    what `make VBS=1 clean` removes besides the image and the manifest (optional)
#
# and makes its `all` depend on vbs-all. Everything that goes into the enclave is listed in the
# template's vbs.packed_files (measured into the signed image); the application is built with
# $(VBS_GCC) (a gcc for the vbs glibc: dynamic PIE, -ffixed-x18) or $(VBS_CC_STATIC) /
# $(VBS_CC_DYNAMIC), into a folder of its own so that the direct and SGX builds are not touched.
#
# Where the image is linked and signed: in WSL with interop, on the Windows of the same machine
# (saguaro-vbs copies the objects to a Windows folder; see saguaro-vbs --help for
# SAGUARO_VBS_WINDOWS_BASE); elsewhere on the Windows machine named by SAGUARO_VBS_WINDOWS (ssh);
# with SAGUARO_VBS_WINDOWS=none or no Windows only the Linux half is built (`make VBS=1 vbs-sign`
# later). $(SAGUARO) runs the application like saguaro-direct: `$(SAGUARO) $(VBS_APP) args`.
#
# The runtime (the vbs glibc, PAL and LibOS) is not built here: `scripts/install.sh --vbs` builds it
# once per checkout (and again when its sources change), and `make VBS=1` stops with a message when
# it is not there.

VBS_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SAGUARO_VBS_REPO ?= $(abspath $(VBS_DIR)/../..)

SAGUARO_VBS := $(VBS_DIR)/saguaro-vbs
SAGUARO = $(SAGUARO_VBS)
VBS_SYSROOT := $(SAGUARO_VBS_REPO)/build-vbs-glibc
VBS_GCC := $(VBS_DIR)/vbs-gcc
VBS_CC_STATIC := env SYSROOT=$(VBS_SYSROOT) $(VBS_DIR)/cc-static-pie.sh
VBS_CC_DYNAMIC := env SYSROOT=$(VBS_SYSROOT) $(VBS_DIR)/cc-dynamic-pie.sh
# the in-enclave folder of the vbs glibc's libraries (loader, libc, libm, ...)
VBS_LIBDIR := /usr/lib

ifeq ($(VBS_APP),)
$(error set VBS_APP before including Makefile.vbs)
endif
VBS_TEMPLATE ?= vbs.manifest.template
VBS_MANIFEST := $(VBS_APP).manifest.vbs
VBS_IMAGE := $(VBS_APP).vbs/image.stamp
# written by `scripts/install.sh --vbs` (saguaro-vbs runtime); it changes only when the runtime
# objects, the vbs glibc or the sources of the Windows half changed, which rebuilds the images
VBS_RUNTIME_STAMP := $(SAGUARO_VBS_REPO)/build-vbs/runtime.stamp

SAGUARO_LOG_LEVEL ?= error

# the example's own `all` stays the default goal
.DEFAULT_GOAL := all

.PHONY: vbs-all vbs-runtime vbs-sign vbs-stop vbs-clean
vbs-all: $(VBS_IMAGE)

# the runtime is installed by scripts/install.sh --vbs, not built by make: this only checks it
vbs-runtime: $(VBS_RUNTIME_STAMP)
$(VBS_RUNTIME_STAMP):
	@echo "error: the VBS runtime (the vbs glibc, PAL and LibOS) is not installed in $(SAGUARO_VBS_REPO)." >&2
	@echo "       Install it once with:  $(SAGUARO_VBS_REPO)/scripts/install.sh --vbs" >&2
	@exit 1

$(VBS_MANIFEST): $(VBS_TEMPLATE)
	$(SAGUARO_VBS) manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Darch_libdir=$(VBS_LIBDIR) \
		$(VBS_DEFINES) \
		$< $@

# pack (saguaro-vbs build), compile the Windows objects, and link and sign on Windows when one is
# reachable; the stamp is written only when all of that succeeded
$(VBS_IMAGE): $(VBS_MANIFEST) $(VBS_DEPS) $(VBS_RUNTIME_STAMP)
	$(SAGUARO_VBS) build $<
	touch $@

vbs-sign: $(VBS_IMAGE)
	$(SAGUARO_VBS) sign $(VBS_APP).vbs

# stop every enclave host of this image (servers started in the background)
vbs-stop:
	-$(SAGUARO_VBS) stop $(VBS_APP).vbs

# `make VBS=1 clean` also removes the VBS files (the Windows folder of the image stays)
clean: vbs-clean
vbs-clean:
	$(RM) -r $(VBS_APP).vbs $(VBS_MANIFEST) $(VBS_CLEAN)
