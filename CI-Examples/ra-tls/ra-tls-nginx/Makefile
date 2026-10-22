# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

RA_TYPE ?= dcap
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
all: nginx.manifest.sgx nginx.sig

%.manifest: %.manifest.template
	saguaro-manifest \
		-Dentrypoint=$$(command -v saguaro-ratls) \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dra_type=$(RA_TYPE) \
		-Dra_client_spid=$(RA_CLIENT_SPID) \
		-Dra_client_linkable=$(RA_CLIENT_LINKABLE) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< > $@

%.manifest.sgx %.sig: sgx-sign-%
	@:

.INTERMEDIATE: sgx-sign-%
sgx-sign-%: %.manifest
	saguaro-sgx-sign \
		--manifest $< \
		--output $<.sgx

.PHONY: check
check: all
	@./run.sh

.PHONY: clean
clean:
	$(RM) -r \
		*.manifest \
		*.manifest.sgx \
		*.sig \
		*.token
