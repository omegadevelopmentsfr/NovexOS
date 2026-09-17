# ============================================================
# NovexOS - boot.s
# Transition to 64-bit Long Mode
# VBE graphics switched on-demand via BGA (Bochs VBE Extensions)
# ============================================================

/* ---- Multiboot 1 constants ---- */
#define MULTIBOOT_MAGIC    0x1BADB002
#define MULTIBOOT_FLAGS    0x00000003 /* ALIGN + MEMINFO */
#define MULTIBOOT_CHECKSUM -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "aw"
.align 4
multiboot_header:
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM

/* ---- Boot data sections ---- */
.section .bss
.align 4096
.global pml4_table
.global pdpt_table
pml4_table: .skip 4096
pdpt_table: .skip 4096
pd_table:   .skip 4096
/* Page tables for 16MB identity mapping (8 x 4KB page tables) */
pt_table0: .skip 4096
pt_table1: .skip 4096
pt_table2: .skip 4096
pt_table3: .skip 4096
pt_table4: .skip 4096
pt_table5: .skip 4096
pt_table6: .skip 4096
pt_table7: .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .rodata
.align 8
gdt64:
    .quad 0                         /* Null Descriptor */
    .quad 0x00af9a000000ffff        /* 0x08: Code Kernel (64-bit Long Mode) */
    .quad 0x00cf92000000ffff        /* 0x10: Data Kernel (64-bit) */
gdt64_ptr:
    .word . - gdt64 - 1
    .quad gdt64

/* ---- Startup code (32-bit) ---- */
.section .text
.code32
.global _start

_start:
    cli
    # 1. TRACE 'K' (White on Blue): Kernel entry reached
    movw $0x1F4B, (0xB800A)

    # TRACE 'A' (Red): Kernel started
    movw $0x4F41, (0xB8000)

    # Save the Multiboot pointer (EBX)
    mov %ebx, %esi

    # Set up temporary stack
    mov $stack_top, %esp

    # 1.5. Zero all page tables
    # (PML4, PDPT, PD are 3 tables, then 8 PTs = 11 tables total)
    mov $pml4_table, %edi
    mov $11264, %ecx  # (11 * 4096) / 4 -- 11 page tables of 4096B, in dwords
    xor %eax, %eax
    rep stosl

    # 2. Clear BSS section
    mov $_sbss, %edi
    mov $_ebss, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    # TRACE '2': BSS cleared
    movw $0x0F32, (0xB8002)

    # 3. Set up paging (Identity Map 16MB)
    # NOTE: "mov %eax, (symbol)" is deliberately avoided here.
    # GAS mis-encodes that accumulator+absolute-address form as a
    # 64-bit moffs (opcode A3 with an 8-byte address) even inside a
    # .code32 section, because this whole file is assembled with -m64
    # (required for the .code64 half below). That extra 4 bytes
    # desyncs every instruction after it, which is what was crashing
    # the boot. Fix: always go through a scratch register (%edx) so
    # GAS is forced to emit the normal ModRM encoding instead.

    # PML4[0] -> PDPT
    mov $pdpt_table, %eax
    or $0x3, %eax
    mov $pml4_table, %edx
    mov %eax, (%edx)

    # PDPT[0] -> PD
    mov $pd_table, %eax
    or $0x3, %eax
    mov $pdpt_table, %edx
    mov %eax, (%edx)

    # PD[0] -> PT0, PD[1] -> PT1, ..., PD[7] -> PT7
    # Each PD entry covers 2MB, so 8 entries = 16MB

    mov $pt_table0, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, (%edx)

    mov $pt_table1, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 8(%edx)

    mov $pt_table2, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 16(%edx)

    mov $pt_table3, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 24(%edx)

    mov $pt_table4, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 32(%edx)

    mov $pt_table5, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 40(%edx)

    mov $pt_table6, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 48(%edx)

    mov $pt_table7, %eax
    or $0x3, %eax
    mov $pd_table, %edx
    mov %eax, 56(%edx)

    # Fill the 8 PTs (map 8x512 4KB pages = 16MB)
    mov $pt_table0, %edi
    mov $0x03, %eax            # Present + Writable
    mov $0x1000, %ecx       # (512 * 8) = 4096 entries total (16MB)
.loop_pt:
    mov %eax, (%edi)           # Low 32 bits
    movl $0, 4(%edi)           # High 32 bits (Must be 0 for identity map < 4GB)
    add $4096, %eax
    add $8, %edi
    loop .loop_pt

    # TRACE '3': Page tables ready
    movw $0x0F33, (0xB8004)

    # 4. Enable Long Mode
    mov $pml4_table, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $0x20, %eax # PAE (bit 5)
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax # LME (bit 8)
    wrmsr

    # TRACE '4': LME enabled
    movw $0x0F34, (0xB8006)

    mov %cr0, %eax
    or $0x80000000, %eax # Paging ON (bit 31)
    mov %eax, %cr0

    # Load 64-bit GDT
    lgdt (gdt64_ptr)

    # TRACE '5': Jump to 64-bit
    movw $0x0F35, (0xB8008)

    pushl $0x08               # Code segment selector (64-bit)
    pushl $long_mode_entry
    lret

/* ---- Kernel code (64-bit) ---- */
.code64
long_mode_entry:
    # Reset segments
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs

    # TRACE 'K' (Green): Boot complete!
    movw $0x2F4B, (0xB8000)

    # Pass Multiboot pointer to C (RDI is the 1st argument in 64-bit)
    mov %rsi, %rdi
    
    call kernel_main

.halt_loop:
    hlt
    jmp .halt_loop