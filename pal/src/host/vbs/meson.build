# The Windows VBS-enclave PAL is an AArch64 *Windows* enclave DLL plus a Windows host executable,
# produced by the Windows/clang toolchain (link /ENCLAVE + veiid + signtool), not by this meson
# build, which targets the build machine's own (Linux) toolchain. There is therefore nothing for
# meson to compile here in phase 1: enabling -Dvbs=enabled only records the intent and points at
# the out-of-tree build scripts. Keeping the option wired (default disabled) means the normal
# PAL builds are unaffected while the PAL grows on this branch. See
# Documentation/devel/vbs-enclave/PLAN.md (D1, D3) and scripts/vbs/.

warning('The vbs PAL (-Dvbs=enabled) is built with the Windows/clang toolchain via scripts/vbs/, ' +
        'not by meson. This meson build produces no vbs artifacts. See ' +
        'Documentation/devel/vbs-enclave/PLAN.md.')
