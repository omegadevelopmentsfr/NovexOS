/*
 * OmegaOS - installer.h
 * Interactive installation wizard for disk/partition management
 */

#ifndef INSTALLER_H
#define INSTALLER_H

#include "types.h"

/* Disk detection and installation */
void installer_main(void);

/* Detect available ATA disks */
int installer_detect_disks(void);

/* Show installation menu */
void installer_show_menu(void);

/* Install to disk */
void installer_install_os(void);

#endif /* INSTALLER_H */
