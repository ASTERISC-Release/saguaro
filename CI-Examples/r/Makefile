# Copyright (C) 2024 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

# Installation location of R. By default, Saguaro will run the system R executable.
R_HOME ?= /usr/lib/R
R_EXEC = $(R_HOME)/bin/exec/R

ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

LD_LIBRARY_PATH := $(LD_LIBRARY_PATH):$(R_HOME)/lib
export LD_LIBRARY_PATH

.PHONY: all
all: R.manifest
ifeq ($(SGX),1)
all: R.manifest.sgx R.sig
endif

R.manifest: R.manifest.template
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dr_home=$(R_HOME) \
		-Dr_exec=$(R_EXEC) \
		$< >$@

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# for details on this workaround see
# https://github.com/gramineproject/gramine/blob/e8735ea06c/CI-Examples/helloworld/Makefile
R.manifest.sgx R.sig: sgx_sign
	@:

.INTERMEDIATE: sgx_sign
sgx_sign: R.manifest
	saguaro-sgx-sign \
		--manifest $< \
		--output $<.sgx

ifeq ($(SGX),)
SAGUARO = saguaro-direct
else
SAGUARO = saguaro-sgx
endif

.PHONY: check
check: all
	$(SAGUARO) ./R --slave --vanilla -f scripts/sample.r > OUTPUT 2> /dev/null
	@grep -q "success" OUTPUT && echo "[ Success 1/1 ]"
	@$(RM) OUTPUT

.PHONY: clean
clean:
	$(RM) *.manifest *.manifest.sgx *.token *.sig OUTPUT

.PHONY: distclean
distclean: clean
