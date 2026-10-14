# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Test programs for graphics under gramine-direct (x86_64), see
# Documentation/devel/graphics-direct/. `make check` runs each natively and under gramine-direct.

PROGRAMS = drm_info gbm_map egl_readback
NODE ?= /dev/dri/renderD128

CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags libdrm)
LDLIBS_drm_info = $(shell pkg-config --libs libdrm)
LDLIBS_gbm_map = $(shell pkg-config --libs gbm)
LDLIBS_egl_readback = $(shell pkg-config --libs gbm egl glesv2)

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
all: $(PROGRAMS) $(addsuffix .manifest,$(PROGRAMS))

drm_info: drm_info.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_drm_info)

gbm_map: gbm_map.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS_gbm_map)

egl_readback: egl_readback.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gbm egl glesv2) $< -o $@ $(LDLIBS_egl_readback)

%.manifest: direct-tests.manifest.template FORCE
	gramine-manifest -Dlog_level=$(GRAMINE_LOG_LEVEL) -Dentrypoint=$* $< $@
	gramine-manifest-check $@

.PHONY: FORCE

.PHONY: check
check: all
	@for p in $(PROGRAMS); do \
		echo "== $$p native"; ./$$p $(NODE) || exit 1; \
		echo "== $$p gramine-direct"; gramine-direct $$p $(NODE) || exit 1; \
	done

.PHONY: eglinfo
eglinfo: eglinfo.manifest
	gramine-direct eglinfo -B

eglinfo.manifest: direct-tests.manifest.template FORCE
	gramine-manifest -Dlog_level=$(GRAMINE_LOG_LEVEL) -Dentrypoint=/usr/bin/eglinfo $< $@
	gramine-manifest-check $@

.PHONY: clean
clean:
	$(RM) $(PROGRAMS) *.manifest
