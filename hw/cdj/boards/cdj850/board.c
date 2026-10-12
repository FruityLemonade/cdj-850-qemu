/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
/*
 * Pioneer CDJ-850 MAIN board (Renesas SH7263, SH-2A) -- bring-up.
 *
 * The SoC is the SH7263 (SH-2A core, big-endian: this machine lives in the
 * sh4eb target, like the SH7269 GUI board, and runs with -cpu sh2a). The
 * service manual (CDJ-850 service manual, block diagram and parts list)
 * names the parts: IC103 is the MAIN CPU (SH7263), IC101 the MAIN
 * firmware flash (DYW1796, "FLASH ROM(32M)"), IC102 the work SDRAM
 * (K4S561632J-UC75, "SDRAM(256M)"), the bus clock is CLK_66M at 66 MHz for
 * flash and SDRAM, and the crystals are X101 33 MHz (CPU/bus PLL source on
 * EXTAL) and X102 48 MHz (USB clock).
 *
 * Memory map (SH7263 bus state controller areas; SDRAM belongs on CS3 when
 * only one SDRAM space is used, per the SH7262/SH7264 SDRAM app note):
 *   CS0  0x00000000  4 MB NOR flash (DYW1796, 32 Mbit = 4 MB).
 *   CS3  0x0C000000  32 MB SDRAM (K4S561632J, 256 Mbit = 32 MB).
 *   on-chip RAM 0xFFF80000 (SH7263: 64 KB high-speed + 16 KB retention;
 *     the reset SP vector is 0xFFF90000, the top of the 64 KB bank).
 *   on-chip peripherals 0xFFFC0000 up (BSC at 0xFFFC0000, CPG at 0xFFFE0000,
 *     ports at 0xFFFE3800+, all seen as literals in the boot block).
 *
 * Boot: through Pioneer's own bootloader in the NOR flash, the way the real
 * unit starts. make_flash_cdj850.py lays boot.bin at flash 0 and main_flat.bin
 * at 0x60000 (the updater's own offsets, per notes/cdj850-layout.md) in an
 * otherwise-erased 4 MB image. SH-2A power-on reset loads PC from the vector
 * at 0 and SP from the vector at 4 (SH-2A Software Manual §3; SH7263 hardware
 * manual §5.2.3); the image holds PC = 0x00000100, SP = 0xFFF90000, with the
 * manual-reset copies at 8/12. There is no -kernel path: the MAIN region
 * starts with a 32-byte label, not code, and runs in place from flash.
 */

/* DYW1796 32 Mbit = 4 MB: 63 x 64 KiB + 8 x 8 KiB = 4032 + 64 = 4096 KiB.
 * Top-boot shape mirrored from the cdj900's 32 Mbit part; the exact DYW1796
 * sector map is unverified (only erase/program care, and nothing programs
 * yet). */
/* K4S561632J 256 Mbit = 32 MB of 16-bit SDRAM on CS3. */

static const CdjBoardDesc cdj850_board = {
    .name = "cdj850",
    .dram_phys = 0x0C000000,
    .dram_size = 32 * MiB,
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    /* Unverified for the DYW1796 (Spansion 32 Mbit values, as on cdj900). */
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .flash_be = true,
    /* Power-on reset PC vector (flash offset 0); do not copy cdj900's. */
    .fw_entry = 0x00000100,
    /* Power-on reset SP vector (flash offset 4). */
    .init_sp = 0xFFF90000,
    /* 0 keeps the CPU's reset value (MD/RB/BL set, IMASK=15), which matches
     * the SH-2A power-on state (VBR=0, IMASK=F); the bootloader sets SR. */
    .init_sr = 0,
    .sr_seed_slot = 0,
    .sr_seed = 0,
    /* CLK_66M bus clock. Pphi (peripheral clock, 33 MHz max) feeds the timers,
     * not the bus; no timer is modelled yet, so this is currently inert. */
    .periph_hz = 66000000,
    /* No INTEVT trace window (the SH-4 CCN/INTC model does not apply). */
    .ccn_trace_lo = 1,
    .ccn_trace_hi = 0,
    .exit_report = NULL,
};

