# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Bastion (GOG.com Linux build) under saguaro-vm with the Tirith graphics redirection (x86_64).
# The game is commercial: buy it on GOG.com and unpack it with ./setup-bastion.sh first. See
# README.md here and dev docs: graphics/ for the host setup.

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as saguaro.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
GAME_DIR = $(abspath bastion_game)
GAME_BIN = bastion_game/Bastion.bin.x86_64
# Save games and settings (guest /root/.local/share and /root/.config)
DATA_DIR ?= $(HOME)/.local/share/saguaro-bastion

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1
# 1: the host's SDL2 replaces the game's bundled one; 0: use the bundled SDL2.
SYSTEM_SDL ?= 1
# 1: sys.experimental__multiprocess = true (x86_64 VM PAL), so that FMOD's `sh -c` probe can run.
MULTIPROC ?= 0
# Game options; -windowed is also in the manifest (-noborder removes the window frame).
BASTION_ARGS ?= -windowed

ifeq ($(DEBUG),1)
SAGUARO_LOG_LEVEL = debug
else
SAGUARO_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(SAGUARO_LOG_LEVEL) \
	-Dgame_dir=$(GAME_DIR) \
	-Ddata_dir=$(DATA_DIR) \
	-Dhome=$(HOME) \
	-Dshared_input=$(SHARED_INPUT) \
	-Dsystem_sdl=$(SYSTEM_SDL) \
	-Dmultiproc=$(MULTIPROC)

.PHONY: all
all: bastion.manifest

$(GAME_BIN):
	@echo "Bastion is not unpacked: buy it on GOG.com, then run ./setup-bastion.sh" >&2; exit 1

.PHONY: FORCE
bastion.manifest: bastion.manifest.template $(GAME_BIN) FORCE
	saguaro-manifest $(MANIFEST_ARGS) $< $@
	saguaro-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(DATA_DIR)/share $(DATA_DIR)/config $(HOME)/.cache

# Without Gramine: the preload library presents through its own X connection. The saves of this
# run go to the same DATA_DIR, through XDG_DATA_HOME and XDG_CONFIG_HOME.
.PHONY: run-native
run-native: $(GAME_BIN) dirs
	cd bastion_game && env LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} \
		XDG_DATA_HOME=$(DATA_DIR)/share XDG_CONFIG_HOME=$(DATA_DIR)/config \
		SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null ./Bastion.bin.x86_64 $(BASTION_ARGS)

.PHONY: run
run: all dirs
	saguaro-vm bastion

# saguaro-direct on the host GPU (dev docs: graphics-direct/): stock Mesa with DRI3 by
# default; PRELOAD=1 uses the preload library instead (needs an install built with a VM-type PAL).
# The X server is the one of $(DISPLAY), reached through its Unix socket. Arguments come from the
# command line (BASTION_ARGS).
PRELOAD ?= 0
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))

bastion-direct.manifest: bastion-direct.manifest.template $(GAME_BIN) FORCE
	saguaro-manifest $(MANIFEST_ARGS) -Ddisplay=$(X_DISPLAY_NUM) -Dpreload=$(PRELOAD) \
		-Degl_discrete=$${EGL_DISCRETE:-0} $< $@
	saguaro-manifest-check $@

.PHONY: run-direct
run-direct: bastion-direct.manifest dirs
	saguaro-direct bastion-direct $(BASTION_ARGS)

.PHONY: clean
clean:
	$(RM) *.manifest

# The game files stay: remove bastion_game/ by hand to unpack again.
