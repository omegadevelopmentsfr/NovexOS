/*
 * NovexOS - vbe.c
 * Bochs VBE (BGA) framebuffer driver.
 * Uses PCI to find the framebuffer address and BGA I/O ports
 * to set the graphics mode on-demand.
 */

#include "vbe.h"
#include "io.h"
#include "string.h"

extern int chosen_res_id;

/* BGA I/O Ports */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF

/* BGA Registers */
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_BANK 5
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9

/* BGA specific values */
#define VBE_DISPI_ID4 0xB0C4
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80

/* PCI Configuration Ports */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t *fb_ptr = NULL;
static uint32_t fb_width = 1024;
static uint32_t fb_height = 768;
static uint32_t fb_pitch = 1024 * 4; /* 32bpp */
static uint32_t fb_bpp = 32;
static int fb_ready = 0;

/* External page-table symbols from boot.s */
extern char pml4_table[];
extern char pdpt_table[];

static void vbe_write(uint16_t index, uint16_t value) {
  outw(VBE_DISPI_IOPORT_INDEX, index);
  outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
  outw(VBE_DISPI_IOPORT_INDEX, index);
  return inw(VBE_DISPI_IOPORT_DATA);
}

/*
 * Read from PCI configuration space.
 * bus (8 bits), slot (5 bits), func (3 bits), offset (8 bits).
 */
static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t offset) {
  uint32_t address;
  uint32_t lbus = (uint32_t)bus;
  uint32_t lslot = (uint32_t)slot;
  uint32_t lfunc = (uint32_t)func;

  /* Create config address (bit 31 is enable bit) */
  address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) |
                       (offset & 0xFC) | ((uint32_t)0x80000000));

  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

/* Find the Bochs VGA PCI device and extract its BAR0 (framebuffer address) */
static uint32_t pci_get_bga_framebuffer(void) {
  /* Search all buses and slots for the QEMU/Bochs VGA adapter */
  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      uint32_t vendor_device = pci_read_config((uint8_t)bus, slot, 0, 0);
      /* Vendor 0x1234 (QEMU), Device 0x1111 (VGA) or Vendor 0x15AD (VMware SVGA
       * fallback) */
      if (vendor_device == 0x11111234 || vendor_device == 0x040515AD) {
        /* Read BAR0 (offset 0x10) */
        uint32_t bar0 = pci_read_config((uint8_t)bus, slot, 0, 0x10);
        /* The address is in the upper bits, lower 4 bits are flags */
        return bar0 & 0xFFFFFFF0;
      }
    }
  }
  return 0;
}

void vbe_map_framebuffer(uint64_t phys_addr, uint64_t size) {
  static uint64_t fb_pd_table[512] __attribute__((aligned(4096)));
  uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_table;
  uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_table;

  uint64_t pdpt_index = (phys_addr >> 30) & 0x1FF;
  uint64_t pd_start = (phys_addr >> 21) & 0x1FF;
  uint64_t num_pages = (size + 0x1FFFFF) >> 21;

  /* Determine which Page Directory to use */
  uint64_t *target_pd = NULL;

  if (pdpt[pdpt_index] & 0x01) {
    /* PDPT entry exists, use its PD table (mask flags to get address) */
    target_pd = (uint64_t *)(pdpt[pdpt_index] & ~((uint64_t)0xFFF));
  } else {
    /* Make a new PD table */
    memset(fb_pd_table, 0, sizeof(fb_pd_table));
    target_pd = fb_pd_table;
    pdpt[pdpt_index] = ((uint64_t)(uintptr_t)target_pd) | 0x03;
  }

  /* Identity map using 2MB pages */
  uint64_t base = phys_addr & ~((uint64_t)0x1FFFFF);
  for (uint64_t i = 0; i < num_pages && (pd_start + i) < 512; i++) {
    target_pd[pd_start + i] = (base + i * 0x200000) | 0x83;
  }

  /* Ensure PML4[0] points to our PDPT (already done in boot.s, but let's be
   * safe) */
  pml4[0] = ((uint64_t)(uintptr_t)pdpt) | 0x03;

  /* Flush TLB */
  __asm__ volatile("mov %%cr3, %%rax\n"
                   "mov %%rax, %%cr3\n" ::
                       : "rax", "memory");
}

void vbe_init(void *multiboot_info) {
  (void)multiboot_info; /* Unused now, we use BGA */

  serial_print("[VBE] Initializing BGA PCI...\r\n");

  /* Verify BGA is present */
  uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
  if (id < 0xB0C0 || id > 0xB0C5) {
    serial_print("[VBE] BGA device NOT found!\r\n");
    fb_ready = 0;
    return;
  }

  /* Target resolutions to try (from best to fallback) */
  uint32_t try_w[] = {1920, 1600, 1280, 1024};
  uint32_t try_h[] = {1080, 900, 720, 768};
  int res_count = 4;
  int found = 0;

  int start_idx = 0;
  if (chosen_res_id == 1)
    start_idx = 3;
  else if (chosen_res_id == 2)
    start_idx = 2;

  for (int i = start_idx; i < res_count; i++) {
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0); /* Disable to configure */
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)try_w[i]);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)try_h[i]);
    vbe_write(VBE_DISPI_INDEX_BPP, 32);
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0x01 | VBE_DISPI_LFB_ENABLED);

    uint16_t act_w = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t act_h = vbe_read(VBE_DISPI_INDEX_YRES);

    if (act_w == try_w[i] && act_h == try_h[i]) {
      fb_width = try_w[i];
      fb_height = try_h[i];
      fb_pitch = try_w[i] * 4;
      found = 1;
      serial_print("[VBE] Set resolution to ");
      serial_print_hex(try_w[i]);
      serial_print("x");
      serial_print_hex(try_h[i]);
      serial_print("\r\n");
      break;
    }
  }

  if (!found) {
    /* Hard fallback */
    fb_width = 1024;
    fb_height = 768;
    fb_pitch = 1024 * 4;
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);
    vbe_write(VBE_DISPI_INDEX_XRES, 1024);
    vbe_write(VBE_DISPI_INDEX_YRES, 768);
    vbe_write(VBE_DISPI_INDEX_BPP, 32);
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0x01 | VBE_DISPI_LFB_ENABLED);
    serial_print("[VBE] Fallback resolution 1024x768 applied.\r\n");
  }

  /* Get physical framebuffer address via PCI */
  uint32_t fb_phys = pci_get_bga_framebuffer();
  serial_print("[VBE] PCI Framebuffer addr: ");
  serial_print_hex(fb_phys);
  serial_print("\r\n");

  if (fb_phys == 0) {
    serial_print(
        "[VBE] Warning: PCI scan failed, falling back to 0xFD000000\r\n");
    fb_phys = 0xFD000000;
  }

  /* Map the framebuffer */
  uint64_t fb_size = (uint64_t)fb_width * fb_height * 4;
  vbe_map_framebuffer(fb_phys, fb_size);

  fb_ptr = (uint32_t *)(uintptr_t)fb_phys;

  serial_print("[VBE] Graphics mode enabled.\r\n");

  fb_ready = 1;
}

uint32_t *vbe_get_framebuffer(void) { return fb_ptr; }
uint32_t vbe_get_width(void) { return fb_width; }
uint32_t vbe_get_height(void) { return fb_height; }
uint32_t vbe_get_pitch(void) { return fb_pitch; }
uint32_t vbe_get_bpp(void) { return fb_bpp; }
int vbe_is_available(void) { return fb_ready; }
