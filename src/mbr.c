/*
 * NovexOS - mbr.c
 * MBR Parser.
 * Scans the Master Boot Record to find a valid FAT16 partition.
 */

#include "mbr.h"
#include "ata.h"
#include "string.h"

static struct mbr disk_mbr;

uint32_t mbr_find_partition(void) {
  /* Read LBA 0 (MBR) */
  ata_read_sectors(0, 1, (uint8_t *)&disk_mbr);

  /* Check signature */
  if (disk_mbr.signature != 0xAA55) {
    return 0; /* Invalid MBR */
  }

  /* Scan partitions */
  for (int i = 0; i < 4; i++) {
    uint8_t type = disk_mbr.partitions[i].type;
    /* Check for FAT16 (04, 06, 0E) or FAT32 (0B, 0C) */
    if (type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B ||
        type == 0x0C) {
      return disk_mbr.partitions[i].lba_start;
    }
  }

  return 0; /* No FAT16 partition found */
}

void mbr_create_partition_table(struct mbr *m, uint32_t sector_count) {
  memset(m, 0, sizeof(struct mbr));

  /* Stage 1 Bootstrap: Loads Stage 2 (LBA 1, 8 sectors) to 0x1000:0000 */
  uint8_t boot_code[446];
  memset(boot_code, 0, 446);
  uint8_t code[] = {
      0xFA,                   /* cli */
      0x31, 0xC0, 0x8E, 0xD8, /* xor ax,ax; mov ds,ax */
      0x8E, 0xD0,             /* mov ss,ax */
      0xBC, 0x00, 0x7C,       /* mov sp, 0x7C00 */
      0xFB, 0xFC,             /* sti; cld */

      /* Save drive ID at offset 428 (0x1AC) -> addr 0x7DAC */
      0x88, 0x16, 0xAC, 0x7D,            /* mov [0x7DAC], dl */
      0xB4, 0x0E, 0xB0, '1', 0xCD, 0x10, /* Print '1' */

      /* Check Extensions */
      0xBB, 0xAA, 0x55, /* mov bx, 0x55AA */
      0xB4, 0x41,       /* mov ah, 0x41 */
      0xCD, 0x13,       /* int 13h */
      0x72, 0x14,       /* jc error */

      /* Read Stage 2 (LBA 1, 8 sectors) to 0x1000:0000 */
      0xB4, 0x42,             /* ah = 42h */
      0x8A, 0x16, 0xAC, 0x7D, /* dl = [0x7DAC] */
      0xBE, 0xAE, 0x7D,       /* si = 0x7DAE (DAP at offset 430) */
      0xCD, 0x13,             /* int 13h */
      0x72, 0x05,             /* jc error */

      /* Success: Jump to Stage 2 */
      0xB4, 0x0E, 0xB0, '2', 0xCD, 0x10, /* Print '2' */
      0xEA, 0x00, 0x00, 0x00, 0x10,      /* jmp 0x1000:0000 */

      /* Error loop */
      0xB4, 0x0E, 0xB0, 'E', 0xCD, 0x10, /* Print 'E' */
      0xFB, 0xF4, 0xEB, 0xFD             /* sti; hlt; jmp -3 */
  };
  memcpy(boot_code, code, sizeof(code));

  /* Disk Address Packet (DAP) at offset 430 (0x1AE) */
  uint8_t dap[] = {
      0x10, 0x00,             /* Size (16), Reserved */
      0x08, 0x00,             /* Count (8 sectors) */
      0x00, 0x00,             /* Offset (0) */
      0x00, 0x10,             /* Segment (0x1000) */
      0x01, 0x00, 0x00, 0x00, /* LBA Start (1) */
      0x00, 0x00, 0x00, 0x00  /* LBA High */
  };
  memcpy(boot_code + 430, dap, sizeof(dap));

  memcpy(m->bootstrap, boot_code, 446);

  /* Primary Partition 1: FAT32 */
  m->partitions[0].status = 0x80; /* Bootable */
  m->partitions[0].type = 0x0B;   /* FAT32 LBA */
  m->partitions[0].lba_start = 2048;
  m->partitions[0].sector_count = sector_count - 2048;

  m->signature = 0xAA55;
}

void mbr_write(struct mbr *m) { ata_write_sectors(0, 1, (uint8_t *)m); }
