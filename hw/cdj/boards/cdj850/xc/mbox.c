/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "mbox.h"
#include "trace.h"

/*
 * Mailbox/host-port at 0xFFFFC000 (channels +0x000/+0x004 and +0x800/+0x804):
 * minimal ACK stub so the driver-init handshake proceeds.
 *
 * Firmware protocol (driver 0x0C075E40/0x0C075EF6/0x0C075F80/0x0C075FCA,
 * entered after the 0xFFFE7000 serial packet; pcs 0x0C075E5C-0x0C07605E):
 * write a command word to +0x000, clear/poll the status word at +0x004
 * (bit 0 = complete, bit 24 = second ready bit, both polled), then read
 * the response back from +0x000. A second, identical channel pair lives
 * at +0x800/+0x804 (driver 0x0C076006: same clear/poll/write sequence).
 * Data words read back the last-written value (RAM-identical); status
 * words additionally report both ready bits set (always-ready: the real
 * peer answers at once on the bench, and a fake busy delay only risks a
 * firmware-side timeout). The driver has dedicated init paths for a
 * bit0-clear data word (0x0C075F34) that also proceed under always-ready.
 *
 * The block's identity is open (DSP host port vs panel MCU mailbox;
 * Task 7/8 own it): command words seen are 0x01000000 and 0x90102, the
 * polled status mirrors to SDRAM 0x0CEE33D0/0x0CEE33D4/0x0CEE3160, and a
 * 0x0C0CF4C4(0x0C155AAC, 0) call after the first handshake must return 0.
 * Carved out of the on-chip-hi RAM window (the boot zero-fill
 * 0xFFFFFCA4-0xFFFFFEA3 does not cover it) with a higher-priority
 * overlap, as notes/cdj850-bringup.md provides.
 */
#define CDJ850_MBOX_BASE    0xFFFFC000
#define CDJ850_MBOX_SIZE    0x810
#define CDJ850_MBOX_ST0     0x004
#define CDJ850_MBOX_ST1     0x804
/* Status-word ready bits: bit 0 = complete (bld/bf-polled), bit 24. */
#define CDJ850_MBOX_READY   0x01000001u

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_MBOX_SIZE];
} Cdj850Mbox;

static uint64_t cdj850_mbox_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Mbox *s = opaque;
    uint64_t v = 0;
    unsigned i;
    static const hwaddr st[2] = { CDJ850_MBOX_ST0, CDJ850_MBOX_ST1 };
    int k;

    for (i = 0; i < size; i++) {
        v = (v << 8) | (off + i < CDJ850_MBOX_SIZE ?
                        s->reg[off + i] : 0);
    }
    /* Status words report ready on top of the stored value. */
    for (k = 0; k < 2; k++) {
        unsigned b;

        for (b = 0; b < 4; b++) {
            hwaddr a = st[k] + b;

            if (a < off || a >= off + size) {
                continue;
            }
            if ((CDJ850_MBOX_READY >> (b * 8)) & 0xff) {
                unsigned shift = (off + size - 1 - a) * 8;

                v |= (uint64_t)(((CDJ850_MBOX_READY >> (b * 8)) & 0xff)
                                << shift);
            }
        }
    }
    if (cdj850_trace()) {
        info_report("cdj850: mbox read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_mbox_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Mbox *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = off + size - 1 - i;

        if (a < CDJ850_MBOX_SIZE) {
            s->reg[a] = val >> (8 * i);
        }
    }
    if (cdj850_trace()) {
        info_report("cdj850: mbox write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_mbox_ops = {
    .read = cdj850_mbox_read,
    .write = cdj850_mbox_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_mbox = {
    .name = "cdj850-mbox",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Mbox, CDJ850_MBOX_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_mbox_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Mbox *s = g_new0(Cdj850Mbox, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_mbox, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_mbox_ops, s,
                          "cdj850.mbox", CDJ850_MBOX_SIZE);
    /* Priority 2: carve out of the on-chip-hi RAM (priority 1). */
    memory_region_add_subregion_overlap(sysmem, CDJ850_MBOX_BASE, &s->iomem,
                                        2);
}
