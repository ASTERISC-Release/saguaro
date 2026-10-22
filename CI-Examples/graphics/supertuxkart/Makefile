# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# SuperTuxKart under saguaro-vm with the Tirith graphics redirection (x86_64). Build the game first
# with ./install-stk.sh; see dev docs: graphics/ for the host setup.

# The graphics preload library is built and installed with Saguaro (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as saguaro.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
STK_DIR = $(abspath .)
STK_BIN = stk-build/stk-code/cmake_build/bin/supertuxkart
STK_ARGS ?= --race-now --screensize=1920x1080 --seed=11 --track=abyss --kart=konqi
# Length in seconds of the automatic race run by `run-startup` and `run-native-startup`.
PROFILE_TIME ?= 1
# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1

STK_STARTUP_ARGS = --race-now --benchmark --profile-time=$(PROFILE_TIME) --screensize=1920x1080 \
	--windowed --seed=11 --track=abyss --kart=konqi

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(SAGUARO_LOG_LEVEL) \
	-Dstk_dir=$(STK_DIR) \
	-Dhome=$(HOME) \
	-Dprofile_time=$(PROFILE_TIME) \
	-Dshared_input=$(SHARED_INPUT)

.PHONY: all
all: supertuxkart.manifest supertuxkart-startup.manifest

$(STK_BIN):
	@echo "SuperTuxKart is not built: run ./install-stk.sh first" >&2; exit 1

supertuxkart.manifest: supertuxkart.manifest.template $(STK_BIN) FORCE
	saguaro-manifest $(MANIFEST_ARGS) $< $@
	saguaro-manifest-check $@

# Automatic race of PROFILE_TIME seconds that prints the frame-rate statistics and exits.
.PHONY: FORCE
supertuxkart-startup.manifest: supertuxkart.manifest.template $(STK_BIN) FORCE
	saguaro-manifest $(MANIFEST_ARGS) -Dstartup_probe=1 $< $@
	saguaro-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(HOME)/.config $(HOME)/.local/share $(HOME)/.cache

# Without Saguaro: the preload library presents through its own X connection. Run this once before
# the first VM run; it creates the game's configuration under ~/.config/supertuxkart.
.PHONY: run-native
run-native: $(STK_BIN) dirs
	cd $(dir $(STK_BIN)) && LD_PRELOAD=$(GRAPHICS_LIB) \
		EGL_DISCRETE=$${EGL_DISCRETE:-0} SDL_AUDIODRIVER=dummy ./supertuxkart $(STK_ARGS)

.PHONY: run-native-startup
run-native-startup:
	$(MAKE) run-native STK_ARGS="$(STK_STARTUP_ARGS)"

.PHONY: run
run: all dirs
	saguaro-vm supertuxkart

.PHONY: run-startup
run-startup: all dirs
	saguaro-vm supertuxkart-startup

# saguaro-direct on the host GPU (dev docs: graphics-direct/): stock Mesa with DRI3 by
# default; PRELOAD=1 uses the preload library instead (needs an install built with a VM-type PAL).
# The X server is the one of $(DISPLAY), reached through its Unix socket.
PRELOAD ?= 0
# The host's library directory of this architecture, and the NVIDIA driver's device nodes in the
# direct manifest when the host has them (NVIDIA=0 or 1 overrides).
ARCH_LIBDIR ?= /lib/$(shell $(CC) -dumpmachine)
NVIDIA ?= $(if $(wildcard /dev/nvidiactl),1,0)
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))
DIRECT_MANIFEST_ARGS = $(MANIFEST_ARGS) -Ddisplay=$(X_DISPLAY_NUM) -Darch_libdir=$(ARCH_LIBDIR) -Dnvidia=$(NVIDIA) \
	-Dpreload=$(PRELOAD) \
	-Degl_discrete=$${EGL_DISCRETE:-0}

supertuxkart-direct.manifest: supertuxkart-direct.manifest.template $(STK_BIN) FORCE
	saguaro-manifest $(DIRECT_MANIFEST_ARGS) $< $@
	saguaro-manifest-check $@

supertuxkart-direct-startup.manifest: supertuxkart-direct.manifest.template $(STK_BIN) FORCE
	saguaro-manifest $(DIRECT_MANIFEST_ARGS) -Dstartup_probe=1 $< $@
	saguaro-manifest-check $@

.PHONY: run-direct run-direct-startup
run-direct: supertuxkart-direct.manifest dirs
	saguaro-direct supertuxkart-direct
run-direct-startup: supertuxkart-direct-startup.manifest dirs
	saguaro-direct supertuxkart-direct-startup

# The benchmark without Saguaro and without the preload library (stock Mesa), for comparison.
.PHONY: run-native-stock-startup
run-native-stock-startup: $(STK_BIN) dirs
	cd $(dir $(STK_BIN)) && SDL_AUDIODRIVER=dummy vblank_mode=0 ./supertuxkart $(STK_STARTUP_ARGS)

.PHONY: clean
clean:
	$(RM) *.manifest

.PHONY: distclean
distclean: clean
	$(RM) -r stk-build
