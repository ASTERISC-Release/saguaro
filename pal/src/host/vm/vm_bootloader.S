.macro globl sym
.globl \sym
.hidden \sym
.endm

    globl   pal_start

_start64:
    call    pal_start_c
    hlt
    jmp     .

    .code32
_start32:
    lgdt    (_gdtr - _start32)(%ebp)

    mov     $(_gdt_entry_kernel_cs - _gdt_start + 8), %eax  // 32-bit kernel data at this offset
    mov     %eax, %ds
    mov     %eax, %es
    mov     %eax, %fs
    mov     %eax, %gs
    mov     %eax, %ss

#ifdef PAL_HOST_SEV
    // AMD SEV (Documentation/devel/sev/PLAN.md): paging is still off, so every data access is
    // private (encrypted). If CPUID 0x8000001F reports SEV and SEV_STATUS (MSR 0xC0010131) bit 0
    // says it is active, remember the C-bit (CPUID 0x8000001F EBX[5:0]) and set it in the static
    // page tables below, so that the PAL's memory stays private once paging is on.
    mov     $0x80000000, %eax
    cpuid
    cmp     $0x8000001f, %eax
    jb      2f
    mov     $0x8000001f, %eax
    cpuid
    test    $2, %eax                    // SEV supported
    jz      2f
    mov     %ebx, %esi
    mov     $0xc0010131, %ecx           // SEV_STATUS MSR
    rdmsr
    test    $1, %eax                    // SEV enabled for this guest
    jz      2f
    mov     %eax, (g_sev_status - _start32)(%ebp)
    mov     %edx, (g_sev_status + 4 - _start32)(%ebp)
    mov     %esi, %ecx
    and     $0x3f, %ecx
    cmp     $32, %ecx                   // the C-bit is above bit 31 on every SEV CPU
    jb      2f
    sub     $32, %ecx
    mov     $1, %edx
    shl     %cl, %edx
    mov     %edx, (g_sev_c_bit_mask + 4 - _start32)(%ebp)
    lea     (_initial_page_tables - _start32)(%ebp), %esi
    lea     (_initial_pml4_table_base + 8 - _start32)(%ebp), %edi
1:  cmpl    $0, (%esi)
    je      3f
    or      %edx, 4(%esi)
3:  add     $8, %esi
    cmp     %edi, %esi
    jb      1b
2:
#endif

    lea     (_initial_pml4_table_base - _start32)(%ebp), %eax
    mov     %eax, %cr3

    mov     %cr4, %eax
    or      $(1 << 18) + (1 << 16) + (1 << 10) + (1 << 9) + (1 << 6) + (1 << 5), %eax   // + FSGSBASE: rdgsbase/wrgsbase for the %gs hand-over (kernel_events.S)
    mov     %eax, %cr4                  // OSXSAVE, OSXMMEXCPT, OSFXSR, MCE, PAE

    xor     %edx, %edx
    mov     $3, %eax                    // SSE and X87
    xor     %ecx, %ecx
    xsetbv

    mov     %cr0, %eax
    or      $(1 << 31), %eax            // PG
    mov     %eax, %cr0

    lea     (_sys_initial_stack - _start32)(%ebp), %esp
    lea     (_start64 - _start32)(%ebp), %eax
    push    $(_gdt_entry_kernel_cs - _gdt_start)
    push    %eax
    lret                                // far return to _start64 in 64-bit mode

    .section .resetvec, "ax"
    .align  0x80
    .fill   0x70, 1, 0xcc
pal_start:
    cli
    cld
    add     $(_start32 - _end), %ebp
    jmp     _start32
    .align  0x80, 0xcc
_end:

    .data
    .align  0x1000

// We use 1:1 virtual-to-physical address translation. For this, we use the classic 4-level paging
// scheme. In this bootloader, we define a static page table hierarchy spanning 32MB of RAM:
//   1. CR3 register points to a PML4 table, which contains a single PML4E 64-bit entry
//   2. PML4E entry points to a PDPT table, which contains a single PDPTE 64-bit entries
//   3. PDPTE entry points to a PD table, which contains 16 PDE 64-bit entries
//   4. PDE entry points to one of 16 PTs (page tables), each containing 512 PTE entries
//
// Note that each PT has 512 entries, each entry pointing to a 4KB page. There are 16 PTs, thus, our
// page table hierarchy describes 32MB of memory. After this bootloader completes and jumps to the
// 64-bit C kernel, the kernel immediately allocates memory for the final page table hierarchy
// spanning up to 512GB of memory and rewires the CR3 register to this final hierarchy.
//
// The below static PT hierarchy constitutes (16 + 1 + 1 + 1) 4KB pages, or 76KB of data segment of
// the final kernel executable in total.
//
// For visual represenation of this address translation, see Figure 4-8 in Intel SDM.

// 4KB leaf entries (PTE): 16 pages, each with 512 entries
#ifdef PAL_HOST_SEV
_initial_page_tables:
#endif
1:  .rept   16 * 512
    .quad   (. - 1b) / 8 * (1 << 12) + 0x7    // 7 = User, Writable, Present
    .endr
    .align  0x1000, 0

// 2MB (PDE) entries: one page with 16 entries
2:  .rept   16
    .quad   1b + (. - 2b) / 8 * 0x1000 + 0x7  // 7 = User, Writable, Present
    .endr
    .align  0x1000, 0

// 1GB (PDPTE) entries: one page with single entry
3:  .quad   2b + 0x7                          // 7 = User, Writable, Present
    .align  0x1000, 0

// 512GB entries (PML4E): one page with single entry
_initial_pml4_table_base:
    .quad   3b + 0x7                          // 7 = User, Writable, Present
    .align  0x1000, 0


// GDT
    .align  0x10
_gdt_start:
    .quad   0
_gdt_entry_kernel_cs:
    .long   0xffff, 0xaf9b00  // 64-bit kernel code
    .long   0xffff, 0xcf9300  // 32-bit kernel data
    .long   0x0068, 0xcf8900  // tss
_gdtr:
    .word   . - _gdt_start - 1
    .quad   _gdt_start


#ifdef PAL_HOST_SEV
    globl   g_sev_c_bit_mask
    globl   g_sev_status
    .align  8
g_sev_c_bit_mask:                     // the C-bit as a PTE mask, 0 when SEV is not active
    .quad   0
g_sev_status:                         // SEV_STATUS MSR as read at boot
    .quad   0
#endif

    .bss
    .align  0x10
    .space  0x4000
_sys_initial_stack:


    .section .note, "a"
    .align  4
    .long   2f - 1f
    .long   4f - 3f
    .long   18
1:  .string "Xen"
2:  .align  4
3:  .quad   pal_start
4:
