# SPDX-License-Identifier: LGPL-3.0-or-later
# Vulkan feasibility tests (Documentation/devel/vulkan/). `make` builds vk-headless; the SPIR-V
# headers are committed and only regenerated when a shader changes and glslangValidator exists.

CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS = -lvulkan
GLSLANG ?= glslangValidator

.PHONY: all
all: vk-headless vk-mt vk-two vk-session-shim.so clock-probe xwin-dump xsend-key

xsend-key: xsend-key.c
	$(CC) $(CFLAGS) $< -o $@ -lX11

xwin-dump: xwin-dump.c
	$(CC) $(CFLAGS) $< -o $@ -lxcb

vk-mt: vk-mt.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS) -lpthread

# several graphics processes at once (design-gpu.md): forks N children that exec /vk-headless
vk-two: vk-two.c
	$(CC) $(CFLAGS) $< -o $@

vk-session-shim.so: vk-session-shim.c
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@ -ldl

vk-headless: vk-headless.c compute_spv.h vert_spv.h frag_spv.h
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

compute_spv.h: compute.comp
	$(GLSLANG) -V --target-env vulkan1.1 --vn compute_spv -o $@ $<
vert_spv.h: tri.vert
	$(GLSLANG) -V --target-env vulkan1.1 --vn vert_spv -o $@ $<
frag_spv.h: tri.frag
	$(GLSLANG) -V --target-env vulkan1.1 --vn frag_spv -o $@ $<

# gramine-direct: `make direct-<prog>.manifest` for vk-headless (this directory) or a program in
# /usr/bin (vulkaninfo, vkcube, vkmark); then `gramine-direct direct-<prog> [args]`.
GRAMINE_LOG_LEVEL ?= error
EAGER ?= 0
SHIM ?= 0
# PRESENT=1: presentation through libgraphics.so's Vulkan layer (phase 2)
PRESENT ?= 0
LAYER_DIR = $(shell python3 -c "import graminelibos; print(graminelibos._CONFIG_PKGLIBDIR)")/vulkan
X_DISPLAY_NUM = $(patsubst :%,%,$(firstword $(subst ., ,$(DISPLAY))))
ETC_VULKAN = $(if $(wildcard /etc/vulkan),1,0)
prog_binary = $(if $(filter vk-headless vk-mt vk-two clock-probe,$(1)),$(1),/usr/bin/$(1))

.PHONY: FORCE
direct-%.manifest: vk-direct.manifest.template FORCE
	gramine-manifest -Dlog_level=$(GRAMINE_LOG_LEVEL) -Ddisplay=$(X_DISPLAY_NUM) \
		-Detc_vulkan=$(ETC_VULKAN) -Dentrypoint=/$* -Dbinary=$(call prog_binary,$*) $< $@
	gramine-manifest-check $@

# gramine-vm: `make vm-<prog>.manifest` (EGL_DISCRETE=1 for renderD129; EAGER=1 for programs that
# dlopen the Vulkan loader; SHIM=1 for the phase-0 shim; PRESENT=1 for windows through the layer;
# MULTIPROCESS=1 for the VM PAL's demand-paged processes, which vm-vk-two needs: its children are
# processes; TWO=1 also mounts /vk-headless for them), then
# `QEMU_PATH=<qemu-tirith>/build/qemu-system-x86_64 gramine-vm vm-<prog> [args]`.
vm-%.manifest: vk-vm.manifest.template vk-session-shim.so FORCE
	gramine-manifest -Dlog_level=$(GRAMINE_LOG_LEVEL) -Degl_discrete=$${EGL_DISCRETE:-0} \
		-Deager=$(EAGER) -Dshim=$(SHIM) -Dpresent=$(PRESENT) -Dlayer_dir=$(LAYER_DIR) \
		-Dmultiprocess=$${MULTIPROCESS:-0} -Dtwo=$${TWO:-0} \
		-Detc_vulkan=$(ETC_VULKAN) -Dentrypoint=/$* -Dbinary=$(call prog_binary,$*) $< $@
	gramine-manifest-check $@ || echo "(gramine-manifest-check failed; see TESTING.md)"

.PHONY: clean
clean:
	$(RM) vk-headless vk-mt vk-two vk-session-shim.so clock-probe xwin-dump xsend-key *.manifest *.manifest.sgx *.sig
