# ============================================================
# OmegaOS - boot.s
# Transition vers le mode 64 bits (Long Mode)
# ============================================================

/* ---- Constantes Multiboot 1 ---- */
#define MULTIBOOT_MAGIC    0x1BADB002
#define MULTIBOOT_FLAGS    0x00000003 /* ALIGN + MEMINFO (pas de AOUT_KLUDGE pour ELF64) */
#define MULTIBOOT_CHECKSUM -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot, "aw"
.align 4
multiboot_header:
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM

/* ---- Sections de données pour le boot ---- */
.section .bss
.align 4096
pml4_table: .skip 4096
pdpt_table: .skip 4096
pd_table:   .skip 4096
pt_table:   .skip 4096

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

/* ---- Code de démarrage (32 bits) ---- */
.section .text
.code32
.global _start

_start:
    cli
    # 1. TRACE 'A' (Rouge) : Le noyau a démarré
    movw $0x4F41, (0xB8000)

    # Sauvegarde le pointeur Multiboot (EBX)
    mov %ebx, %esi

    # Setup de la pile temporaire
    mov $stack_top, %esp

    # 2. Nettoyage de la section BSS
    mov $_sbss, %edi
    mov $_ebss, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    # TRACE '2' : BSS nettoyé
    movw $0x0F32, (0xB8002)

    # 3. Configuration de la pagination (Identity Map 2Mo)
    # PML4[0] -> PDPT
    mov $pdpt_table, %eax
    or $0x3, %eax
    mov %eax, (pml4_table)

    # PDPT[0] -> PD
    mov $pd_table, %eax
    or $0x3, %eax
    mov %eax, (pdpt_table)

    # PD[0] -> PT
    mov $pt_table, %eax
    or $0x3, %eax
    mov %eax, (pd_table)

    # Remplissage de la PT (Map 512 pages de 4Ko = 2Mo)
    mov $pt_table, %edi
    mov $0x03, %eax # Present + Writable
    mov $512, %ecx
.loop_pt:
    mov %eax, (%edi)
    add $4096, %eax
    add $8, %edi
    loop .loop_pt

    # TRACE '3' : Tables de pages prêtes
    movw $0x0F33, (0xB8004)

    # 4. Activation du mode Long
    mov $pml4_table, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $(1 << 5), %eax # PAE
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax # LME
    wrmsr

    # TRACE '4' : LME Activé
    movw $0x0F34, (0xB8006)

    mov %cr0, %eax
    or $(1 << 31), %eax # Paging ON
    mov %eax, %cr0

    # Chargement de la GDT 64 bits
    lgdt (gdt64_ptr)

    # TRACE '5' : Saut vers 64 bits
    movw $0x0F35, (0xB8008)

    pushl $0x08               # Code segment selector (64-bit)
    pushl $long_mode_entry
    lret

/* ---- Code noyau (64 bits) ---- */
.code64
long_mode_entry:
    # Reset des segments
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs

    # TRACE 'K' (Vert) : Succès total !
    movw $0x2F4B, (0xB8000)

    # Passer le pointeur Multiboot au C (RDI est le 1er arg en 64-bit)
    mov %rsi, %rdi
    
    call kernel_main

.halt_loop:
    hlt
    jmp .halt_loop