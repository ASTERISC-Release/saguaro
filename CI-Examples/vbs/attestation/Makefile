# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The ASTERISC Research Lab
#
# `make VBS=1` builds and packages the attestation example for a Windows VBS enclave
# (dev docs: vbs-enclave/manual.md, "Makefile flow"). attest.manifest has no template variables, so it is the
# template of attest.manifest.vbs; its vbs.build compiles attest.c for the enclave.
#   make VBS=1
#   gramine-vbs attest <64 hex digits>
#   make VBS=1 check        runs it and verifies the report

.PHONY: all
ifeq ($(VBS),1)
VBS_APP = attest
VBS_TEMPLATE = attest.manifest
VBS_DEPS = attest.manifest
VBS_CLEAN = attest
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
	$(RM) -r attest attest.vbs attest.manifest.vbs OUTPUT
