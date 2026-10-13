# CDJ-850 bring-up log

UPSTREAM_BASE=9042f71c674fc45f62b284764a21ba1d9451bb67

Upstream: https://github.com/Stijn-Jacobs/cdj-nxs2-qemu.git
Branch: cdj850-bringup (off main at UPSTREAM_BASE, 2026-10-12)
Goal: add Pioneer CDJ-850 firmware support to the emulator.
Constraint: never commit C850MAIN.UPD or anything derived from it.
New code license: GPL-2.0-or-later.

## Task 1 baseline (2026-10-12)

- Step 1: cloned upstream, created branch `cdj850-bringup`. Base short hash: 9042f71.
- Step 2: `split_update.py` is a script (argv parsing at top level), so a bare
  `import split_update` raises IndexError by design; `py_compile` passes and
  the importable tooling module `lzss_decode` imports cleanly.
- Step 3: all six "Adding a model" prerequisite paths exist:
  models/cdj900.conf, hw/cdj/boards/cdj900/board.c, hw/cdj/common/cdj_common.h,
  scripts/run/boot_main.sh, scripts/firmware/prepare_firmware.sh,
  launcher/firmware.py, launcher/model.py.
- Step 4: this file force-added (`git add -f`) because upstream .gitignore
  ignores `notes/` (local-data dir); committed as the branch's first commit.

## Task 5 skeleton + first boot (2026-10-12)

Machine `cdj850` (hw/cdj/boards/cdj850/board.c + meson.build, Kconfig
`CONFIG_CDJ850` selecting `CDJ_COMMON`, `default_cpu_type = -cpu sh2a`;
runs in the sh4eb target: ~/qemu-build-eb/qemu-system-sh4eb).

- fw_entry 0x00000100, init_sp 0xFFF90000: SH-2A power-on reset loads PC
  from the vector at 0 and SP from the vector at 4 (SH-2A Software Manual
  §3; SH7263 hardware manual §5.2.3); the boot region holds PC = 0x100,
  SP = 0xFFF90000 (manual-reset copies at 8/12). init_sr = 0 keeps the
  CPU reset value; the first three instructions re-mask anyway
  (`stc sr,r0; or #-16,r0; ldc r0,sr`, seen in `-d in_asm`).
- DRAM 32 MB at CS3 0x0C000000 (256 Mbit K4S561632J / 8 = 32 MB).
- Flash 4 MB at CS0 0x00000000 (DYW1796 32 Mbit / 8 = 4 MB;
  63 x 64 KiB + 8 x 8 KiB = 4096 KiB; big-endian bus, new `flash_be`
  descriptor flag; flash IDs unverified).
- Clocks: bus CLK_66M 66 MHz (service-manual block diagram; feeds flash +
  SDRAM); X101 33 MHz crystal (CPU/bus PLL on EXTAL), X102 48 MHz (USB).
- PVR/PRR/CVR stay 0: the SH7263 has no SH-4-style memory-mapped PVR and
  the boot code holds no 0xFF00xxxx literals; QEMU never reads the IDs on
  translate/helper paths.
- On-chip RAM 256 KB at 0xFFF80000; `sh7263.internal` unimp 0xFFFC0000 up
  (BSC/CPG/INTC/DMAC/ports); `sh7263.cache` unimp 0xF0000000 (16 MB) +
  0xE0000000 (4 KB); open-bus CS0-rest/CS1/CS2/CS3-rest.

Boot (firmware installed locally, untracked):
  scripts/firmware/prepare_firmware.sh --model cdj850 --install C850MAIN.UPD
  (+ boot.bin copied from a plain prepare outdir: --install only copies
  MODEL_EXPECTED images)
  python3 scripts/firmware/make_flash_cdj850.py  # extract/cdj850/flash_cdj850.bin
  CDJ_MODEL=cdj850 MAIN_QEMU=~/qemu-build-eb/qemu-system-sh4eb \
    MAIN_BOOT=flash FLASH=extract/cdj850/flash_cdj850.bin \
    bash scripts/run/boot_main.sh bringup0 30

First boot (30 s, EB binary, no resets, 654 log lines): entry 0x100 runs
real code; the first access is an SDRAM mode-register write
(`sh7263.internal: unimplemented device write (size 4, offset 0x01000,
value 0x00000000)` = 0xFFFC1000), then CPG (0xFFFE0002/04/10/18), ports
(0xFFFE3890+), BSC (0xFFFC0004/10/28, RTCOR 0xA55A keys), cache-array
writes (0xF0800228+ stride 0x10, value 0x0DDEF800), and a descending
zero-fill at 0xFFFC3FCA4+. Totals: 591 internal writes, 32 cache writes,
29 internal reads (MRS/CPG/port readbacks). The 8 s and 30 s runs log
identical counts: after init the firmware parks (likely sleeping for an
unmodeled INTC/CMT tick). Next: model the internal block (Task 6).

