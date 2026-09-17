/*
 * NovexOS - mbr.c
 * Focused on debug and extreme compatibility.
 */

#include "mbr.h"
#include "ata.h"
#include "string.h"

static struct mbr disk_mbr;

uint32_t mbr_find_partition(void) {
  ata_read_sectors(0, 1, (uint8_t *)&disk_mbr);
  if (disk_mbr.signature != 0xAA55)
    return 0;
  for (int i = 0; i < 4; i++) {
    uint8_t type = disk_mbr.partitions[i].type;
    /* FAT variants: 0x04=FAT16<32M, 0x06=FAT16, 0x0B/0x0C=FAT32,
     *               0x0E=FAT16 LBA
     * Also accept NTFS (0x07) and Linux (0x83) so that FAT32 overlay
     * partitions can be located even when the MBR type was overridden. */
    if (type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B ||
        type == 0x0C || type == 0x07 || type == 0x83) {
      return disk_mbr.partitions[i].lba_start;
    }
  }
  return 0;
}

void mbr_create_partition_table(struct mbr *m, uint32_t sector_count) {
  memset(m, 0, sizeof(struct mbr));

  /* Stage 1 Bootstrap: Extra verbose diagnostic */
  uint8_t boot_code[446];
  memset(boot_code, 0, 446);

  uint8_t code[] = {
      0xFA,                   /* cli */
      0x31, 0xC0, 0x8E, 0xD8, /* xor ax,ax; mov ds,ax */
      0x8E, 0xD0,             /* mov ss,ax */
      0xBC, 0x00, 0x7C,       /* mov sp, 0x7C00 */
      0xFB, 0xFC,             /* sti; cld */

      0x88, 0x16, 0xA6, 0x7D, /* mov [0x7DA6], dl (Save Drive ID) */

      /* Print '1' (Stage 1 Start) */
      0xB4, 0x0E, 0xB0, '1', 0xCD, 0x10,

      /* Print 'X' (Testing Extensions) */
      0xB0, 'X', 0xCD, 0x10, 0xBB, 0xAA, 0x55, /* mov bx, 55AA */
      0xB4, 0x41,                              /* mov ah, 41 */
      0x8A, 0x16, 0xA6, 0x7D,                  /* reload dl */
      0xCD, 0x13,                              /* int 13h */
      0x72, 0x2A,                              /* jc error */

      /* Print 'R' (Reading) */
      0xB0, 'R', 0xCD, 0x10, 0xB4, 0x42, /* ah = 42h */
      0x8A, 0x16, 0xA6, 0x7D,            /* mov dl, [0x7DA6] */
      0xBE, 0xA8, 0x7D,                  /* si = 0x7DA8 (DAP) */
      0xCD, 0x13,                        /* int 13h */
      0x72, 0x1D,                        /* jc error */

      /* Print '2' (S2 Loaded) */
      0xB4, 0x0E, 0xB0, '2', 0xCD, 0x10,
      /* Print 'J' (Jump imminent) */
      0xB0, 'J', 0xCD, 0x10,

      0x8A, 0x16, 0xA6, 0x7D,       /* mov dl, [0x7DA6] */
      0xEA, 0x00, 0x00, 0x00, 0x10, /* jmp 0x1000:0000 */

      /* Error loop */
      0xB4, 0x0E, 0xB0, 'E', 0xCD, 0x10, /* Print 'E' */
      0xFB, 0xF4, 0xEB, 0xFD             /* sti; hlt; jmp -2 */
  };
  memcpy(boot_code, code, sizeof(code));

  /* Drive ID storage at 0x7DA6 (aligned-2) */
  boot_code[422] = 0x80;

  /* DAP at 0x7DA8 (aligned-8) */
  uint8_t dap[] = {
      0x10, 0x00,             /* Size (16), Reserved */
      0x08, 0x00,             /* Count (8 sectors) */
      0x00, 0x00,             /* Offset (0) */
      0x00, 0x10,             /* Segment (0x1000) */
      0x01, 0x00, 0x00, 0x00, /* LBA Start (1) */
      0x00, 0x00, 0x00, 0x00  /* LBA High */
  };
  memcpy(boot_code + 424, dap, sizeof(dap));

  memcpy(m->bootstrap, boot_code, 446);

  /* Primary Partition 1: FAT32 */
  m->partitions[0].status = 0x80; /* Bootable */
  m->partitions[0].type = 0x0B;   /* FAT32 LBA */
  m->partitions[0].lba_start = 2048;
  m->partitions[0].sector_count = sector_count - 2048;
  m->signature = 0xAA55;
}

void mbr_write(struct mbr *m) { ata_write_sectors(0, 1, (uint8_t *)m); }