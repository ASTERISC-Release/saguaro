# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages this example for a Windows VBS enclave (Documentation/vbs.md,
# "Makefile flow"); `gramine-vbs tcpserver` runs it and `make VBS=1 check` checks the output.

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = tcpserver
VBS_TEMPLATE = tcpserver.manifest
VBS_DEPS = $(GRAMINE_VBS_REPO)/pal/src/host/vbs/test/tcpserver/tcpserver.c
VBS_CLEAN = tcpserver
include ../../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all:
	@echo "this example is for the VBS backend only: make VBS=1"
endif

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh

.PHONY: clean
clean:
	$(RM) -r tcpserver tcpserver.vbs tcpserver.manifest.vbs OUTPUT
