# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Minetest 5.6.1 under saguaro-vm with the Tirith graphics redirection (x86_64). Build the game
# first with ./setup-minetest.sh; see dev docs: graphics/ for the host setup.

# The graphics preload library is built and installed with Saguaro (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as saguaro.libgraphics.
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
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

.PHONY: all
all: minetest.manifest

$(MT_BIN):
	@echo "Minetest is not built: run ./setup-minetest.sh first" >&2; exit 1

.PHONY: FORCE
minetest.manifest: minetest.manifest.template $(MT_BIN) FORCE
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dmt_dir=$(MT_DIR) \
		-Dhome=$(HOME) \
		-Dshared_input=$(SHARED_INPUT) \
		$< $@
	saguaro-manifest-check $@

.PHONY: dirs
dirs: $(MT_BIN)
	mkdir -p $(HOME)/.cache
	cp $(MT_CONF) $(MT_DIR)/minetest.conf

# Without Saguaro: the preload library presents through its own X connection.
.PHONY: run-native
run-native: $(MT_BIN) dirs
	LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} SDL_AUDIODRIVER=dummy \
		ALSOFT_DRIVERS=null $(MT_BIN) $(MT_ARGS)

.PHONY: run
run: all dirs
	saguaro-vm minetest

# saguaro-direct on the host GPU (dev docs: graphics-direct/): stock Mesa with DRI3 by
# default; PRELOAD=1 uses the preload library instead (needs an install built with a VM-type PAL).
# The X server is the one of $(DISPLAY), reached through its Unix socket.
PRELOAD ?= 0
# The host's library directory of this architecture, and the NVIDIA driver's device nodes in the
# direct manifest when the host has them (NVIDIA=0 or 1 overrides).
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
NVIDIA ?= $(if $(wildcard /dev/nvidiactl),1,0)
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))

minetest-direct.manifest: minetest-direct.manifest.template $(MT_BIN) FORCE
	saguaro-manifest \
		-Dlog_level=$(SAGUARO_LOG_LEVEL) \
		-Dmt_dir=$(MT_DIR) \
		-Ddisplay=$(X_DISPLAY_NUM) \
		-Darch_libdir=$(ARCH_LIBDIR) \
		-Dnvidia=$(NVIDIA) \
		-Dpreload=$(PRELOAD) \
		-Degl_discrete=$${EGL_DISCRETE:-0} \
		$< $@
	saguaro-manifest-check $@

.PHONY: run-direct
run-direct: minetest-direct.manifest dirs
	saguaro-direct minetest-direct

# The same game without Saguaro and without the preload library (stock Mesa), for comparison.
.PHONY: run-native-stock
run-native-stock: $(MT_BIN) dirs
	SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null vblank_mode=0 $(MT_BIN) $(MT_ARGS)

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r luanti-5.6.1
