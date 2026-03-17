# NovexOS

![Version](https://img.shields.io/badge/version-0.7.1--alpha-6C63FF)
![Kernel](https://img.shields.io/badge/kernel-custom%20built-informational)
![License](https://img.shields.io/badge/license-CC%20BY--ND%204.0-red)
![Status](https://img.shields.io/badge/status-in%20development-orange)
![Built by](https://img.shields.io/badge/built%20by-Omega%20Developments-0ea5e9)

A new generation desktop operating system — built from scratch by **Omega Developments**.

[Getting Started](#getting-started) | [Roadmap](#roadmap) | [Contributing](#contributing) | [License](#license)

---

## About NovexOS

NovexOS is a fully independent desktop operating system developed entirely from the ground up by [Omega Developments](https://github.com/omegadevelopments). It is not a distribution, not a fork — it has its own custom kernel, its own memory model, and its own design language.

### Core Philosophy

| Principle              | Description                                                  |
| ---------------------- | ------------------------------------------------------------ |
| **Built from scratch** | Custom kernel, bootloader, drivers — zero external OS base   |
| **Open to use**        | Free to use and integrate, with mandatory attribution        |
| **Performance-first**  | Lightweight architecture with no unnecessary overhead        |
| **Secure by design**   | Memory safety and privilege separation baked in from day one |

---

## Getting Started

### Prerequisites

```bash
# Required
gcc           # C compiler (system GCC or x86_64-elf-gcc cross-compiler)
binutils      # Provides: as (assembler), ld (linker), objcopy
make          # Build system

# Required for make run / make iso
qemu-system-x86_64  # Emulator for testing
grub-mkrescue       # GRUB bootable ISO creation
xorriso             # ISO image backend (used by grub-mkrescue)
```

> **Cross-compiler (optional but recommended):** If `x86_64-elf-gcc` is available, the Makefile will use it automatically. Otherwise it falls back to the system `gcc`. Using a dedicated cross-compiler avoids potential host contamination.

> **Note:** `nasm` is **not** required. All assembly files (`boot.s`, `interrupts.s`, `loader.s`) use GAS syntax and are assembled via `gcc` / `as`.

A Linux build environment is required (macOS and Windows are not currently supported).

### Build from Source (only on Linux for the moment)

```bash
# 1. Clone the repository
git clone https://github.com/omegadevelopmentsfr/NovexOS.git
cd novexOS
```

```bash
# 2. Build the .iso file
make build
```

```bash
# 2.1. Build the .iso file and run it in QEMU
make run
```

```bash
# 2.2. Clean the build (delete it)
make clean
```

> **Alpha software.** NovexOS is in early development and is not suitable for production or daily use yet.

---

## Roadmap

### Done

- [x] Stage 1 & Stage 2 Bootloader
- [x] Kernel entry point (protected mode / long mode)
- [x] Basic VGA text output driver
- [x] GDT & IDT setup
- [x] Physical & virtual memory manager
- [x] Keyboard & PS/2 driver
- [x] Basic shell (NovexShell)
- [x] NovexDE — graphical desktop environment

### In Progress

- [ ] OS installer (NovexInstaller)
- [ ] FAT32, NTFS, Ext4 support

### Planned

- [ ] Process scheduler (round-robin)
- [ ] NovexFS — custom filesystem
- [ ] ELF binary loader
- [ ] Networking stack (TCP/IP)
- [ ] Package manager
- [ ] Multi-core support (SMP)

## Contributing

NovexOS welcomes contributions within the bounds of its license.

```bash
# Fork the repo and create a branch
git checkout -b feat/my-feature

# Make your changes, then submit a Pull Request
```

Please read [CONTRIBUTING.md](./CONTRIBUTING.md) before submitting. All contributors must follow our [Code of Conduct](./CODE_OF_CONDUCT.md).

Areas where help is most needed: kernel & memory management, desktop environment (NovexDE), driver development, and documentation.

---

## Community

- **Website** — [omegadevelopmentsfr.dpdns.org](https://omegadevelopmentsfr.dpdns.org)
- **Bug Reports** — [GitHub Issues](https://github.com/omegadevelopmentsfr/novex-os/issues)
- **Feature Requests** — [GitHub Discussions](https://github.com/omegadevelopmentsfr/novex-os/discussions)

---

## License

NovexOS is licensed under the [Creative Commons Attribution-NoDerivatives 4.0 International (CC BY-ND 4.0)](./LICENSE).

This means:

- You are free to **use and redistribute** NovexOS.
- You are **not allowed to modify** it or create derivative works.
- Any use must clearly credit **Omega Developments** as the original author.

For commercial licensing or special permissions, contact: [omegadevelopmentsfr@gmail.com](mailto:omegadevelopmentsfr@gmail.com)

---

Made with dedication by [Omega Developments](https://github.com/omega-developments)