## Task 6 increment 1: INTC + CMT (2026-10-12)

Park analysis (monitor `info registers` on a live boot): pc=0x0C0B8DBE
(SDRAM MAIN code), sr=0x400000e0 (IMASK=14), sp=0xFFF8FFF8 (still the
reset stack), r0=0xFFFFFFFB (-5), r22=0xFFFEC000 (CMT base). Capstone-BE
disassembly of guest bytes shows the init sequence calls its first
subroutine (target from pool 0x0C0B8F78 = 0x0C0CDDCA, arg r4=1), gets -5,
and takes `bf 0x0C0B8DBE` into a `bra self` halt loop. (QEMU monitor
`x/i` mis-decodes the branch target as 0x0C0B8DBA -- trust Capstone, not
`x/i`, for SH-2A code.) The subroutine is TOPPERS-style kernel-object
init (error immediates -25/-18/-17/-34/-41/-5 = E_CTX/E_ID/E_PAR/
E_NOID/E_OBJ/E_SYS); nearby pool holds "freeWorkComm: pointer is
NULL!!!" strings. Boot-log order shows the firmware wrote CMT CMSTR=1
(CMCOR 0x1045 = 1 ms tick at Pphi/8) and later READ CMSTR back; the
unmodelled read returned 0, so the firmware concludes the timer never
started (E_SYS) and halts. Fix: a real ticking CMT with readback.

Modeled this increment (commit `feat(cdj850): model INTC+CMT ...`):
- CMT ch0 0xFFFEC000/0x10 (new hw/cdj/boards/cdj850/xc/cmt.c): CMSTR/
  CMCSR/CMCNT/CMCOR with readback, CMF write-0-clear, 1 ms QEMU tick,
  SH-2A irq queue delivering CMI0 vector 188 level 8 (family value from
  the SH7269 board; SH7263 manual table unchecked). Ch1 storage only.
- INTC 0xFFFE0800/0x800: plain storage for ICR/IPR RMW (delivery does
  not consult it).
- CCR 0xFFFC1000: u32 storage (boot cache-init sequence reads back).
- Cache windows as RAM sinks (0xF0000000/16 MB, 0xE0000000/4 KB): the
  boot tag pattern must read back.
- On-chip-hi RAM 0xFFFF0000/64 KB: the descending zero-fill
  0xFFFFFCA4-0xFFFFFEA3 must stick (later peripherals here carve out
  with higher-priority overlaps).
- CCN audit shadow 0xFF000000/0x30 (Task 5 concern): identical semantics
  to the mapped SH-4 CCN model (EXPEVT/INTEVT mirror env, rest zero) but
  reports the first accesses with guest PC, so a MAIN touch at
  0xFF00xxxx becomes visible. Candidates 0xFF000000/08/24 exist as
  single literals in main_flat.bin (contexts look code-ish, verdict
  open); UBC 0xFF200000 was already a logging unimp.
- Left unmodelled (writes succeed, no reads seen yet): BSC, CPG, ports.

Correction to the Task 5 paragraph above: 0xFFFC1000 is modelled here as
the cache-control word (CCR), not an SDRAM mode register -- the access
pattern (writes 0, 0x808, 0x101, two readbacks) is the boot block's cache
init, and the SDRAM mode setup lives in the BSC writes at 0xFFFC0000+.

Port gap (same increment): the remaining 25 reads are all port
readbacks (0xFFFE3890-96, 0xFFFE3910-16, 0xFFFE3990-96, 0xFFFE3A10-16,
0xFFFE3A86-96 at 1222-1224 ms, pcs 0x0C0B9090-0x0C0B91CE), each preceded
by a write to the same offset with the same value (CDJ850_TRACE=1).
Modelled as plain storage 0xFFFE3800/0x800 (no strapped pins needed).
After this increment the unimp log is 18 writes, 0 reads (BSC
0xFFFC0004/10/28/34/4C/50/54/58 incl. RTCOR 0xA55A keys, CPG
0xFFFE0002/04/10/18, PFC 0xFFFE0408/0C/10/14/18, 0xFFFC5040) -- down
from 591+32+29. CCN audit shadow saw zero touches in the full boot, so
MAIN does not use 0xFF000000-0xFF00002F (the three single literals
0xFF000000/08/24 in main_flat.bin are data, not executed MMIO).

