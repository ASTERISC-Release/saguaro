# SPDX-License-Identifier: LGPL-3.0-or-later
# Vulkan feasibility tests (Documentation/devel/vulkan/). `make` builds vk-headless; the SPIR-V
# headers are committed and only regenerated when a shader changes and glslangValidator exists.

CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS = -lvulkan
GLSLANG ?= glslangValidator

.PHONY: all
all: vk-headless

vk-headless: vk-headless.c compute_spv.h vert_spv.h frag_spv.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

compute_spv.h: compute.comp
	$(GLSLANG) -V --target-env vulkan1.1 --vn compute_spv -o $@ $<
vert_spv.h: tri.vert
	$(GLSLANG) -V --target-env vulkan1.1 --vn vert_spv -o $@ $<
frag_spv.h: tri.frag
	$(GLSLANG) -V --target-env vulkan1.1 --vn frag_spv -o $@ $<

.PHONY: clean
clean:
	$(RM) vk-headless *.manifest *.manifest.sgx *.sig
