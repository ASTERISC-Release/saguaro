# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs, Documentation/vbs.rst): CPython built for the
# enclave once (scripts/vbs/build-python.sh), packed with the scripts by vbs.manifest.template
VBS_PYTHON_STAMP = ../../build-vbs-python/.vbs-python
VBS_APP = python
VBS_DEPS = $(wildcard scripts/*.py) helper-files/hosts $(VBS_PYTHON_STAMP)
include ../../scripts/vbs/Makefile.vbs
all: vbs-all

$(VBS_PYTHON_STAMP): | $(VBS_RUNTIME_STAMP)
	sh $(VBS_DIR)/build-python.sh --if-needed
else
all: python.manifest
ifeq ($(SGX),1)
all: python.manifest.sgx python.sig
endif
endif

RA_TYPE ?= none
RA_CLIENT_SPID ?=
RA_CLIENT_LINKABLE ?= 0

python.manifest: python.manifest.template
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dentrypoint=$(realpath $(shell sh -c "command -v python3")) \
		-Duser_site=$(shell python3 -m site --user-site) \
		-Dra_type=$(RA_TYPE) \
		-Dra_client_spid=$(RA_CLIENT_SPID) \
		-Dra_client_linkable=$(RA_CLIENT_LINKABLE) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
python.manifest.sgx python.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: python.manifest
	gramine-sgx-sign \
		--manifest $< \
		--output $<.sgx

.PHONY: check
check: all
ifeq ($(VBS),1)
	GRAMINE="$(GRAMINE)" VBS=1 ./run-tests.sh > TEST_STDOUT 2> TEST_STDERR
	@grep -q "Success 1/4" TEST_STDOUT
	@grep -q "Success 2/4" TEST_STDOUT
	@echo "[ Success: 1/4 and 2/4; NumPy and SciPy are not in the vbs CPython ]"
else
	./run-tests.sh > TEST_STDOUT 2> TEST_STDERR
	@grep -q "Success 1/4" TEST_STDOUT
	@grep -q "Success 2/4" TEST_STDOUT
	@grep -q "Success 3/4" TEST_STDOUT
	@grep -q "Success 4/4" TEST_STDOUT
ifeq ($(SGX),1)
	@grep -q "Success SGX report" TEST_STDOUT
	@grep -q "Success SGX quote" TEST_STDOUT
endif
endif

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT* *.PID TEST_STDOUT TEST_STDERR
	$(RM) -r scripts/__pycache__

.PHONY: distclean
distclean: clean