/* SH-2A vector reset, after the SH7269 GUI board's sh7269_reset: reset the CPU
 * (which leaves SH-4-style pc=0xA0000000) and apply the board's vectors. */
static void cdj850_reset(void *opaque)
{
    SuperHCPU *cpu = opaque;
    CPUSH4State *env = &cpu->env;

    cpu_reset(CPU(cpu));
    env->pc = cdj_board->fw_entry;
    env->gregs[15] = cdj_board->init_sp;
    /* SH-2A has no MMU and runs privileged from reset. */
    env->mmucr = 0;
}

/* On-chip RAM: 256 KB at 0xFFF80000, up to where the peripherals begin at
 * 0xFFFC0000 (same convention as the SH7269 board). Covers the 64 KB
 * high-speed bank the reset stack lives in plus the retention bank. */
#define CDJ850_OCRAM_BASE   0xFFF80000
#define CDJ850_OCRAM_SIZE   (256 * KiB)

static void cdj850_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ocram = g_new(MemoryRegion, 1);
    SuperHCPU *cpu = cdj_board_init(machine, &cdj850_board);
    uint8_t reset_word[2];

    if (machine->kernel_filename) {
        error_report("%s: no -kernel path; boot through the bootloader in "
                     "the NOR flash (MAIN_BOOT=flash, image from "
                     "scripts/firmware/make_flash_cdj850.py)", cdj850_board.name);
        exit(1);
    }
    /* The reset vectors are the first words of the flash; erased there means
     * an image built without the bootloader, and the CPU would run 0xFFFF. */
    cpu_physical_memory_read(cdj850_board.flash_phys, reset_word,
                             sizeof(reset_word));
    if (reset_word[0] == 0xFF && reset_word[1] == 0xFF) {
        error_report("%s: the NOR flash holds no bootloader; build it with "
                     "scripts/firmware/make_flash_cdj850.py from the prepared "
                     "firmware", cdj850_board.name);
        exit(1);
    }

    memory_region_init_ram(ocram, NULL, "cdj850.ocram", CDJ850_OCRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, CDJ850_OCRAM_BASE, ocram);

    /* On-chip peripheral block (BSC/CPG/INTC/DMAC/ports): not modelled yet,
     * so every access is logged rather than silently reading zero. No A7
     * alias: SH-2A has no segments, and an alias could shadow a real area. */
    create_unimplemented_device("sh7263.internal", 0xFFFC0000, 0x40000);
    /* Cache address/data arrays the boot block touches (literals 0xF0000008,
     * 0xF0800008, 0xE0000000): logged, like the GUI board's cache window. */
    create_unimplemented_device("sh7263.cache", 0xF0000000, 0x1000000);
    create_unimplemented_device("sh7263.cache-p0", 0xE0000000, 0x1000);

    /* Unpopulated chip-select ranges: reads all ones, writes logged. */
    cdj_open_bus(sysmem, "sh7263.cs0-rest", 0x00400000, 0x03C00000);
    cdj_open_bus(sysmem, "sh7263.cs1", 0x04000000, 0x04000000);
    cdj_open_bus(sysmem, "sh7263.cs2", 0x08000000, 0x04000000);
    cdj_open_bus(sysmem, "sh7263.cs3-rest", 0x0E000000, 0x02000000);

    info_report("%s: boot through the bootloader in the NOR flash, "
                "entry 0x%08x, sp 0x%08x",
                cdj850_board.name, cdj850_board.fw_entry,
                cdj850_board.init_sp);
    qemu_register_reset(cdj850_reset, cpu);
}

static void cdj850_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-850 (Renesas SH7263 / SH-2A), bring-up";
    mc->init = cdj850_init;
    mc->default_cpu_type = TYPE_SH2A_CPU;
    mc->default_ram_size = 32 * MiB;
}

DEFINE_MACHINE("cdj850", cdj850_machine_init)
