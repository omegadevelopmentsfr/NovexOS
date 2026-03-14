/*
 * NovexOS - ata.c
 * ATA Driver - PIO Mode (Polling).
 */

#include "ata.h"
#include "idt.h"
#include "io.h"
#include "string.h"

/* ATA Commands */
#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC

/* Status Register Bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* -----------------------------------------------------------------------
 * 400ns delay: read the alternate status register 4 times.
 * Required after any drive selection or command send.
 * ----------------------------------------------------------------------- */
static void ata_delay400ns(void) {
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
}

/* Wait for BSY to clear. Returns 0 on timeout, 1 on success.
 * Uses a raw counter (no timer) — works even before interrupts are enabled. */
static int ata_wait_bsy(void) {
  /* ~1 billion iterations ≈ several seconds on x86 */
  for (uint32_t i = 0; i < 0x8000000; i++) {
    uint8_t status = inb(ATA_PRIMARY_IO + 7);
    if (status == 0xFF)
      return 0; /* Floating bus */
    if (!(status & ATA_SR_BSY))
      return 1; /* OK */
  }
  return 0; /* Timeout */
}

/* Wait for DRQ to be set (data ready). */
static int ata_wait_drq(void) {
  for (uint32_t i = 0; i < 0x8000000; i++) {
    uint8_t status = inb(ATA_PRIMARY_IO + 7);
    if (status == 0xFF)
      return 0;
    if (status & ATA_SR_ERR)
      return 0;
    if (status & ATA_SR_DRQ)
      return 1;
  }
  return 0; /* Timeout */
}

/* -----------------------------------------------------------------------
 * ata_init: soft-reset the controller to ensure a known state.
 * Without this, if the BIOS or loader left a pending command,
 * all subsequent detections will fail.
 * ----------------------------------------------------------------------- */
void ata_init(void) {
  /* SRST = bit 2 of the Device Control Register (0x3F6) */
  outb(ATA_PRIMARY_CTRL, 0x04); /* Assert SRST */
  /* Wait at least 5µs — a few reads are sufficient */
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  outb(ATA_PRIMARY_CTRL, 0x00); /* Release SRST, interrupts disabled (nIEN=0) */
  /* Wait for the controller to finish its reset: up to 31ms */
  ata_wait_bsy();
}

/* -----------------------------------------------------------------------
 * ata_identify: detect an ATA drive (formatted or not).
 *
 * The IDENTIFY command is purely hardware — it does not read disk content,
 * only controller metadata. An empty/unformatted disk is detected exactly
 * like a disk with a filesystem.
 *
 * Returns 1 if an ATA drive is present, 0 otherwise.
 * ----------------------------------------------------------------------- */
int ata_identify(int drive) {
  uint8_t drive_select = (drive == 0) ? ATA_MASTER : ATA_SLAVE;

  /* 1. Select the drive */
  outb(ATA_PRIMARY_IO + 6, drive_select);
  ata_delay400ns(); /* ATA spec §9.7: required after selection */

  /* 2. Check that the bus is not floating */
  uint8_t status = inb(ATA_PRIMARY_IO + 7);
  if (status == 0xFF)
    return 0; /* No drive on this bus */

  /* 3. Wait for the drive to be ready (not BSY) */
  if (!ata_wait_bsy())
    return 0;

  /* 4. Reset cylinder registers to 0 (required for ATA IDENTIFY) */
  outb(ATA_PRIMARY_IO + 2, 0);
  outb(ATA_PRIMARY_IO + 3, 0);
  outb(ATA_PRIMARY_IO + 4, 0);
  outb(ATA_PRIMARY_IO + 5, 0);

  /* 5. Send IDENTIFY */
  outb(ATA_PRIMARY_IO + 7, ATA_CMD_IDENTIFY);
  ata_delay400ns();

  /* 6. Read status: 0x00 = no drive, 0xFF = floating bus */
  status = inb(ATA_PRIMARY_IO + 7);
  if (status == 0x00 || status == 0xFF)
    return 0;

  /* 7. Wait for BSY to clear */
  if (!ata_wait_bsy())
    return 0;

  /* 8. Check LBA mid/high registers:
   *    An ATAPI drive (CD-ROM) sets 0x14/0xEB -> reject.
   *    An ATA drive sets 0x00/0x00 -> accept. */
  uint8_t lba_mid  = inb(ATA_PRIMARY_IO + 4);
  uint8_t lba_high = inb(ATA_PRIMARY_IO + 5);
  if ((lba_mid == 0x14 && lba_high == 0xEB) || /* ATAPI */
      (lba_mid == 0x69 && lba_high == 0x96))   /* SATA in PI mode */
    return 0;

  /* 9. Wait for DRQ (identification data available) */
  if (!ata_wait_drq())
    return 0;

  /* 10. Read the 256 identification words (required to flush the buffer) */
  for (int i = 0; i < 256; i++) {
    uint16_t tmp;
    __asm__ volatile("inw %1, %0" : "=a"(tmp) : "Nd"((uint16_t)ATA_PRIMARY_IO));
    (void)tmp;
  }

  return 1; /* ATA drive found (formatted or not) */
}

