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
VBS_DEPS = $(wildcard scripts/*.py) helper-files/hosts $(VBS_PYTHON_STAMP) vbs-build/site/.stamp
include ../../scripts/vbs/Makefile.vbs
all: vbs-all

$(VBS_PYTHON_STAMP): | $(VBS_RUNTIME_STAMP)
	sh $(VBS_DIR)/build-python.sh --if-needed

# NumPy and SciPy: the manylinux wheels for CPython 3.12 (PyPI, checked by SHA-256), unpacked into
# vbs-build/site/ and packed with the host's copies of the runtime they need (libstdc++, libgcc_s,
# libz: not in the vbs sysroot; their GLIBC symbols are within the sysroot's 2.39)
# The runtime libraries from build-x18-runtime.sh (used when present; the host's copies name x18
# and do not pack)
VBS_X18_RUNTIME ?= $(HOME)/.local/share/gramine-vbs/x18-runtime/lib

# The wheel files are those of the machine's architecture (the x86_64 wheels do not run, and do not
# pack, on AArch64).
ifeq ($(shell uname -m),aarch64)
NUMPY_WHEEL = numpy-2.5.4-cp312-cp312-manylinux_2_27_aarch64.manylinux_2_28_aarch64.whl
NUMPY_SHA256 = fe4d21ab149f15e4e6043dfb0de87e6e5f34ac176cde83060e9802981fca2ac2
NUMPY_URL = https://files.pythonhosted.org/packages/dc/96/8319e2457ae4333c62c815c7006b869a4f60985c1e01024c2f8c6c040fe5/$(NUMPY_WHEEL)
SCIPY_WHEEL = scipy-1.18.1-cp312-cp312-manylinux_2_27_aarch64.manylinux_2_28_aarch64.whl
SCIPY_SHA256 = 3c085faa2cfa879c5141df483f836f4d691045a078224a670fa570fa01612d89
SCIPY_URL = https://files.pythonhosted.org/packages/75/9a/2e71719f31eaefe0e3a1706c4a1ded94e664bfd95ffca2b219a671faee01/$(SCIPY_WHEEL)
else
NUMPY_WHEEL = numpy-2.5.4-cp312-cp312-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl
NUMPY_SHA256 = fbde6962867ee75b48b0ee29b2b9372ec5d617799dbaf38e82dc0596f2f7738a
NUMPY_URL = https://files.pythonhosted.org/packages/43/a3/c799c62e19c337e6d3770b08e475887fb30ce8477d3c09efca6b2f0228a6/$(NUMPY_WHEEL)
SCIPY_WHEEL = scipy-1.18.1-cp312-cp312-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl
SCIPY_SHA256 = f55fa87b6c612ecd6b058f167c53231b1d14e412efe361d3d6e38b3631c73218
SCIPY_URL = https://files.pythonhosted.org/packages/df/64/ff35eb9e54894cf471ff4716abd3c81eb0a0626869217ce3e6ba4ccf17d7/$(SCIPY_WHEEL)
endif

$(NUMPY_WHEEL):
	../common_tools/download --output $@ --sha256 $(NUMPY_SHA256) --url $(NUMPY_URL)
$(SCIPY_WHEEL):
	../common_tools/download --output $@ --sha256 $(SCIPY_SHA256) --url $(SCIPY_URL)

vbs-build/site/.stamp: $(NUMPY_WHEEL) $(SCIPY_WHEEL) | $(VBS_RUNTIME_STAMP)
	rm -rf vbs-build/site vbs-build/lib && mkdir -p vbs-build/site vbs-build/lib
	unzip -q $(NUMPY_WHEEL) -d vbs-build/site
	unzip -q $(SCIPY_WHEEL) -d vbs-build/site
ifneq ($(wildcard $(VBS_X18_RUNTIME)/libstdc++.so.6),)
	# the runtime libraries rebuilt with -ffixed-x18 (scripts/vbs/build-x18-runtime.sh)
	cp -L $(VBS_X18_RUNTIME)/libstdc++.so.6 $(VBS_X18_RUNTIME)/libgcc_s.so.1 \
		$(VBS_X18_RUNTIME)/libz.so.1 vbs-build/lib/
else
	cp -L "$(shell $(CC) -print-file-name=libstdc++.so.6)" vbs-build/lib/
	cp -L "$(shell $(CC) -print-file-name=libgcc_s.so.1)" vbs-build/lib/
	cp -L /usr/lib/$(shell $(CC) -dumpmachine)/libz.so.1 vbs-build/lib/
endif
	touch $@
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
	@grep -q "Success 3/4" TEST_STDOUT
	@grep -q "Success 4/4" TEST_STDOUT
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
