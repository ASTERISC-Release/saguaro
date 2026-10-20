# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages this example for a Windows VBS enclave (Documentation/vbs.md,
# "Makefile flow"); `gramine-vbs dynhello` runs it and `make VBS=1 check` checks the output.

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = dynhello
VBS_TEMPLATE = vbs.manifest.template
VBS_DEPS = $(GRAMINE_VBS_REPO)/pal/src/host/vbs/test/dynhello/dynhello.c
VBS_CLEAN = dynhello
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	$(GRAMINE) dynhello > OUTPUT
	grep -q "sorted 1 3 5 7 9" OUTPUT
	grep -q "cos(0) = 1" OUTPUT
	@echo "[ Success ]"

.PHONY: clean
clean:
	$(RM) -r dynhello dynhello.vbs dynhello.manifest.vbs OUTPUT
