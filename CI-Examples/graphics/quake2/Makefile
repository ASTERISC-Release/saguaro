# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Yamagi Quake II under gramine-vm with the Tirith graphics redirection (x86_64). Build the game
# first with ./setup-quake.sh; see Documentation/devel/graphics/ for the host setup.

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as gramine.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
Q2_DIR = $(abspath quake2_bin)
Q2_BIN = quake2_bin/release/quake2
Q2_ARGS ?= +vid_maxfps 999 +toggleconsole

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(GRAMINE_LOG_LEVEL) \
	-Dq2_dir=$(Q2_DIR) \
	-Dhome=$(HOME) \
	-Dshared_input=$(SHARED_INPUT)

.PHONY: all
all: quake2.manifest

$(Q2_BIN):
	@echo "Quake II is not built: run ./setup-quake.sh first" >&2; exit 1

.PHONY: FORCE
quake2.manifest: quake2.manifest.template $(Q2_BIN) FORCE
	gramine-manifest $(MANIFEST_ARGS) $< $@
	gramine-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(HOME)/.yq2 $(HOME)/.cache

# Without Gramine: the preload library presents through its own X connection. Quake II refuses to
# run as root, so as root (the usual case in the container) it is started in a user namespace that
# shows the same user as uid 1000; under Gramine the manifest's loader.uid does the same job.
ifeq ($(shell id -u),0)
NATIVE_AS = unshare --user --map-user=1000 --map-group=1000 env
else
NATIVE_AS = env
endif

.PHONY: run-native
run-native: $(Q2_BIN) dirs
	$(NATIVE_AS) LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} \
		SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null $(Q2_BIN) $(Q2_ARGS)

.PHONY: run
run: all dirs
	gramine-vm quake2

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r quake2_bin
