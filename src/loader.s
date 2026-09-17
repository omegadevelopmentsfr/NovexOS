.code16
.section .text
.global _start

/*
 * Stage 2 Loader (V26 — Guaranteed Handshake Edition)
 * Fixed offsets at the start of the binary for 100% reliable patching.
 */

_start:
    jmp bootstrap

# --- FIXED PATCH AREA (Starts at offset 8) ---
# Each field is 4 bytes. Total header size = 16 bytes.
.org 8
kernel_lba:     .long 0   # Offset 8
kernel_sectors: .long 0   # Offset 12
kernel_entry:   .long 0   # Offset 16
magic_patch:    .word 0xAA55 # Offset 20

bootstrap:
    cli
    # 1. Setup Segments
    mov $0x1000, %ax; mov %ax, %ds
    mov %dl, (drive_id)
    mov $0x9000, %ax; mov %ax, %ss; mov $0xFFFF, %sp
    
    # 2. Main Diagnostic Dashboard
    mov $0xB800, %ax; mov %ax, %es; xor %di, %di
    mov $2000, %cx; movw $0x1F20, %ax; rep stosw
    mov $0xB800, %ax; mov %ax, %gs
    
    # Status '2' (Starting Stage 2)
    movw $0x1F32, %gs:(0) 

    # 3. Enable A20 & Check
    call .enable_a20; call .check_a20
    test %ax, %ax; jnz .a20_ok
    movw $0x4F41, %gs:(0); cli; hlt; jmp .
.a20_ok:
    movw $0x1F41, %gs:(2); call .enter_unreal
    movw $0x1F55, %gs:(4) # 'U' (Unreal Mode)
    
    # 4. LOAD KERNEL
    movw $0x1F4C, %gs:(6) # 'L' (Loading)

    # Diagnostic Trace: Show LBA at top right. FIXED: .to_hex clobbers %al
    # in place, so the running value being shifted must live in a register
    # .to_hex never touches (%ebx here), not in %eax itself -- shifting
    # %eax after to_hex() corrupted it with the ASCII digit instead of the
    # remaining nibbles, which is what produced garbage like "33333330"
    # for every trace in earlier debug builds.
    mov (kernel_lba), %ebx
    push %ebx
    mov $44, %di
    mov $8, %cx
.tr_lba:
    mov %bl, %al
    call .to_hex
    mov %al, %gs:(%di)
    sub $2, %di
    shr $4, %ebx
    loop .tr_lba
    pop %ebx

    test %ebx, %ebx
    jnz .load_begin
    # Red 'P' if LBA is 0 (Patch Failed)
    movw $0x4F50, %gs:(0); cli; hlt; jmp .

.load_begin:
    mov (kernel_lba), %eax
    mov (kernel_sectors), %cx
    movl $0x100000, %edi
.load_loop:
    push %eax
    push %cx
    push %edi

    # Read 1 sector into 0x2000:0x0000
    mov $0x2000, %bx
    mov %bx, %es
    xor %bx, %bx
    call .read_sector
    jc .disk_err

    # Copy 512 bytes (128 dwords) from ES:0 to FS:EDI
    xor %esi, %esi
    mov $128, %cx
.copy:
    movl %es:(%esi), %edx
    addr32 movl %edx, %fs:(%edi)
    add $4, %esi
    add $4, %edi
    loop .copy

    pop %edi
    addl $512, %edi
    pop %cx
    pop %eax
    inc %eax
    dec %cx
    jnz .load_loop

    # 5. VERIFY KERNEL MAGIC
    movl $0x100000, %ebx
    addr32 movl %fs:(%ebx), %eax
    cmp $0x1BADB002, %eax
    je .magic_ok

    # Show the ACTUAL magic value read, at row 2, columns 0-7, so we can
    # see what's really at 0x100000 instead of the expected 0x1BADB002.
    push %eax
    mov %eax, %ebx
    mov $166, %di
    mov $8, %cx
.tr_magic:
    mov %bl, %al
    call .to_hex
    mov %al, %gs:(%di)
    sub $2, %di
    shr $4, %ebx
    loop .tr_magic
    pop %eax

    # Red 'M' (Mismatch)
    movw $0x4F4D, %gs:(8); cli; hlt; jmp .

.disk_err:
    # Red 'D' (Disk Read Error)
    movw $0x4F44, %gs:(8); cli; hlt; jmp .

.magic_ok:
    movw $0x2F4D, %gs:(8); movw $0x2F4B, %gs:(10) # Success M, K

    # 6. Final Handover
    xor %ax, %ax; mov %ax, %es; mov $0x9500, %di
    movl $0x00000001, %es:(%di); movl $640, %es:4(%di); movl $65536, %es:8(%di)

    cli; lgdt (gdt_ptr)
    mov %cr0, %eax; or $1, %eax; mov %eax, %cr0
    .byte 0x66, 0xEA; .long pm_entry + 0x10000; .word 0x08

# --- HELPERS ---
.enable_a20:
    mov $0x2401, %ax; int $0x15; in $0x92, %al; or $0x02, %al; out %al, $0x92; ret
.check_a20:
    push %ds; push %es; xor %ax, %ax; mov %ax, %ds; not %ax; mov %ax, %es
    mov $0x0500, %di; mov $0x0510, %si; mov %ds:(%di), %al; push %ax
    mov %es:(%si), %al; push %ax; movb $0x00, %ds:(%di); movb $0xFF, %es:(%si)
    cmpb $0xFF, %ds:(%di); pop %ax; mov %al, %es:(%si); pop %ax; mov %al, %ds:(%di)
    mov $0, %ax; je 1f; mov $1, %ax
1:  pop %es; pop %ds; ret
.enter_unreal:
    lgdt (gdt_ptr_unreal); mov %cr0, %eax; or $1, %eax; mov %eax, %cr0
    jmp 1f
1:  mov $0x10, %ax; mov %ax, %fs; mov %cr0, %eax; and $0xFFFFFFFE, %eax; mov %eax, %cr0
    jmp 2f
2:  sti; ret
.read_sector:
    # CRITICAL: %di must be preserved -- the caller (.load_loop) uses
    # %edi as its 32-bit destination pointer, and %di is %edi's low 16
    # bits. Without saving/restoring it here, "mov %di, (dap_segment)"
    # below clobbers the caller's destination pointer with whatever ES
    # currently is (0x2000), silently corrupting every single sector
    # copy to land at the same wrong physical address instead of
    # advancing through memory.
    push %eax; push %bx; push %cx; push %dx; push %si; push %di
    movb $0x10, (dap); movb $0, (dap+1); movw $1, (dap_count)
    mov %bx, (dap_offset); mov %es, %di; mov %di, (dap_segment)
    mov %ax, (dap_lba); shr $16, %eax; mov %ax, (dap_lba+2); movl $0, (dap_lba+4)
    mov (drive_id), %dl; mov $0x42, %ah; mov $dap, %si; int $0x13
    pop %di; pop %si; pop %dx; pop %cx; pop %bx; pop %eax; ret
.to_hex:
    and $0x0F, %al; add $0x30, %al; cmp $0x39, %al; jbe 1f; add $7, %al
1:  ret

# --- DATA ---
.align 16
drive_id: .byte 0
dap:      .byte 0x10, 0
dap_count: .word 0
dap_offset: .word 0
dap_segment: .word 0
dap_lba:     .quad 0
.align 16
gdt_unreal: .quad 0; .quad 0x00CF9A000000FFFF; .quad 0x00CF92000000FFFF
gdt_ptr_unreal: .word 23; .long gdt_unreal + 0x10000
gdt_pm: .quad 0; .quad 0x00CF9A000000FFFF; .quad 0x00CF92000000FFFF
gdt_ptr: .word 23; .long gdt_pm + 0x10000

.code32
pm_entry:
    mov $0x10, %ax; mov %ax, %ds; mov %ax, %es; mov %ax, %ss
    mov $0x2BADB002, %eax; mov $0x9500, %ebx
    # Fixed offset 16 for entry point
    mov (0x10010), %ecx 
    jmp *%ecx