/*
 * OmegaOS - mbr.h
 * Master Boot Record (MBR) parser.
 */

#ifndef MBR_H
#define MBR_H

#include "types.h"

/* MBR Partition Entry (16 bytes) */
struct mbr_partition_entry {
  uint8_t status; /* 0x80 = bootable, 0x00 = inactive */
  uint8_t chs_start[3];
  uint8_t type; /* 0x01=FAT12, 0x04=FAT16<32M, 0x06=FAT16 */
  uint8_t chs_end[3];
  uint32_t lba_start;    /* LBA of partition start */
  uint32_t sector_count; /* Number of sectors */
} __attribute__((packed));

/* MBR Structure (512 bytes) */
struct mbr {
  uint8_t bootstrap[446];
  struct mbr_partition_entry partitions[4];
  uint16_t signature; /* 0x55AA */
} __attribute__((packed));

/* Read MBR and return the LBA start of the first valid FAT16 partition */
/* Returns 0 if no valid partition found */
uint32_t mbr_find_partition(void);

/* Create a new MBR with a single FAT32 partition */
void mbr_create_partition_table(struct mbr *m, uint32_t sector_count);

/* Write MBR to disk */
void mbr_write(struct mbr *m);

#endif /* MBR_H */
