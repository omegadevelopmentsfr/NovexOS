/*
 * OmegaOS - linker.ld
 * Linker script optimisé pour le boot physique (x86_64)
 */

ENTRY(_start)

SECTIONS
{
    /* On commence à 1Mo, le standard pour Multiboot */
    . = 1M;

    _kernel_start = .;

    /* * La section Multiboot doit être au tout début du binaire 
     * pour être trouvée par les vieux BIOS/GRUB.
     */
    .multiboot : ALIGN(4096)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4096)
    {
        *(.text)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata)
    }

    .data : ALIGN(4096)
    {
        *(.data)
    }

    /* * _edata marque la fin des données initialisées. 
     * C'est crucial pour le header Multiboot (load_end_addr).
     */
    _edata = .;

    .bss : ALIGN(4096)
    {
        _sbss = .;
        *(COMMON)
        *(.bss)
        . = ALIGN(4096);
        _ebss = .;
    }

    _kernel_end = .;
}