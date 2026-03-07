/*
 * OmegaOS - ata.h
 * ATA (Advanced Technology Attachment) Driver.
 * Supports PIO (Programmed I/O) mode for reading/writing sectors.
 */

#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ATA Bus I/O Ports (Primary Bus) */
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_PRIMARY_IRQ 14

/* Drive selections */
#define ATA_MASTER 0xA0
#define ATA_SLAVE 0xB0

/* Initialize ATA driver */
void ata_init(void);

/* Identify drive (returns 1 if found, 0 if not) */
int ata_identify(int drive);

/* Read sectors (LBA28) */
void ata_read_sectors(uint32_t lba, uint8_t count, uint8_t *buffer);

/* Write sectors (LBA28) */
void ata_write_sectors(uint32_t lba, uint8_t count, uint8_t *buffer);

#endif /* ATA_H */