Debug method note: QEMU monitor `x/i` mis-decodes SH-2A branch targets
(`bf 0x0C0B8DBE` shown as `bf 0x0C0B8DBA`) and 32-bit insns; use
Capstone-BE on `xp`-dumped bytes plus `--gdb tcp:...` single-stepping
(`s`/`g`/`m`/`M` packets, scripted; do NOT send QStartNoAckMode
repeatedly, use one session per boot) for ground truth. QEMU watchpoints
+ single-stepping 32-bit code can trip a `record_save` translator
assertion (debug path only; normal boots unaffected).

Pool-2 wall (park persists at 0x0C0B8DBE, r0=-5=E_SYS): single-step
traces prove the failing call is the RTOS object-create 0x0C0CDDCA
whose second small-block alloc (pool 2, 0x28 B via 0x0C0D0256, and an
earlier 0x44 B one via 0x0C0C72A2) returns NULL because pool 2's arena
base [0x0CE4A330] (static [0x0C18BF68]) is 0 and no executed code ever
writes it nonzero (2 BE32 refs in the whole SDRAM image, both reads).
Pool 1 (15 MB arena at 0x0CEEFA20) works. No device reads happen
between pool init and the halt (traced), no CS/external touches at all,
flash ID mode is never entered (no 0xAAA/0x554 sequences), and 0xFF
power-on fill of OCRAM/hi-RAM changes nothing -- so this is not a
missing-peripheral gap but an RTOS-init precondition. WHY the base is 0
on real HW too (same image/code) is unexplained; candidates exhausted:
not ports/CCR/CMT/INTC readbacks (all consistent), not CCN, not flash
IDs, not RAM power-on.
  Local diagnostic bridge (NOT committed, runtime-only): halt at -S,
  `Mce4a330,8:0DDEFA20 00100000` (pool-2 base = free SDRAM hole
  0x0DDEFA20, size 1 MB), cont. Boot then proceeds through RTOS init
  into driver init, next gap: tight size-1 read poll of 0xFFFE7004
  (pc 0x0C077C3C, r0=0xFFFE7004). Continue the per-gap loop from there
  with the bridge applied; each new peripheral still gets its own
  commit below.

## Task 6 increment 2: SCIF/serial 0xFFFE7000 (2026-10-13)

Adopted the uncommitted `cdj850_ser_*` stub (+106 lines in board.c):
kept, with one fix. The stub is sound: setup writes +3/+0/+1/+2/+4/+3
(0x00/0x80/0x40/0xC5/0x04/0x80 -- SCIF SCSMR/SCBRR/SCSCR-style init),
1140 TX bytes through +6 with a `read +4, and #4, loop while 0'
TX-ready poll (driver 0x0C077B06/0x0C077C3C), teardown read +3 / write
+3=0. Bridged boot (`CDJ_RPOKE="0x0c0d0122:0:0x0c400000:72:209715200,
0x0c0b8db2:0:0,0x0c0ff6c6:0:0,0x0c00570a:0:0"`, see below) sends the
full packet and the driver completes (port write +0x282, port read
+0x21E follow). Fix: busy reads reported 0x0C (bits 2+3), so the
`and #4' poll saw ready at once and the fake transfer never waited;
busy now reports 0x08 (bit 3 shifting, TX-ready clear), ready 0x04.
The packet is binary, not text, so this channel is a protocol link
(panel/DSP candidate), not the DBG_TxD console -- the stub stays a
TRACE logger (CDJ_FWLOG equivalent). Packet archived untracked for
Tasks 7/8: `extract/cdj850/ser_tx_bringup2.bin` (1140 B, sha256
395475ef...f91196; first 00000900000030001304d03f07b49e0, last 45c8...
0af080ff0000).

Bridge note (correction to the paragraph above): the GDB `Mce4a330'
poke applied at reset is cleared mid-boot ([0x0CE4A330] reads back 0
at the E_SYS halt; the 0x0DDEFA20 hole holds image bytes then, not
free space). The working bridge is the committed `CDJ_RPOKE'
translator facility: RPOKE[0] (allocator ramp at 0x0C0D0122 into
0x0C400000+72) is load-bearing; RPOKE[1..3] (r0=0 at 0x0C0B8DB2/
0x0C0FF6C6/0x0C00570A) are no-ops (r0 already 0 at first hit) kept as
guards. MAIN is not verbatim in flash (main region entropy 7.69 =
compressed; strings like `freeWorkComm' exist only in SDRAM), so all
firmware forensics below are against live `xp`/memsave bytes, with
Capstone-BE (QEMU `x/i` mis-decodes SH-2A; Capstone 5.0.7 itself
misses some SH-2A 32-bit forms -- trust `in_asm`, not Capstone,
for what executed).

