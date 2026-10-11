# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Minetest 5.6.1 under gramine-vm with the Tirith graphics redirection (x86_64). Build the game
# first with ./setup-minetest.sh; see Documentation/devel/graphics/ for the host setup.

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as gramine.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
MT_DIR = $(abspath luanti-5.6.1)
MT_BIN = luanti-5.6.1/bin/minetest
# Start the prepared world directly, without the main menu.
MT_ARGS ?= --go --worldname world1
# Settings file copied into the game directory before every run (the game rewrites its copy on
# exit): minetest.conf is moderate with a 60 FPS cap, minetest-max.conf is Tirith's maximum quality
# without a cap.
MT_CONF ?= minetest.conf

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1


ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

.PHONY: all
all: minetest.manifest

$(MT_BIN):
	@echo "Minetest is not built: run ./setup-minetest.sh first" >&2; exit 1

.PHONY: FORCE
minetest.manifest: minetest.manifest.template $(MT_BIN) FORCE
	gramine-manifest \
		-Dlog_level=$(GRAMINE_LOG_LEVEL) \
		-Dmt_dir=$(MT_DIR) \
		-Dhome=$(HOME) \
		-Dshared_input=$(SHARED_INPUT) \
		$< $@
	gramine-manifest-check $@

.PHONY: dirs
dirs: $(MT_BIN)
	mkdir -p $(HOME)/.cache
	cp $(MT_CONF) $(MT_DIR)/minetest.conf

# Without Gramine: the preload library presents through its own X connection.
.PHONY: run-native
run-native: $(MT_BIN) dirs
	LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} SDL_AUDIODRIVER=dummy \
		ALSOFT_DRIVERS=null $(MT_BIN) $(MT_ARGS)

.PHONY: run
run: all dirs
	gramine-vm minetest

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r luanti-5.6.1
