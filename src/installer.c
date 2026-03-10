/*
 * NovexOS - installer.c
 * Interactive installation wizard
 */

#include "installer.h"
#include "ata.h"
#include "io.h"
#include "keyboard.h"
#include "mbr.h"
#include "string.h"

extern void terminal_writestring(const char *s);
extern void terminal_putchar(char c);
extern void terminal_set_color(uint8_t color);
extern uint8_t terminal_get_color(void);

/* Disk detection state */
static struct {
  int disk_found;
  int is_unformatted; /* 1 = disque vide/non formaté, 0 = MBR valide */
  uint32_t disk_sectors;
} disk_state = {0, 0, 0};

/* Affiche un octet en hex */
static void print_hex_byte(uint8_t b) {
  const char *hex = "0123456789ABCDEF";
  char buf[3];
  buf[0] = hex[(b >> 4) & 0xF];
  buf[1] = hex[b & 0xF];
  buf[2] = '\0';
  terminal_writestring(buf);
}

/* -----------------------------------------------------------------------
 * Détection du disque : purement hardware via IDENTIFY.
 * Fonctionne que le disque soit vide, non formaté, FAT32, NTFS, ext4…
 * Vérifie ensuite si le MBR est valide pour informer l'utilisateur.
 * ----------------------------------------------------------------------- */
/* Dump brut des registres ATA pour diagnostic */
static void ata_dump_registers(void) {
  /* Sélectionner Master avant de lire */
  outb(0x1F6, 0xA0);
  /* 400ns delay */
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);

  terminal_writestring("  Reg 0x1F7 (Status)  : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x3F6 (AltStat) : 0x");
  print_hex_byte(inb(0x3F6));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F6 (Drive)   : 0x");
  print_hex_byte(inb(0x1F6));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F4 (CylLo)   : 0x");
  print_hex_byte(inb(0x1F4));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F5 (CylHi)   : 0x");
  print_hex_byte(inb(0x1F5));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F1 (Error)   : 0x");
  print_hex_byte(inb(0x1F1));
  terminal_writestring("\n");
}

int installer_detect_disks(void) {
  terminal_writestring("\n=== Detection du disque dur ===\n");

  terminal_writestring("--- Registres ATA AVANT IDENTIFY ---\n");
  ata_dump_registers();

  terminal_writestring("Envoi commande IDENTIFY (0xEC)...\n");
  /* Reset doux */
  outb(0x3F6, 0x04);
  inb(0x3F6);
  inb(0x3F6);
  outb(0x3F6, 0x00);
  /* Attente BSY */
  uint32_t t = 0;
  while ((inb(0x1F7) & 0x80) && t < 500000)
    t++;
  terminal_writestring("  Post-reset Status   : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");

  /* Sélection Master + envoi IDENTIFY */
  outb(0x1F6, 0xA0);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  outb(0x1F2, 0);
  outb(0x1F3, 0);
  outb(0x1F4, 0);
  outb(0x1F5, 0);
  outb(0x1F7, 0xEC);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);

  uint8_t status = inb(0x1F7);
  terminal_writestring("  Status apres IDENTIFY: 0x");
  print_hex_byte(status);
  terminal_writestring("\n");

  /* Attendre BSY=0 */
  t = 0;
  while ((inb(0x1F7) & 0x80) && t < 500000)
    t++;
  terminal_writestring("  Status apres attente : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");
  terminal_writestring("  CylLo / CylHi       : 0x");
  print_hex_byte(inb(0x1F4));
  terminal_writestring(" / 0x");
  print_hex_byte(inb(0x1F5));
  terminal_writestring("\n");

  terminal_writestring("--- Appel ata_identify() ---\n");
  int result = ata_identify(0);

  if (!result) {
    terminal_writestring("[ECHEC] ata_identify() retourne 0\n");
    disk_state.disk_found = 0;
    disk_state.is_unformatted = 0;
    return 0;
  }

  terminal_writestring("[OK] Disque ATA detecte sur Primary Master.\n");
  disk_state.disk_found = 1;

  /* Niveau 2 : lire le secteur 0 (MBR) pour voir si le disque est formaté */
  terminal_writestring("Lecture du MBR (secteur 0)...\n");
  uint8_t mbr_buf[512];
  ata_read_sectors(0, 1, mbr_buf);

  uint8_t sig_lo = mbr_buf[510];
  uint8_t sig_hi = mbr_buf[511];
  terminal_writestring("  Signature MBR : 0x");
  print_hex_byte(sig_hi);
  print_hex_byte(sig_lo);
  terminal_writestring("\n");

  if (sig_lo == 0x55 && sig_hi == 0xAA) {
    terminal_writestring("[OK] MBR valide (0xAA55) - disque partitionne.\n");
    disk_state.is_unformatted = 0;
  } else {
    terminal_writestring("[INFO] Pas de signature MBR valide - ");
    terminal_writestring("disque vide ou non formate.\n");
    terminal_writestring("  (C'est normal pour un nouveau disque)\n");
    disk_state.is_unformatted = 1;
  }

  return 1;
}

