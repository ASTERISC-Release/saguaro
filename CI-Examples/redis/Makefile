# Copyright (C) 2023 Gramine contributors
# SPDX-License-Identifier: BSD-3-Clause

# Build Redis as follows:
#
# - make               -- create non-SGX no-debug-log manifest
# - make SGX=1         -- create SGX no-debug-log manifest
# - make SGX=1 DEBUG=1 -- create SGX debug-log manifest
# - make VBS=1         -- build Redis for a Windows VBS enclave, pack and sign it (in WSL; see
#                         dev docs: vbs-enclave/manual.md); make VBS=1 check runs it and checks it
#
# Any of these invocations clones Redis' git repository and builds Redis in
# default configuration and in the latest-to-date (6.0.5) version.
#
# By default, Redis uses poll/epoll mechanism of Linux. To build Redis with
# select, use `make USE_SELECT=1`. For correct re-builds, always clean up
# Redis source code beforehand via `make distclean`.
#
# Use `make clean` to remove Gramine-generated files and `make distclean` to
# additionally remove the cloned Redis git repository.

################################# CONSTANTS ###################################

# directory with arch-specific libraries, used by Redis
# the below path works for Debian/Ubuntu; for CentOS/RHEL/Fedora, you should
# overwrite this default like this: `ARCH_LIBDIR=/lib64 make`
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)

SRCDIR = src
COMMIT = 6.0.5
TAR_SHA256 = f7ded6c27d48c20bc78e797046c79b6bc411121f0c2d7eead9fea50d6b0b6290

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
ifeq ($(VBS),1)
# Windows VBS enclave (scripts/vbs/Makefile.vbs): Redis built with the vbs gcc (against the vbs
# glibc, libc malloc) in vbs-build/, packed by vbs.manifest.template
VBS_APP = redis-server
VBS_DEPS = vbs-build/redis-server
VBS_CLEAN = vbs-build OUTPUT
include ../../scripts/vbs/Makefile.vbs
all: vbs-all
else
all: redis-server redis-server.manifest
ifeq ($(SGX),1)
all: redis-server.manifest.sgx redis-server.sig
endif
endif

############################## REDIS EXECUTABLE ###############################

# Redis is built as usual, without any changes to the build process (except to
# test select syscall instead of poll/epoll). The source is downloaded from the
# GitHub repo (6.0.5 tag) and built via `make`. The result of this build process
# is the final executable "src/redis-server".

$(SRCDIR)/Makefile:
	../common_tools/download --output redis.tar.gz \
		--sha256 $(TAR_SHA256) \
		--url https://github.com/antirez/redis/archive/$(COMMIT).tar.gz \
		--url https://packages.gramineproject.io/distfiles/redis-$(COMMIT).tar.gz
	mkdir $(SRCDIR)
	tar -C $(SRCDIR) --strip-components=1 -xf redis.tar.gz

ifeq ($(USE_SELECT),1)
$(SRCDIR)/src/redis-server: $(SRCDIR)/Makefile
	sed -i 's|#define HAVE_EPOLL 1|/* no HAVE_EPOLL */|g' src/src/config.h
	$(MAKE) -C $(SRCDIR)
else
$(SRCDIR)/src/redis-server: $(SRCDIR)/Makefile
	$(MAKE) -C $(SRCDIR)
endif

ifeq ($(VBS),1)
redis.tar.gz:
	../common_tools/download --output $@ \
		--sha256 $(TAR_SHA256) \
		--url https://github.com/antirez/redis/archive/$(COMMIT).tar.gz \
		--url https://packages.gramineproject.io/distfiles/redis-$(COMMIT).tar.gz

vbs-build/src/Makefile: redis.tar.gz
	mkdir -p vbs-build/src
	tar -C vbs-build/src --strip-components=1 -xf redis.tar.gz
	touch $@

# MALLOC=libc: the vbs glibc's malloc instead of the bundled jemalloc (fewer moving parts in the
# enclave); CC reaches the bundled dependencies (hiredis, lua, linenoise) through MAKEFLAGS
vbs-build/redis-server: vbs-build/src/Makefile | $(VBS_RUNTIME_STAMP)
	$(MAKE) -C vbs-build/src CC=$(VBS_GCC) MALLOC=libc
	cp vbs-build/src/src/redis-server $@

.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./vbs-check.sh
endif

################################ REDIS MANIFEST ###############################

# The template file is a Jinja2 template and contains almost all necessary
# information to run Redis under Gramine / Gramine-SGX. We create
# redis-server.manifest (to be run under non-SGX Gramine) by replacing variables
# in the template file using the "saguaro-manifest" script.

redis-server.manifest: redis-server.manifest.template
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		$< > $@

# Manifest for Gramine-SGX requires special "saguaro-sgx-sign" procedure. This
# procedure measures all Redis trusted files, adds the measurement to the
# resulting manifest.sgx file (among other, less important SGX options) and
# creates redis-server.sig (SIGSTRUCT object).

# Make on Ubuntu <= 20.04 doesn't support "Rules with Grouped Targets" (`&:`),
# see the helloworld example for details on this workaround.
redis-server.sig redis-server.manifest.sgx: sgx_outputs
	@:

.INTERMEDIATE: sgx_outputs
sgx_outputs: redis-server.manifest $(SRCDIR)/src/redis-server
	saguaro-sgx-sign \
		--manifest redis-server.manifest \
		--output redis-server.manifest.sgx

########################### COPIES OF EXECUTABLES #############################

# Redis build process creates the final executable as src/redis-server. For
# simplicity, copy it into our root directory.

redis-server: $(SRCDIR)/src/redis-server
	cp $< $@

############################## RUNNING TESTS ##################################

.PHONY: start-native-server
start-native-server: all
	./redis-server --save '' --protected-mode no

ifeq ($(VBS),1)
# GRAMINE is saguaro-vbs (Makefile.vbs)
else ifeq ($(SGX),)
GRAMINE = saguaro-direct
else
GRAMINE = saguaro-sgx
endif

# Note that command-line arguments are hardcoded in the manifest file.
.PHONY: start-gramine-server
start-gramine-server: all
	$(GRAMINE) redis-server

################################## CLEANUP ####################################

ifneq ($(VBS),1)
# the server under $(GRAMINE), a client on the host's loopback (check.sh)
.PHONY: check
check: all
	GRAMINE="$(GRAMINE)" ./check.sh
endif

.PHONY: clean
clean:
	$(RM) *.token *.sig *.manifest.sgx *.manifest redis-server *.rdb

.PHONY: distclean
distclean: clean
	$(RM) -r $(SRCDIR) redis.tar.gz