/* -----------------------------------------------------------------------
 * ata_read_sectors: LBA28 read in PIO mode
 * ----------------------------------------------------------------------- */
void ata_read_sectors(uint32_t lba, uint8_t count, uint8_t *buffer) {
  if (!ata_wait_bsy()) {
    for (int i = 0; i < count * 512; i++)
      buffer[i] = 0;
    return;
  }

  /* 0xE0 = Master + LBA mode */
  outb(ATA_PRIMARY_IO + 6, 0xE0 | ((lba >> 24) & 0x0F));
  ata_delay400ns();
  outb(ATA_PRIMARY_IO + 2, count);
  outb(ATA_PRIMARY_IO + 3, (uint8_t)(lba));
  outb(ATA_PRIMARY_IO + 4, (uint8_t)(lba >> 8));
  outb(ATA_PRIMARY_IO + 5, (uint8_t)(lba >> 16));
  outb(ATA_PRIMARY_IO + 7, ATA_CMD_READ_PIO);

  uint16_t *buf16 = (uint16_t *)buffer;
  for (int i = 0; i < count; i++) {
    if (!ata_wait_bsy())
      break;
    if (!ata_wait_drq())
      break;
    for (int j = 0; j < 256; j++) {
      uint16_t tmp;
      __asm__ volatile("inw %1, %0"
                       : "=a"(tmp)
                       : "Nd"((uint16_t)ATA_PRIMARY_IO));
      buf16[i * 256 + j] = tmp;
    }
  }
}

/* -----------------------------------------------------------------------
 * ata_write_sectors: LBA28 write in PIO mode
 * ----------------------------------------------------------------------- */
void ata_write_sectors(uint32_t lba, uint8_t count, uint8_t *buffer) {
  for (uint16_t i = 0; i < count; i++) {
    if (!ata_wait_bsy())
      return;

    outb(ATA_PRIMARY_IO + 6, 0xE0 | (((lba + i) >> 24) & 0x0F));
    ata_delay400ns();
    outb(ATA_PRIMARY_IO + 2, 1);
    outb(ATA_PRIMARY_IO + 3, (uint8_t)(lba + i));
    outb(ATA_PRIMARY_IO + 4, (uint8_t)((lba + i) >> 8));
    outb(ATA_PRIMARY_IO + 5, (uint8_t)((lba + i) >> 16));
    outb(ATA_PRIMARY_IO + 7, ATA_CMD_WRITE_PIO);

    if (!ata_wait_bsy())
      return;
    if (!ata_wait_drq())
      return;

    uint16_t *buf16 = (uint16_t *)(buffer + (i * 512));
    for (int j = 0; j < 256; j++) {
      outw(buf16[j], ATA_PRIMARY_IO);
    }
    /* Cache flush after write */
    outb(ATA_PRIMARY_IO + 7, 0xE7);
    ata_wait_bsy();
  }
}