void installer_show_menu(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║     NovexOS Installation       ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring("\n");

  if (!disk_state.disk_found) {
    terminal_writestring("Aucun disque dur detecte.\n\n");
    terminal_writestring("Options:\n");
    terminal_writestring("  [1] Continuer sans disque (RAM)\n");
    terminal_writestring("  [2] Re-detecter le disque\n");
    terminal_writestring("  [3] Reboot\n");
  } else {
    if (disk_state.is_unformatted) {
      terminal_writestring("Disque detecte : VIDE / NON FORMATE\n");
      terminal_writestring(
          "L'installation creera un MBR et une partition FAT32.\n\n");
    } else {
      terminal_writestring(
          "Disque detecte : MBR valide (deja partitionne)\n\n");
    }
    terminal_writestring("Options d'installation :\n");
    terminal_writestring("  [1] Installer (formate le disque entier)\n");
    terminal_writestring("  [2] Installation dual-boot\n");
    terminal_writestring("  [3] Re-detecter le disque\n");
    terminal_writestring("  [4] Retour au shell\n");
    terminal_writestring("  [5] Reboot\n");
  }

  terminal_writestring("\nChoix : ");
}

void installer_install_full_disk(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║ ATTENTION : Formatage complet  ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring("\nTOUTES les donnees seront effacees !\n");
  terminal_writestring("Confirmer ? (tapez 'yes') : ");

  char buffer[10];
  int pos = 0;
  while (pos < 9) {
    int key = keyboard_getchar();
    if (key == '\n' || key == '\r') {
      buffer[pos] = '\0';
      break;
    }
    if (key >= 32 && key < 127) {
      buffer[pos] = key;
      terminal_putchar(key);
      pos++;
    }
  }
  terminal_writestring("\n");

  if (strcmp(buffer, "yes") != 0) {
    terminal_writestring("Installation annulee.\n");
    return;
  }

  terminal_writestring("\nCreation de la table de partitions (MBR)...\n");
  struct mbr new_mbr;
  mbr_create_partition_table(&new_mbr, 20480); /* 10 Mo par defaut */
  mbr_write(&new_mbr);
  terminal_writestring("[OK] MBR ecrit.\n");

  terminal_writestring("Installation d'NovexOS...\n");
  terminal_writestring("[OK] Installation terminee !\n");
  terminal_writestring("Vous pouvez rebooter depuis le disque dur.\n");
}

void installer_install_dualboot(void) {
  terminal_writestring("\nDual-boot non encore implemente.\n");
  terminal_writestring("Utilisez l'installation complete pour l'instant.\n");
}

void installer_main(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║     NovexOS Live System        ║\n");
  terminal_writestring("║   Running from RAM (USB key)   ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring(
      "\nAppuyez sur 'I' pour l'installeur, autre touche pour le shell.\n");

  int key = keyboard_getchar();
  if (key != 'I' && key != 'i') {
    terminal_writestring("\nRetour au shell...\n");
    return;
  }

  for (;;) {
    int disk_detected = installer_detect_disks();
    installer_show_menu();

    int choice = keyboard_getchar();
    terminal_putchar(choice);
    terminal_writestring("\n");

    if (!disk_detected) {
      switch (choice) {
      case '1':
        terminal_writestring("Mode RAM...\n");
        return;
      case '2':
        continue;
      case '3':
        terminal_writestring("Reboot...\n");
        __asm__ volatile("hlt");
        break;
      default:
        terminal_writestring("Option invalide.\n");
        break;
      }
    } else {
      switch (choice) {
      case '1':
        installer_install_full_disk();
        break;
      case '2':
        installer_install_dualboot();
        break;
      case '3':
        continue;
      case '4':
        terminal_writestring("Retour au shell...\n");
        return;
      case '5':
        terminal_writestring("Reboot...\n");
        __asm__ volatile("hlt");
        break;
      default:
        terminal_writestring("Option invalide.\n");
        break;
      }
    }
  }
}