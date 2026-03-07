/*
 * OmegaOS - ata.c
 * ATA Driver - PIO Mode (Polling).
 */

#include "ata.h"
#include "idt.h"
#include "io.h"
#include "string.h"

/* ATA Commands */
#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY 0xEC

/* Status Register Bits */
#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF 0x20
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

/* -----------------------------------------------------------------------
 * Délai 400ns : lire le registre de statut alternatif 4 fois
 * Obligatoire après toute sélection de drive ou envoi de commande.
 * ----------------------------------------------------------------------- */
static void ata_delay400ns(void) {
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
}

/* Attend que BSY soit à 0. Retourne 0 si timeout, 1 si OK.
 * Utilise un compteur brut (pas de timer) — fonctionne même
 * si les interruptions ne sont pas encore activées. */
static int ata_wait_bsy(void) {
  /* ~1 milliard d'itérations ≈ plusieurs secondes sur x86 */
  for (uint32_t i = 0; i < 0x8000000; i++) {
    uint8_t status = inb(ATA_PRIMARY_IO + 7);
    if (status == 0xFF)
      return 0; /* Bus flottant */
    if (!(status & ATA_SR_BSY))
      return 1; /* OK */
  }
  return 0; /* Timeout */
}

/* Attend que DRQ soit à 1 (données prêtes). */
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
 * ata_init : soft-reset du contrôleur pour garantir un état connu.
 * Sans ça, si le BIOS ou le loader a laissé une commande en cours,
 * toutes les détections échouent.
 * ----------------------------------------------------------------------- */
void ata_init(void) {
  /* SRST = bit 2 du Device Control Register (0x3F6) */
  outb(ATA_PRIMARY_CTRL, 0x04); /* Assert SRST */
  /* Attendre au moins 5µs — quelques lectures suffisent */
  inb(ATA_PRIMARY_CTRL);
  inb(ATA_PRIMARY_CTRL);
  outb(ATA_PRIMARY_CTRL,
       0x00); /* Relâcher SRST, interruptions désactivées (nIEN=0) */
  /* Attendre que le contrôleur finisse son reset : jusqu'à 31ms */
  ata_wait_bsy();
}

/* -----------------------------------------------------------------------
 * ata_identify : détecte un drive ATA (formaté ou non).
 *
 * La commande IDENTIFY est purement hardware — elle ne lit pas le
 * contenu du disque, seulement les métadonnées du contrôleur.
 * Un disque vide/non formaté est donc détecté exactement comme
 * un disque avec un système de fichiers.
 *
 * Retourne 1 si un drive ATA est présent, 0 sinon.
 * ----------------------------------------------------------------------- */
int ata_identify(int drive) {
  uint8_t drive_select = (drive == 0) ? ATA_MASTER : ATA_SLAVE;

  /* 1. Sélectionner le drive */
  outb(ATA_PRIMARY_IO + 6, drive_select);
  ata_delay400ns(); /* spec ATA §9.7 : obligatoire après sélection */

  /* 2. Vérifier que le bus n'est pas flottant */
  uint8_t status = inb(ATA_PRIMARY_IO + 7);
  if (status == 0xFF)
    return 0; /* Aucun drive sur ce bus */

  /* 3. Attendre que le drive soit prêt (pas BSY) */
  if (!ata_wait_bsy())
    return 0;

  /* 4. Remettre les registres de cylindre à 0 (requis pour IDENTIFY ATA) */
  outb(ATA_PRIMARY_IO + 2, 0);
  outb(ATA_PRIMARY_IO + 3, 0);
  outb(ATA_PRIMARY_IO + 4, 0);
  outb(ATA_PRIMARY_IO + 5, 0);

  /* 5. Envoyer IDENTIFY */
  outb(ATA_PRIMARY_IO + 7, ATA_CMD_IDENTIFY);
  ata_delay400ns();

  /* 6. Lire le statut : 0x00 = aucun drive, 0xFF = bus flottant */
  status = inb(ATA_PRIMARY_IO + 7);
  if (status == 0x00 || status == 0xFF)
    return 0;

  /* 7. Attendre la fin du BSY */
  if (!ata_wait_bsy())
    return 0;

  /* 8. Vérifier les registres LBA mid/high :
   *    Un drive ATAPI (CD-ROM) met 0x14/0xEB → on rejette.
   *    Un drive ATA met 0x00/0x00 → on accepte. */
  uint8_t lba_mid = inb(ATA_PRIMARY_IO + 4);
  uint8_t lba_high = inb(ATA_PRIMARY_IO + 5);
  if ((lba_mid == 0x14 && lba_high == 0xEB) || /* ATAPI */
      (lba_mid == 0x69 && lba_high == 0x96))   /* SATA en mode PI */
    return 0;

  /* 9. Attendre DRQ (données d'identification disponibles) */
  if (!ata_wait_drq())
    return 0;

  /* 10. Lire les 256 words d'identification (obligatoire pour vider le buffer)
   */
  for (int i = 0; i < 256; i++) {
    uint16_t tmp;
    __asm__ volatile("inw %1, %0" : "=a"(tmp) : "Nd"((uint16_t)ATA_PRIMARY_IO));
    (void)tmp;
  }

  return 1; /* Drive ATA trouvé (formaté ou non) */
}

/* -----------------------------------------------------------------------
 * ata_read_sectors : lecture LBA28 en mode PIO
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
 * ata_write_sectors : écriture LBA28 en mode PIO
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
      outw(ATA_PRIMARY_IO, buf16[j]);
    }
    /* Cache flush après l'écriture */
    outb(ATA_PRIMARY_IO + 7, 0xE7);
    ata_wait_bsy();
  }
}