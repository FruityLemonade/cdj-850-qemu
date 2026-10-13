/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 mailbox/host-port at 0xFFFFC000 (minimal ACK stub). */
#ifndef CDJ850_MBOX_H
#define CDJ850_MBOX_H
#include "cdj_common.h"

void cdj850_mbox_init(MemoryRegion *sysmem, SuperHCPU *cpu);
#endif
