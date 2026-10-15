/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2026 The ASTERISC Research Lab */

/*
 * Loader for the packed objects of this enclave image (phase 2 of
 * Documentation/devel/vbs-enclave/PLAN.md, refinement (a') of decision D2).
 *
 * At build time scripts/vbs/pack.py places the load image of each AArch64 Linux shared object
 * (the LibOS, the patched glibc, the application, its listed libraries) verbatim into the `.gxpack`
 * section of the signed enclave image and generates a table of them. This loader prepares one such
 * object for use: it makes the object's own writable segments read-write, applies the object's
 * dynamic relocations there, binds its imports to a caller-supplied table, and returns its entry.
 * It never creates executable memory and never executes anything outside the signed image: the
 * code it returns lies in the image's executable section, which the platform measured and signed.
 */

#ifndef PAL_VBS_PACKED_H
#define PAL_VBS_PACKED_H

#include <stdint.h>

/* One symbol the enclave offers to packed objects (resolves their undefined symbols). */
struct pal_vbs_import {
    const char* name;
    void*       addr;
};

/* Result of preparing a packed object. */
struct pal_vbs_loaded {
    const char* name;
    uint8_t*    base;       /* load base inside the image's .gxpack section */
    uint64_t    size;       /* size of the load image */
    void*       entry;      /* address of the entry symbol chosen at pack time, or NULL */
    void*       dynamic;    /* the object's PT_DYNAMIC, inside its writable segment */
    uint64_t    nrela;      /* relocations applied (DT_RELA + DT_JMPREL) */
    uint64_t    nimports;   /* undefined symbols bound to the import table */
    uint64_t    nrelro;     /* RELRO pages made read-only again after relocation */
};

/* Error codes (negative); the loader writes no byte before every check has passed. */
enum {
    PAL_VBS_PK_OK          = 0,
    PAL_VBS_PK_NOTFOUND    = -1,  /* no object of that name in the packed table */
    PAL_VBS_PK_BADELF      = -2,  /* header, program headers or dynamic section malformed */
    PAL_VBS_PK_LAYOUT      = -3,  /* writable range overlaps executable code or leaves the image */
    PAL_VBS_PK_PROTECT     = -4,  /* VirtualProtect to read-write failed */
    PAL_VBS_PK_RELTYPE     = -5,  /* unsupported relocation type */
    PAL_VBS_PK_RELTARGET   = -6,  /* relocation target outside the object's writable segments */
    PAL_VBS_PK_UNDEF       = -7,  /* undefined symbol not in the import table */
    PAL_VBS_PK_LOADED      = -8,  /* already prepared */
};

/* Number of packed objects and the name of the i-th (NULL if out of range). */
uint64_t pal_vbs_packed_count(void);
const char* pal_vbs_packed_name(uint64_t i);

/*
 * Prepare the packed object `name`. On failure returns a negative code above and, if `detail` is
 * not NULL, a value that locates the problem (an offset in the object, a relocation type or the
 * index of a symbol). On success fills `out`.
 */
int64_t pal_vbs_packed_load(const char* name, const struct pal_vbs_import* imports,
                            uint64_t nimports, struct pal_vbs_loaded* out, uint64_t* detail);

/* The generated table and the bounds of the two packed sections (for the vbs PAL proper). */
struct pal_vbs_packed_obj;
const struct pal_vbs_packed_obj* pal_vbs_packed_table(uint64_t* count, uint64_t* blob_base,
                                                       uint64_t* blob_size, uint64_t* files_base,
                                                       uint64_t* files_size);

/* Look up a defined dynamic symbol of a prepared object (NULL if absent). */
void* pal_vbs_packed_sym(const struct pal_vbs_loaded* obj, const char* sym);
#if defined(_M_X64)
/* nonzero if `rip` is a `syscall` of the packed code rewritten into `ud2` by pack.py */
int pal_vbs_is_syscall_site(uint64_t rip);
#endif

#endif /* PAL_VBS_PACKED_H */
