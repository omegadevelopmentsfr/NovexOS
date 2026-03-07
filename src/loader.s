.code16
.section .text
.global _start

/* 
 * Stage 2 Loader (V3)
 * Linked at 0. Loaded at 0x10000 (1000:0000).
 */

_start:
    cli
    /* Standard Real Mode Setup */
    xor %ax, %ax
    mov %ax, %es
    mov %ax, %ss
    mov $0x7C00, %sp
    
    /* Set DS to our segment (0x1000) so labels work */
    mov $0x1000, %ax
    mov %ax, %ds
    sti

    /* Print Stage 2 Banner */
    mov $msg_s2, %si
    call print_string
    
    /* Enable A20 Line (Fast A20) */
    in $0x92, %al
    or $2, %al
    out %al, $0x92

    /* Target buffer for kernel reading: 0x20000 (above loader) */
    mov $0x2000, %ax
    mov %ax, %es
    xor %bx, %bx            /* ES:BX = 0x2000:0 */
    
    mov $msg_loading, %si
    call print_string

    mov (kernel_lba), %eax   /* DS:kernel_lba = 0x1000:496 */
    mov (kernel_sectors), %cx

load_loop:
    cmp $0, %cx
    je load_done
    
    push %cx
    mov $1, %cx
    call read_sectors
    pop %cx
    
    inc %eax                /* Next LBA */
    mov %es, %dx
    add $0x20, %dx          /* Next 512 bytes */
    mov %dx, %es
    
    /* Heartbeat dot */
    mov $0x0E2E, %ax
    int $0x10
    
    dec %cx
    jmp load_loop

load_done:
    mov $msg_jump, %si
    call print_string

    /* Transition to Protected Mode */
    cli
    lgdt gdt_ptr
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* Far jump to 32-bit segment. 
     * Linear address = 0x10000 + offset of pm_start.
     */
    .byte 0x66, 0xEA
    .long pm_start + 0x10000
    .word 0x08

.code32
pm_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    
    /* Pulse '1' (PM Start) */
    movw $0x0E31, 0xB801A
    
    /* Move Kernel from 0x20000 to 0x100000 (1MB) */
    mov $0x20000, %esi
    mov $0x100000, %edi
    mov 0x10000 + 500, %ecx
    shl $7, %ecx
    rep movsl

    /* Pulse '2' (Move Done) */
    movw $0x0E32, 0xB801C

    /* Multiboot Handoff */
    mov $0x2BADB002, %eax   /* MAGIC MUST BE IN EAX */
    
    /* Set up a minimal Multiboot Info structure at 0x10200 */
    mov $0x10200, %ebx
    movl $0x00000001, (%ebx) /* Flags: meminfo only */
    movl $640, 4(%ebx)       /* mem_lower (640K) */
    movl $16384, 8(%ebx)     /* mem_upper (16M) */

    /* Pulse '3' (Pre-Jump) */
    movw $0x0E33, 0xB801E

    /* FINAL JUMP TO KERNEL ENTRY POINT */
    /* Use ECX to hold the jump target so EAX/EBX stay intact */
    mov 0x10000 + 504, %ecx
    jmp *%ecx

/* --- Helper Functions (16-bit) --- */
.code16
print_string:
    mov $0x0E, %ah
.ps_loop:
    lodsb
    test %al, %al
    jz .ps_done
    int $0x10
    jmp .ps_loop
.ps_done:
    ret

/* Read Sectors via INT 13h AH=42h (Extensions) */
read_sectors:
    push %eax
    mov %eax, (dap_lba)
    mov %bx, (dap_offset)
    mov %es, %ax
    mov %ax, (dap_segment)
    
    mov $0x42, %ah
    mov $0x80, %dl
    mov $dap, %si
    int $0x13
    jc disk_error
    pop %eax
    ret

disk_error:
    mov $msg_error, %si
    call print_string
.halt:
    hlt
    jmp .halt

/* --- Data --- */
msg_s2:      .asciz "\r\nOmegaStage2: "
msg_loading: .asciz "loading"
msg_jump:    .asciz " ok!\r\nJumping to OmegaOS Kernel...\r\n"
msg_error:   .asciz " DISK ERROR!\r\n"

.align 4
dap:
    .byte 0x10
    .byte 0x00
    dap_count: .word 1
    dap_offset: .word 0
    dap_segment: .word 0
    dap_lba: .quad 0

.align 8
gdt_start:
    .quad 0
    .quad 0x00CF9A000000FFFF /* Code (32-bit PM) */
    .quad 0x00CF92000000FFFF /* Data */
gdt_ptr:
    .word . - gdt_start - 1
    .long gdt_start + 0x10000

/* Metadata area (at the end of binary, to be patched) */
.org 496
kernel_lba:     .long 0
kernel_sectors: .long 0
kernel_entry:   .long 0
magic:          .word 0xAA55
