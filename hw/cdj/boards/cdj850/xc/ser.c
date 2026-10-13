/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "ser.h"
#include "trace.h"

/*
 * Serial block at 0xFFFE7000 (bytes used: +0..+6): TX stub with tracing.
 * The firmware writes +3/+0/+1/+2/+4/+3 (setup), then sends a packet
 * byte-by-byte through +6, polling +4 bit 2 (TX-ready) between bytes
 * (driver at 0x0C077B06/0x0C077C3C: read +4, and #4, loop while 0).
 * A write to +3 or +6 kicks a fake transfer: the next
 * CDJ850_SER_BUSY_READS reads of +4 report shifting (bit 3, TX-ready
 * clear), later reads report ready (bit 2, matching the 0x04 the firmware
 * itself wrote to +4). Count-based (not time-based) so log stalls cannot
 * wedge it.
 * TX bytes (+6) are the firmware's output; they print under CDJ850_TRACE
 * (the CDJ_FWLOG equivalent for this bring-up). The block's identity
 * (SCIF channel? panel link? DSP host port?) is open until its driver is
 * identified -- this is an ACK stub, not hardware.
 */
#define CDJ850_SER_BASE    0xFFFE7000
#define CDJ850_SER_SIZE    0x10

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SER_SIZE];
    uint32_t busy_reads;
} Cdj850Ser;

#define CDJ850_SER_BUSY_READS 16
/* Status bits on +4: bit 2 = TX-ready (polled), bit 3 = shifting (busy).
 * Busy reports bit 3 WITHOUT bit 2, so the firmware's `and #4' poll
 * genuinely waits out the fake transfer instead of seeing ready at once. */
#define CDJ850_SER_TXRDY 0x04
#define CDJ850_SER_BUSY  0x08

static uint64_t cdj850_ser_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Ser *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_SER_SIZE - 1)];
    }
    if (off <= 4 && 4 < off + size) {
        unsigned shift = (off + size - 1 - 4) * 8;

        v &= ~((uint64_t)(CDJ850_SER_TXRDY | CDJ850_SER_BUSY) << shift);
        if (s->busy_reads) {
            s->busy_reads--;
            v |= (uint64_t)CDJ850_SER_BUSY << shift;
        } else {
            v |= (uint64_t)CDJ850_SER_TXRDY << shift;
        }
    }
    if (cdj850_trace()) {
        info_report("cdj850: ser read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_ser_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Cdj850Ser *s = opaque;
    unsigned i;
    bool kick = false;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (CDJ850_SER_SIZE - 1)] = val >> (8 * i);
    }
    if (off <= 3 && 3 < off + size) {
        kick = true;
    }
    if (off <= 6 && 6 < off + size) {
        /* TX byte: the firmware's serial output. */
        unsigned shift = (off + size - 1 - 6) * 8;
        unsigned byte = (val >> shift) & 0xff;

        kick = true;
        if (cdj850_trace()) {
            info_report("cdj850: ser TX 0x%02x ('%c') at pc=0x%08x "
                        "%" PRIu64 " ms", byte,
                        (byte >= 32 && byte < 127) ? byte : '.',
                        s->cpu->env.pc, cdj850_vms());
        }
    }
    if (kick) {
        s->busy_reads = CDJ850_SER_BUSY_READS;
    }
    if (cdj850_trace()) {
        info_report("cdj850: ser write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_ser_ops = {
    .read = cdj850_ser_read,
    .write = cdj850_ser_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_ser = {
    .name = "cdj850-ser",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Ser, CDJ850_SER_SIZE),
        VMSTATE_UINT32(busy_reads, Cdj850Ser),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_ser_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Ser *s = g_new0(Cdj850Ser, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_ser, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ser_ops, s,
                          "cdj850.ser", CDJ850_SER_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_SER_BASE, &s->iomem,
                                        1);
}
