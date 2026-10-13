# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 The ASTERISC Research Lab

# Bastion (GOG.com Linux build) under gramine-vm with the Tirith graphics redirection (x86_64).
# The game is commercial: buy it on GOG.com and unpack it with ./setup-bastion.sh first. See
# README.md here and Documentation/devel/graphics/ for the host setup.

# The graphics preload library is built and installed with Gramine (-Dgraphics=enabled and a
# VM-type PAL); the manifest refers to it as gramine.libgraphics.
GRAPHICS_LIB = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/libgraphics.so
GAME_DIR = $(abspath bastion_game)
GAME_BIN = bastion_game/Bastion.bin.x86_64
# Save games and settings (guest /root/.local/share and /root/.config)
DATA_DIR ?= $(HOME)/.local/share/gramine-bastion

# Input goes through the shared-memory ring in the comm page; 0 falls back to the X connection.
SHARED_INPUT ?= 1
# 1: the host's SDL2 replaces the game's bundled one; 0: use the bundled SDL2.
SYSTEM_SDL ?= 1

ifeq ($(DEBUG),1)
GRAMINE_LOG_LEVEL = debug
else
GRAMINE_LOG_LEVEL = error
endif

MANIFEST_ARGS = \
	-Dlog_level=$(GRAMINE_LOG_LEVEL) \
	-Dgame_dir=$(GAME_DIR) \
	-Ddata_dir=$(DATA_DIR) \
	-Dhome=$(HOME) \
	-Dshared_input=$(SHARED_INPUT) \
	-Dsystem_sdl=$(SYSTEM_SDL)

.PHONY: all
all: bastion.manifest

$(GAME_BIN):
	@echo "Bastion is not unpacked: buy it on GOG.com, then run ./setup-bastion.sh" >&2; exit 1

.PHONY: FORCE
bastion.manifest: bastion.manifest.template $(GAME_BIN) FORCE
	gramine-manifest $(MANIFEST_ARGS) $< $@
	gramine-manifest-check $@

.PHONY: dirs
dirs:
	mkdir -p $(DATA_DIR)/share $(DATA_DIR)/config $(HOME)/.cache

# Without Gramine: the preload library presents through its own X connection. The saves of this
# run go to the same DATA_DIR, through XDG_DATA_HOME and XDG_CONFIG_HOME.
.PHONY: run-native
run-native: $(GAME_BIN) dirs
	cd bastion_game && env LD_PRELOAD=$(GRAPHICS_LIB) EGL_DISCRETE=$${EGL_DISCRETE:-0} \
		XDG_DATA_HOME=$(DATA_DIR)/share XDG_CONFIG_HOME=$(DATA_DIR)/config \
		SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null ./Bastion.bin.x86_64

.PHONY: run
run: all dirs
	gramine-vm bastion

.PHONY: clean
clean:
	$(RM) *.manifest

# The game files stay: remove bastion_game/ by hand to unpack again.
