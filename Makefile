# ============================================================
# NovexOS - Makefile
# ============================================================

# --- Toolchain ---
# Cherche x86_64-elf-gcc, sinon utilise le GCC système
IS_CROSS := $(shell command -v x86_64-elf-gcc 2>/dev/null && echo yes || echo no)

ifeq ($(IS_CROSS),yes)
	CC       := x86_64-elf-gcc
	AS       := x86_64-elf-gcc   # on passe par gcc pour le préprocesseur C
	LD       := x86_64-elf-ld
	OBJCOPY  := x86_64-elf-objcopy
	CFLAGS   := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Werror -Isrc \
	             -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2
	LDFLAGS  := -T linker.ld -melf_x86_64 -n
	ASFLAGS  := -x assembler-with-cpp -c -m64
else
	CC       := gcc
	AS       := gcc              # on passe par gcc pour le préprocesseur C
	LD       := ld
	OBJCOPY  := objcopy
	CFLAGS   := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Werror -Isrc \
	             -m64 -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2
	LDFLAGS  := -T linker.ld -melf_x86_64 -n
	ASFLAGS  := -x assembler-with-cpp -c -m64
endif

# --- Répertoires et fichiers source ---
SRCDIR  := src
OBJDIR  := build

# Fichiers assembleur du kernel (passent par gcc pour les #define)
ASM_SRC := $(SRCDIR)/boot.s $(SRCDIR)/interrupts.s
C_SRC   := $(wildcard $(SRCDIR)/*.c)

ASM_OBJ  := $(OBJDIR)/boot.o $(OBJDIR)/interrupts.o
C_OBJ    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(C_SRC))
# loader_bin.o est généré par objcopy depuis le binaire 16-bit du loader
LOADER_OBJ := $(OBJDIR)/loader_bin.o
OBJECTS  := $(ASM_OBJ) $(C_OBJ) $(LOADER_OBJ)

# --- Fichiers de sortie ---
KERNEL   := novexos.bin
ISO      := novexos.iso
DISK_IMG := disk.img

# ============================================================
# Cible par défaut
# ============================================================
all: $(KERNEL)
	@echo ""
	@echo "========================================="
	@echo " NovexOS compiled : $(KERNEL)"
	@echo " Launch with : make run"
	@echo "========================================="

# ============================================================
# Édition de liens du kernel principal
# ============================================================
$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

# ============================================================
# Assemblage des fichiers .s du kernel (via gcc pour les #define)
# ============================================================
$(OBJDIR)/boot.o: $(SRCDIR)/boot.s | $(OBJDIR)/.created
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/interrupts.o: $(SRCDIR)/interrupts.s | $(OBJDIR)/.created
	$(AS) $(ASFLAGS) $< -o $@

# ============================================================
# Compilation des fichiers .c
# ============================================================
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)/.created
	$(CC) -c $< -o $@ $(CFLAGS)

# ============================================================
# Build du loader 16-bit (real mode → binaire plat)
#
# 1. Assembler loader.s en ELF avec as (16-bit, pas de CPP)
# 2. Linker en binaire plat démarrant à 0x0
# 3. Convertir le .bin en .o (symboles _binary_..._start/end/size)
# ============================================================
$(OBJDIR)/loader.o: $(SRCDIR)/loader.s | $(OBJDIR)/.created
	as --32 $< -o $@

$(OBJDIR)/loader.elf: $(OBJDIR)/loader.o
	ld -m elf_i386 -Ttext=0x0 $< -o $@

$(OBJDIR)/loader.bin: $(OBJDIR)/loader.elf
	$(OBJCOPY) -O binary $< $@

# objcopy embed : génère les symboles que shell.c référence :
#   _binary_build_loader_bin_start
#   _binary_build_loader_bin_end
#   _binary_build_loader_bin_size
$(OBJDIR)/loader_bin.o: $(OBJDIR)/loader.bin
	$(OBJCOPY) \
		-I binary \
		-O elf64-x86-64 \
		-B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$< $@

# ============================================================
# Création du répertoire build (sentinelle pour éviter le conflit
# de noms avec la cible phony "build")
# ============================================================
$(OBJDIR)/.created:
	mkdir -p $(OBJDIR)
	@touch $@

# ============================================================
# Création de l'image disque vide (200 Mo)
# ============================================================
$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=1M count=200

# ============================================================
# make run — Compile, génère l'ISO et lance QEMU
# L'ISO est un hybrid MBR (flashable sur clé USB avec dd)
# ============================================================
run: $(ISO) $(DISK_IMG)
	@echo ""
	@echo ">>> Lancement de NovexOS dans QEMU..."
	@echo ">>> (Quitter : fermer la fenêtre ou Ctrl+C dans le terminal)"
	@echo ""
	qemu-system-x86_64 \
		-drive file=$(ISO),format=raw,index=1,media=cdrom \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk \
		-boot d \
		-m 512M \
		-device VGA,vgamem_mb=16 \
		-serial stdio \
		-name "NovexOS"

# ============================================================
# Création de l'ISO bootable (nécessite grub-mkrescue + xorriso)
# grub-mkrescue génère une ISO hybride MBR → flashable sur USB
# ============================================================
$(ISO): $(KERNEL)
	@mkdir -p isodir/boot/grub
	@cp $(KERNEL) isodir/boot/novexos.bin
	@printf 'set timeout=3\nset default=0\n\nmenuentry "NovexOS" {\n    multiboot /boot/novexos.bin\n    boot\n}\n' \
		> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir 2>/dev/null
	@rm -rf isodir
	@echo ""
	@echo "========================================="
	@echo " ISO créée : $(ISO)"
	@echo " Flasher sur USB :"
	@echo "   sudo dd if=$(ISO) of=/dev/sdX bs=4M status=progress"
	@echo "========================================="

# ============================================================
# make build — Compile le kernel et génère l'ISO
# ============================================================
build: $(ISO)
	@echo ""
	@echo "========================================="
	@echo " Build terminé : $(ISO)"
	@echo " Lancer avec   : make run"
	@echo "========================================="

# ============================================================
# make clean — Supprime tout ce qui est généré
# ============================================================
clean:
	@rm -rf $(OBJDIR) $(KERNEL) $(ISO) $(DISK_IMG) isodir
	@echo "Nettoyage terminé."

.PHONY: all build clean run