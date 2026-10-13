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

## Task 6 increment 3: mailbox/host-port 0xFFFFC000 (2026-10-13)

Gap: bridged boot with increment 2 parks spinning at 0x0C075EA2
(`mov.l @(r0,r1),r2; bld #0,r2; bf self` = wait bit0 of 0xFFFFC004).
Driver 0x0C075E40 (called from 0x0C075E36, pr=0x0C075E3A): write
command to +0x000, clear +0x004, poll +0x004 bit0, read response from
+0x000; second identical channel pair at +0x800/+0x804 (driver
0x0C076006, same sequence). Status word also carries bit24 (second
driver 0x0C075EF6 polls it: `tst 0x01000000; bt wait'). Modeled
(board.c `cdj850_mbox_*`, window 0xFFFFC000/0x810, priority-2 carve
out of the on-chip-hi RAM): data words echo last write (RAM-identical),
status words (+0x004/+0x804) OR 0x01000001 (always-ready; the driver
has bit0-clear init paths at 0x0C075F34 that proceed under it too).
Boot evidence (bringup2, 20 s, bridge+TRACE): all 14 mbox accesses
clean -- +0x004/+0x804 read 0x01000001, commands 0x00000000 then
0x00090102 written to both +0x000/+0x800; the
0x0C0CF4C4(0x0C155AAC,0) validation returns 0 (no E-6002-style halt
at 0x0C075ECA). Unimp 19w+1r -> 30w+9r (strict progress): new touches
CPG STBCR2 0xFFFE0018 (R, size1), DMAC DAR_1/CHCR_1 0xFFFE1014/0x101C
(W 0xFFFFC008, W 0/0x1810, R), DMARS0 0xFFFE1300 (RMW 0x2300), same
trio for channel 2 (0xFFFE1024/0x102C/0xFFFE1304=0x0027), BSC(?) read
0xFFFC750C (size1), then port polls. No DMAC stub: 0x1810 leaves DE=0
(mode only, no transfer; SAR/DMATCR never programmed), so storage
would only risk a busy-spin regression -- recorded for later. DMAC
map (from image pools at 0x0C1C14E0+: SAR/DAR/DMATCR/CHCR at
0xFFFE1000+0x10n, DMAOR 0xFFFE1200, DMARS0/1 0xFFFE1300/0x1304,
STBCR2 0xFFFE0018/MSTP8) matches the SH DMAC family (app-note
cross-check).

Park (NOT the milestone): after the port polls the firmware `rts's
into data at 0x0C0C78D8 (pr=0x0C000008, stale on ALL port entries;
sp pristine 0xFFF90000), garbage-executes to an illegal insn, and
lands in the flash halt field at 0x000002F8 (`bra self`, pid-visible
for the rest of the run; CMT vector 188 pends, IMASK=14). This is not
a peripheral gap: the bootloader jmp @[0x49C]=0x0DDEFB7C entry is
legitimate (flash vector), control is register-clean through the
mailbox validation (pr verified at 0x0C075E5C=E3A, 0x0C07607E,
0x0C0CF4C4, orchestrator-ret 0x0C00575C), and zero-filling the RPOKE
scratch (GDB `M0c400000,1000:00..', verified zeros) reproduces the
identical halt. Prime suspect: pool-2 objects consumed unconstructed
(RPOKE bypasses RTOS init) or DMA-absent buffer state. Next step:
fresh-code forensics on the orchestrator(0x0C00575C-ret)->port-entry
path (DMA driver text before pool 0x0C1C14E0; stale dumps unreliable
there -- use live `xp'), watching for the first `lds pr'/indirect
call off the uninitialized structs.

For Task 7 (panel): ser packet above + PFC 0xFFFE0414 RMW + CPG
0xFFFE0018/STBCR2 patterns. For Task 8 (DSP): mailbox channels
0xFFFFC000/4 + 0xFFFFC800/4, command words 0x01000000/0x90102, SDRAM
mirrors 0x0CEE33D0/0x0CEE33D4/0x0CEE3160/0x0CEE34C1, validators
0x0C075E40/0x0C076006/0x0C0CF4C4/0x0C0C688A/0x0C0C6894, data
0x0C155AAC, DMA DAR_1/2 hooked to mbox+8 (+0xFFFFC008/+0xFFFFC808).

## Task 6c: genuine pool-2 init — stale SR.RB strands the pool-2 base (2026-10-13)

Wall (no bridge) reproduced exactly: pc=0x0C0B8DBE, r0=-5, sr IMASK=14,
18 unimp writes / 0 reads. Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-6c-report.md. Forensics
(GDB RSP + `-d in_asm`, in_asm authoritative; scripts in /tmp t6c_*.py):

- The failing call is RTOS object-create 0x0C0CDDCA (from 0x0C0B8D96,
  id=1): its pool-A alloc (via 0x0C0D0230, free list at [0x0CE4A32C] =
  0x0CEEFF30) succeeds, but the 0x28 B pool-2 alloc (via 0x0C0D0256)
  NULLs (head var [0x0CE4A330] = 0) -> E_SYS (-5) -> `bf` to the
  `bra self` halt. The earlier 0x44 B pool-2 alloc (via 0x0C0C72A2 from
  0x0C0895AC) fails the same way; its caller tolerates -5 (stores -1).
- Pool-2 can never fill through MAIN code: exhaustive BE32 scan of a
  wall-time pmemsave (32 MB SDRAM) shows [0x0CE4A330] has exactly 3
  refs (1 writer: the pool-register store at 0x0C0C6BBE; 2 alloc
  readers) and the flag [0x0C18BF68] has exactly 2 refs (both reads).
  GDB write-watch from reset proves both stay 0: BSS zero-fill, an
  init-table clear (0x0C0B92BA), and the pool-register itself writing 0.
- The pool-2 arena base is decoded image data, not peer data:
  [[0x0C0C64E0]] = [0x0C14F700] = 0xFFF86638 (on-chip RAM), written by
  the bootloader LZSS decoder before init runs (byte-wise watch hits),
  with valid .data neighbors. So this is NOT a Task 7/8 peer-data
  dependency (Exit B out): the base is present, but never arrives.
- Root cause (GDB single-step 0x0C0C64B8->0x0C0C64CE): QEMU's sh4
  target applies SH-4 register banking to `-cpu sh2a`, but the SH-2A
  SR has no MD/RB/BL bits (SH-2A Software Manual §2.2.2; reset sets
  only IMASK, reserved bits read 0) and its register banks work
  differently (interrupt auto-save + RESBANK, no active-bank bit).
  QEMU resets SR to MD|RB|BL|IMASK (0x700000F0, verified live at
  0x0C0C64B0 entry: sr=0x700000F1), so the whole boot runs on bank 1
  self-consistently until 0x0C0C64B0 loads the pool-2 base into r4 and
  executes `ldc` of a from-zero SR (0xF0, RB=0): the tail-call then
  reads stale bank-0 r4=0/r5=4 and registers pool-2 base 0. On silicon
  there is no bank switch, so the sequence is harmless and pool-2
  registers 0xFFF86638. The RPOKE ramp is retired (known-bad, Task 6b).
- Fix (board.c `cdj850_reset`, same pattern as the SH7269 GUI board's
  `env->sr = (1u << SR_MD)`): clear the SH-4-only RB/BL bits at reset,
  keeping MD (translator privilege) + IMASK (manual). No translator
  change, no SH-4 path touched.

Boot evidence (no bridge, EB rebuild): pool-2 head [0x0CE4A330] =
0xFFF866CC (live OCRAM free list); the old wall is gone; unimp goes
18w+0r -> 30w+8r, all legitimate driver-init touches (DMAC DAR/CHCR,
DMARS0/1, CPG STBCR2, CPG/PFC writes). The 9th bridged read
(0xFFFC750C) is confirmed bridge-degenerate (table-bytes-as-code in
6b) and does not recur. Boot proceeds through RTOS init, both mailbox
channels (0x90102), INTC IPR programming, and a CMT restart at 1560 ms.

Next park (NOT this task; brief files are board.c/xc only): first task
entry faults on the unimplemented SH-2A word 0x0068 at 0x0C08EFCA
(`extu.b/tst/mov.l` dispatch in 0x0C08EFC4, called from 0x0C08F294):
EXPEVT=0x180 illegal-insn -> firmware default vectors -> flash halt
`bra self` at 0x000002F0 (task stack, IMASK=0, RB=0). 0x0068 is absent
from notes/cdj850-sh-insns.md; identifying/implementing it is
translator-patch work (same class as commit 444dd14). USB enum not
reached (no USB activity yet).

Follow-ups: (a) `do_interrupt` still sets SR.RB on delivered
interrupts (helper.c:293) — two CMT ticks delivered harmlessly so far
(ISR spills/restores, rte returns to RB=0), but a translator-level
SH-2A banking disable (reset + ldc mask + gbank, gated on
SH_FEATURE_SH2A like the existing MD-force) would remove the hazard
class; (b) in_asm prints `mov r15,r5` for bytes 0x65F3 at 0x0C0C64C0
(standard decode: mov r3,r5) — display-only nit, non-load-bearing
(r5=4 comes from stale bank-0 +4 either way).

## Task 6b: wild-RTS forensics — Exit B, NOT a wild RTS (2026-10-13)

Reproduced identically (bridge+RPOKE, 20 s): 30w+9r, pc=0x000002F8
(`bra self` + nop), CMT vector 188 pending, IMASK=14. Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-6b-report.md. Headline
corrections to the park paragraph above (signed off via live-xp GDB +
`-d exec,in_asm`, in_asm authoritative, scripts in /tmp t6b_*.py):

- The `rts` at 0x0C0B91D6 fires TWICE. First (boot, pr=0x0C000008,
  sp=0xFFF90000) returns CORRECTLY into the MAIN init-call chain at
  0x0C000000 (`mov.l @PC,Rn; jsr/n @Rn` trampolines, SH-2A JSR/N=0x4n4B;
  literals 0x0C0B92AC/8FF4/951C; ends `bra self` at 0x0C00000C) and runs
  the 0x0C0B951C->0x0C0C64B0 cascade. The port function itself
  (0x0C0B91C0-D8, port polls + RAM stores, delay-slot store) never
  writes pr: it is clean in both passes.
- The fault is the SECOND pass: the 0x0C0C68xx driver epilogue
  (FPU restore 0x0C0C6806, GBR/MACH/MACL restore + `rte` 0x0C0C682C)
  resumes a task context with pc=0x0C0B931C (boot-time code area, by
  then overwritten with runtime descriptor tables referencing
  RPOKE-scratch 0x0C400AAF/AB0/1B0 + pool arena 0x0CE4A30C),
  pr=0x0C0C78D8 (more table-data), sp=0x0C400A53 (inside RPOKE
  scratch). Pre-history in the same exec flow: allocator 0x0C0D01xx
  (brackets the RPOKE site 0x0C0D0122, 3 alloc iterations at 0x0C0D01C4).
- The resumed thread runs descriptor bytes: a 2-insn oscillation at
  0x0C0B931C/1E (~86k TB execs, ZERO stub/MMIO accesses in the span —
  same-run exec+TRACE correlation) with `sts.l pr,@-r15` descending
  from scratch, then falls into the port driver (works: r6=0x1000 or
  0x0 across runs, same outcome), `rts` to 0x0C0C78D8, executes table
  bytes (`mov #-93,r4`; BSC read 0xFFFC750C=0; `shlr r0` -> 0x7FFF1D43;
  `bsr 0x0C0C78FE`, pr=0x0C0C78E6) and dies on the delay-slot word
  0x0C68 (an SH-2A form both disassemblers print as `.word`; no handler
  TB executes) -> flash halt 0x000002F8. [VBR+0x100]=0x0C0C68FC (never
  runs at fault), [VBR+0xA00]=0.
- Stub-independence (why no board.c change): (a) zero stub MMIO in the
  86k span; (b) the faulting indexed access (base 0x7FFF1D43 + 16-bit
  index) can never reach a valid mapping for ANY index value;
  (c) r6 varies run to run (0x0/0x1000), outcome identical;
  (d) mailbox-not-ready would park EARLIER (validation 0x0C075ECA).
  The DMA-driver-text hypothesis is out too (CHCR=0x1810 DE=0, path
  never touches 0x0C1C14E0).
- Verdict: Exit B. The resumed context is a creature of the RPOKE
  bridge (built by allocator flow through the ramp site, stack inside
  ramped scratch) and/or of peer-starved tables (mailbox echo consumed
  as DSP data). Owned elsewhere: genuine pool-2 init to replace the
  ramp (Task 6 remainder — the [0x0CE4A330] WHY is still open), and/or
  Task 8's DSP/panel upload populating these tables. Do NOT paper over
  with stub fabrications. No commit (Step 4: no code changed); suite
  untouched (baseline).

## Task 6e increment 1: panel-handshake ready pin 0xFFFE391E + xc split (2026-10-13)

Step 1 correction first: Task 6d's "park polling repeated 0x01c40 reads"
was imprecise. The 0x01c40 reads are USB INTSTS0 (offset 0x40 of the
0xFFFC1C00 block), BOUNDED (12x, then boot moves on to the 0x2801x block).
The true infinite park is a SILENT port poll at 0x0C08E8F2, invisible in
the unimp log because the port block is modelled storage.

- Full device address: r0 = 0xFFFE391E (movi20 #0xE391E sign-extends),
  r11 = 0; `mov.w @(r0,r11),r6` reads the word, waits for bit 2
  (extu.w; shar; shar; bld #0,r2; bf loop). Live QMP: pc stable at
  0x0C08E8F2/0x0C08E8F4 over 3 samples/6 s, r0 = 0xFFFE391E, r11 = 0.
  CDJ850_TRACE: `port read +0x11e size 2 = 0x0 at pc=0x0c08e8f2` from
  1587 ms on; +0x11E never written (only +0x110..+0x116 pin-function
  neighbours), so stored zero never releases it.
- Expected pattern: bit 2 = 1 (peer ready/idle). Driver 0x0C08E8AE
  (device id 1; sibling 0x0C08E842 for id 0 never runs this boot):
  int-disable, 0xAA/0x55 probe patterns to void 0x20000xxx scratch,
  SR restore, delay, ACK-wait, outcome to SDRAM 0x0CEE34AA + flag.
  Called from the mode-2 branch of the boot dispatcher (state
  [[0x0C08EC28]] == 2).
- Hardware: panel-handshake GPIO. The 0xFFFC1C00 block is the on-chip
  R8A66597-family USB (offsets match hw/cdj/common/usb_r8a66597.h
  exactly: SYSCFG 0x00/SYSSTS0 0x04/DVSTCTR0 0x08/INTENB0 0x30/INTENB1
  0x32/BRDYENB 0x36/NRDYENB 0x38/BEMPENB 0x3A/INTSTS0 0x40/PIPESEL
  0x64/PIPECFG 0x68/PIPEBUF 0x6A/PIPEMAXP 0x6C/PIPECTR5 0x78; INTENB1
  0x4030 arms BCHG + control-transfer interrupts; firmware strings
  usbh_msc/usbTask/USB-A-host + USB-B-function confirm both stacks).
  The service manual's signal block diagram wires MAIN (IC103) to the
  panel CPU (IC1007) over TSI/TSO/TSCK serial plus PNL_XBUSY/MAIN_XBUSY
  handshake GPIOs (connector CN2003) — the ACK-wait shape fits, with
  USB_CDETFLG/VBUSON and DSP-READY as noted alternates. Exact SH7263 pin
  name for +0x11E unchecked (no hardware manual on file).
- Model (xc/port.c): reads of +0x11F (low byte of the +0x11E word;
  self-caught off-by-one — strapping +0x11E reads back 0x0400, which the
  shar-shar-bld sequence still sees as 0) OR in bit 2, storage otherwise.
  The level is what the driver's own comparison dictates (same always-
  ready rationale as the mbox stub); no data bytes invented; the panel
  protocol stays Task 7's.
- File-split rule applied: port/ser/mbox move to xc/ (xc/port.c,
  xc/ser.c, xc/mbox.c + headers, shared xc/trace.h); board.c keeps
  intc/ccr/ccn-audit; meson updated. Verbatim moves except the pin bit.
- Evidence: rebuild warning-free (only the pre-existing ld notice);
  boot passes the handshake (`port read +0x11e = 0x4` exit at 1482 ms),
  runs ser-upload (1143 TX bytes via 0xFFFE7000), mbox 0x90102 rounds,
  INTC IPR programming, CMT restart — unimp counts unchanged 82w+46r
  (no new unmodelled touches). Suite 659 passed / 38 skipped / 1 xfailed.

Next wall (NOT a fault, NOT a translator gap — a silent peer-event
park, this task BLOCKED here): outer loop 0x0C0B8EAE spins while the
scan 0x0C0B8ED8 returns mask 0 over 17 pool-2 object slots (bytes
0x0CE49CBC = 00 00 03..0E 00 00 00, words 0x01010101.., ptrs to OCRAM
TCBs; release needs [*ptr+20] to differ from word for a claimed entry).
Hard park (60 s run never leaves; pc histogram only scan + CMT-tick
ISR 0x0C0C68FC). -d exec proves the RTOS is alive: usbTask creator
(0x0C10B34A), usbh_msc (0x0C01DAFC+), servoTask entry (0x0C0FF8C8, name
confirmed in SDRAM strings), ser TX driver, tick ISR all execute, then
everyone blocks on peer/device events that never arrive (no panel/DSP/
USB peers; our ser/mbox stubs raise no RX/IRQ). What unblocks it needs
a Task 7/8 call: panel-serial RX bytes? DSP/mbox reply IRQ? servo
"no disc"? SH7263 manual (INTC IPR +0x40E source at 1477 ms, USB
vectors) required before any IRQ wiring — do not guess vectors.

## Task 8a: DSP/servo signaler hunt — BLOCKED (stop rule; 2026-10-13)

Wall reproduced exactly (no bridge, EB rebuild of 07df07e): outer
0x0C0B8EAE spins, scan 0x0C0B8ED8 returns mask 0 over 17 slots
(claim bytes 0x0CE49CBC = 00 02..0E 00 00, words 0x0CE49C74 =
01010101..11111111, ptrs 0x0CE49F58: slots 0/1/16 null, 2-15 OCRAM
TCBs). Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8-report.md.

- Scan protocol decoded (authoritative `-d in_asm`; the 32-bit forms
  0x32E1_8000 = MOVU.B @(0,r14),r2 = claim-byte load and 0x8728-class
  BLD display as `.word`, semantics execute correctly): per idx,
  r3 = ptr[idx+1], r2 = [r3+20] (signal ptr), r4 = [r2] (signal
  value), claim[idx] must equal idx+1, and a mask bit sets only when
  [[ptr+20]] DIFFERS from word[idx]. Release needs ANY single bit
  (outer `tst r0,r0; bt.s re-scan`); post-release stores mask to
  [0x0CE49CD0] and calls the dispatcher (0x0C108xxx), else re-scans.
- `[[ptr+20]]` writer watch (CDJ_MWATCH, all 14 signal cells across 4
  boots + mask cell): the SOLE writer is the RTOS creator at
  0x0C0C7490/pr 0x0C0C7438 (init-fill 0x0X0X0X0X); NO in-guest task
  or ISR ever signals. The 13 claimed objects are a task-init
  barrier gated purely on peer events.
- "mailbox" 0xFFFFC000/0xFFFFC800 identified as SH7263 SSI0/SSI1
  audio (hardware manual Table 18.2: SSICR_0 0xFFFFC000,
  SSISR_0 0xFFFFC004, SSITDR_0 0xFFFFC008, SSIRDR_0 0xFFFFC00C; ch1
  at 0xFFFFC800): init writes 0/0x90102 complete under the stub,
  SSI0 IRQ pri 7 via IPR14 (0x0C075EE0), SSI1/3 + ROM-DEC IRQs via
  IPR15; NO post-init SSI status/data/IRQ wait exists (14 accesses
  at 1474-1475 ms, then silence incl. SSITDR DMA DAR with DE=0).
  NOT the signaler. (Model rename to ssi.c deferred; behavior is
  SSI-plausible today.)
- E-6002 anchor INVALID: the string (and "6002") is absent from
  C850MAIN.UPD, main_flat.bin, and live 32 MB SDRAM (exhaustive
  search); error display uses "SYSTEM ERROR : %s %d" numerics. The
  DSP-fail path cannot be xref'd; DSP program bytes not located.
- DSP upload driver: Port-E GPIO bit-bang (transfer 0x0C0FC9xx,
  gates 0x0C0FCA7A/0x0C0FCBB0 waiting PE6+PE8, bring-up
  0x0C0FD7CC-8D0 asserting PEDRL bits 6,8,7 at 1530 ms, PEIORL
  phases 0xF1C0/0x1C0, reply sampling PE9/PE12/PE14 into bytes at a
  RAM buffer). PE12 = DSPDREQ (service-manual schematic: IC103 pin
  72 = DSPDREQ; DSPC56371AF180 HREQ pin 38; DSPRST-H/SRVRST-H
  release, DRVMUTE-L = no disc). Driver state + byte buffer FROZEN
  across a 20 s live sample while port accesses cycle: retry-stuck
  on DSP reply acceptance, NOT slow.
- Release experiments (local-only port harness, REMOVED before
  commit; delivery verified in TRACE: PFPRL reads 0xA000, PBPRL
  0x18, PFPRH 0x01, PEDRL 0x1680/0x5680 with strapped bits; release
  detector = MWATCH on [0x0CE49CD0]): pe12/pe9/pe10/pb3/pb4/
  pf13+pf15/pf16/pf25/all-combined — NO release in any run.
  Static READY levels do not satisfy the DSP reply validation;
  supplying reply bytes would be fabrication. Stop rule applies.
- Servo: PBPRL-bit3 poll (servoTask sub 0x0C0FF7D4) self-exits via
  a 46-spin timeout; pb3 strap (verified 0x18) gives no release;
  NO-DISC strings + DRVMUTE-L honest no-disc state already hold.
  NOT the signaler (no "no disc" answer needed).
- Release machinery PROVEN genuine-needing: PPOKE of one signal
  cell sets the mask, dispatcher runs, then error-halt at
  0x0C0B8ED4 (degenerate task state, 6b-class); GDB full-13-cell
  poke runs the dispatcher repeatedly then halts with IMASK=15
  (tick starved). Flags alone are insufficient — the 13 tasks must
  REALLY execute.
- Peer-event inventory for follow-ups (all sustained spins, no
  fabrication): PB4 (0x0C0062CE), PF13/PF15 = SD_D1/SD_CD
  (0x0C0D69B2/C0), PF16/FRB = flash-ready (0x0C0057E4), PE10
  (0x0C10DCEA, usb region), PF25/SSIWS2 (0x0C106074), panel
  accept-path stores 0x0C7F663C/0x0C7F65BC NEVER written (MWATCH;
  Task 7 loose end (a), Task 9 scope), DSP reply-byte validation.
  IPR arming decoded: IPR13 = SCIF0/1, IPR14 = SSI0 (pri 7),
  IPR15 = SSI1/3 + ROM-DEC; only the CMT tick is delivered.
- Verdict: 8a BLOCKED per its stop rule (neither DSP nor servo
  releases it with driver-justified semantics). 8b/8c/8d stop
  honestly (no carve target, no SHA, no core, no audio hook).
  No code changed this slice (harness removed; tree == 07df07e).

## Task 8b: send-buffer + reply validation mapped, carve BLOCKED (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8b-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `cfacebc`. Capstone
SH-2A + hand-decode (MOVU.B/BLD gaps vs `translate.c`); `-d in_asm`
spot-check only (driver TBs absent from the window — stated in
report). Live GDB RSP + `info registers` + `CDJ850_TRACE` + standing
32 MB SDRAM dumps.

- Send chain: transfer `0x0C0FC9DC` (5× loop, delay `0x0C107932`,
  helpers `0x0C0C688A/94`, sampler `0x0C0FC8FA`, `bld.b` reply test)
  → mirrors `0x0CEE34B0/84` → port `0xFFFE3A02/06` (`+0x202`,
  phases `0x1C0/0xF1C0` live) → send `0x0C0FCC68/0x0FCD0E`
  (`r4` byte, BSR-only callers) → callers `0x0C0FCAC0/0x0FCBEA`
  (`r12/r14/r11/r13` bytes) → gates `0x0C0FCA7A/0x0FCBB0`
  (wait PE6 then PE8) → bring-up `0x0C0FD7A8/0x0FD7CC`
  (`r4=0x0CE4B5DA`, `pr=0x0C0FC36A` live). Trace: 2–3 words
  (`0x5080/0xB080/0xD080`, 2650–2696 ms) then gate-spin at
  `0x0C0FCBB0` — full program never clocked out.
- Sequencer `0x0C0FDA2C–0x0FDC1E`: 36 hardcoded `(index,data)` via
  `0x0C0FCA5E` (delay-aware parse; 26 such sequencers repo-wide —
  AUDIO-vs-SERVO owner unknown). Control writes, not claimed as
  program. Candidate `0x0CE4B6F4–0x0CE4B810` (284 B, header +
  `07FF…`) has no reader refs and is absent from UPD/main/boot
  contiguously (packed MAIN, entropy 7.55) — carving it would be a
  guess. Refused. No SHA pinned, `notes/cdj850-layout.md` untouched,
  `MODEL_EXPECTED` untouched.
- Reply: sampler `0x0C0FC8FA` → byte `0x0C7F6D6C`; collector
  `0x0C0FC8DC` → 10 B `0x0C7F6D1C` (live zeros/`0x11`, frozen);
  validator `0x0C0FC71C–0x0FC87E` checks `0x0C7F6D6C`,
  `0x0C7F6D5C+2`, `0x0C7F6D67`, `0x0C7F6D02`, `0x0CE4B8A6`,
  `0x0C7F6AE9` — accept stores `1→0x0CE4B8F5` via `0x0C104D64`,
  reject calls `0x0C104E6A`, return `0x0C104EE2`. Never accepted
  live.
- Rename `mbox.c→ssi.c` deferred (forensics-only slice; tree-wide
  touch rides with the next code task). No carve script (would emit
  a guess). 8c inherits: unblock packed-MAIN→SDRAM + AUDIO/SERVO
  owner first; census starts at the 36-pair table + other 25
  sequencers + candidate block (unpinned).

## Task 8c: DSP bootstrap READY + recorded init image; reply BLOCKED (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8c-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `97ae419`.
Commits: `7f29562` (mbox.c→ssi.c behavior-identical rename),
`dddf5f7` (dsp56371.c bootstrap READY + record). No subagents. No
bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets live in /tmp only (never committed).

- Datasheet fetch: DSP56371 UM Rev 2.1 (404 pp, operating modes §4.3,
  SHI-slave bootstrap modes 5/7, HREQ slave-ready semantics, App. A ROM
  source) + datasheet Rev 4.1 (HREQ/SPI-I2C timing §12/13) + DSP56300FM
  + SH7263 §29/30/34 (PEIORL 0xFFFE3A06, PEDRL, direction-aware reads).
  f70e4d §10 nets: DSPRST→IC301/56371 RESET_B pin47, SRVRST→IC201
  (Toshiba TC94A15FG — NOT a 56371), DSPDREQ←HREQ pin38 → MAIN PE12,
  MAIN PE7-11 ↔ DSP ESAI PE7-11, E-6002 = audio-DSP write fail
  (manual pp.26/34/39/43 — 8a's "absent from MAIN strings" stands, but
  the code is real in the manual/diag path).
- Bootstrap model (`dsp56371.c` at board level): DSPRST tracker
  (PEDRL 0→0x40→0x140→0x1C0, fires ~5-9 s) then READY (PE6/PE8/PE12)
  on Port-E reads; every MAIN PEDRL/PEIORL write recorded
  (`CDJ850_DSP_RECORD`, 20 B packed pc/ms/off/size/val). Reply DATA
  nibble stays loopback — no DSP payloads produced.
- Record: 519 genuine bytes, SHA-256 pinned in notes/cdj850-layout.md
  (identical across 3 boots), 259 (index,data) pairs + 0x00 pad, banks
  F0-F8/D7 — a control init image, NOT opcodes (census N/A, no core).
  Post-image (~9 s) the driver spins 42.8k× in the CD6C/CDDC sample
  loop (strobe 0x10 + sampler, PEDRL ∈ {0x1080,0x1180,0x11C0});
  collector zeros, sample byte 0x11, accept flag 0, mask 0 — the reply
  needs genuine DSP execution. BLOCKED precisely there; static-hunt
  roadmap (packed-MAIN→SDRAM decoder) recorded for follow-ups.

## Task 8d: program-source hunt — MOD=5 host-boot; decoder map; outer caller; carve BLOCKED (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8d-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `0e01416`. No subagents.
No bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets live in /tmp only (never committed:
`/tmp/371UM.pdf` + `/tmp/DSP56371.pdf` + `/tmp/DSP56300FM.pdf` +
`/tmp/sh7263_hw.pdf`, re-fetched public Motorola docs).

- MODA–D strapping (f70e4d.pdf p.76, §10.4 MAIN ASSY 3/4, vector-level
  schematic read + `371UM.pdf` Table 4-2): R312→MODA (pin 51) and
  R314→MODC (pin 49) to the supply rail (M, y=256.8 H+V topology),
  R313→MODB (pin 50) and R315→MODD (pin 48) to GND (thick y=240 rail
  via CORE_GND_3/pin 53), all 10 k. Alternating 1-0-1-0 assignment is
  deliberate (a single GND plane would serve all-GND; two rails prove
  differential strapping) → MODD:MODC:MODB:MODA = 0:1:0:1 = Mode 5
  (Bootstrap from SHI, slave SPI; cf. mode 2 = 0010 PROM jump).
  Implication: the DSP host-boots from MAIN — a MAIN-resident program
  upload MUST exist; PROM-resident (no MAIN copy) is OUT. Residual:
  M-rail exact voltage (1.25 V vs 3.3 V) unconfirmed from PDF vectors
  alone, but digital-HIGH intent is unambiguous. R316 4.7 k
  PINIT_NMI→GND and R351 100 DSPRST→RESET_B noted in passing.
- Decoder map (`scripts/firmware/c850_unpack.py`, GPL + test): packed
  `main_flat` `0x20–0x19E70A` (BE32 `0x19E6E6` + LZSS ring
  4096/init `0x20`/pos 4078, whole-flat unpack for the bootloader's
  overrun) → SDRAM `0x0C000000–0x0C39231E` (3744542 B, SHA-256 pinned
  in notes/cdj850-layout.md); remainder `0x19E70A–0x1A0000` checksum +
  pad. Verified against live 32 MB SDRAM (code/rodata spot match;
  BSS/heap diverge from `0x1C14DC` as expected); driver bytes
  disassemble identically from the unpack. Bootloader table at
  `boot.bin` `0x4B0–0x52C` (flash/SDRAM/len words incl. staging
  `0x0DDEFA20`/`0x0DDEFC1C`) cited as corroboration, not load-bearing.
- Outer caller (in_asm hot-TB attempt + unpack xref + live RSP
  breakpoint — RSP authoritative): `0x0C0FCD6C–0x0C0FCE52` is one
  function (gate → strobe `0x0C0FCD0E` r4=`0x10` → sampler
  `0x0C0FC8FA` → assembly `0x0C0FCE14–38`); 0 BSR/BRA callers (register-
  indirect, as predicted). Two literal-pool call sites: servo
  `0x0C0FF7D4`-family at `0x0C0FF86A/86C` (via `[0x0C0FF9EC]`,
  comparison `0x0C0FF86E+`) and audio/player `0x0C103984`-family at
  `0x0C1039B2/B4` (via `[0x0C103A60]`, comparison `0x0C1039B6+`).
  Live break at `0x0C0FCD6C` (40 s) lands pr=`0x0C0FF86E` (servo
  caller hot, r3=`0x0C0FCD6C`). In_asm gap recorded honestly (driver
  TBs `0x0C0FC6xx+` absent from the translation window, as in 8b).
  AUDIO-vs-SERVO owner split (8b concern 1): SHARED routine, both
  tasks call it — resolved as shared, not owned.
- Carve: NONE (BLOCKED). 519-B init image and 284-B `0x0CE4B6F4`
  candidate both absent from packed AND unpacked MAIN (byte search);
  candidate offset `0xE4B6F4` lies beyond the `0x39231E` image
  (runtime heap). Mode 5 predicts a host program, but no contiguous
  blob exists in MAIN — the upload stream is assembled from scattered
  immediates/tables, or the SHI payload shape is still misread. Next
  task needs SHI-protocol analysis (UM App. A Bootstrap ROM is public)
  or a logic-analyzer capture; do NOT guess.

## Task 8e: SHI-framed reinterpretation + header search + unpacker-PC trace — carve BLOCKED with new evidence (2026-10-13)

Full report:
.superpowers/sdd/2026-10-12-cdj850-support/task-8e-report.md.
EB `~/qemu-build-eb/qemu-system-sh4eb`, BASE `694cc05`. No subagents.
No bridge. No fabricated bytes/replies/vectors. Firmware read-only;
datasheets in /tmp only (never committed: `/tmp/371UM.pdf` UM Rev 2.1
404 pp + `/tmp/DSP56300FM.pdf` FM Rev 5 512 pp, re-fetched public
Motorola docs); derived bytes only in /tmp + untracked `extract/`.

- UM App. A sequence extracted verbatim: SHI slave-SPI bootstrap
  (HCSR `$A9`: HEN=1, HM=10, HFIFO=1, HMST=0, HRQE=01) takes 3 B
  count + 3 B start address + 3N B program words into contiguous
  PRAM, then `jmp (r1)` to the start address; 24-bit words (Table
  7-3), MSB-first (§7.4), HREQ asserted when IOSR ready + HRX FIFO
  not full, deasserted at first clock of next word (§7.7.1); PRAM 4K
  `$000000–$000FFF` (Fig. 3-1); opcode markers FM ch. 13
  (NOP `$000000`, RTS `$00000C`, REP `$06+Ax`, BRA `$05`,
  JMP `$0A/$0C/$0D`).
- 519-B re-parse (`scripts/firmware/c850_shi_search.py` + 4 tests,
  commit `68e80fa`): 173 words, offset-0 count `0x8BA08C`/`0x8CA08B`
  (need 171); zero exact-fit offsets in either order; 16-bit /
  data-only / index-only framings misfit; payload census both
  orders barren (0 NOP/RTS/REP/BRA; 2 isolated LE JMP singletons in
  head control bytes; `00Fxxx` words spell the F-bank walk).
- Header scan: unpacked 4816 structural / 253 vector survivors,
  packed 88 / 1 — all classes explained (SH `nop`/load opcodes as
  fake counts; survivors disassemble as valid SH-2A incl. a MAC-loop
  driver at `0x15EB0` and movi20 code at `0x1D1BC`; BSS hits are SH
  tables; packed hit is LZSS-stream coincidence); no survivor meets
  the genuine-program conjunction (pinned in test). Negative redo:
  full-length `bytes.find` — 519 B and 284-B candidate
  (`d47e1d71…c125d9fa4`) absent from packed AND unpacked.
- Unpacker-PC trace (fresh 12 s early `-d in_asm`, 7923 TBs):
  entry `0x0DDEFA20`, bit head `0x0DDEFA52`, literal store
  `0x0DDEFA78/7A`, match loop `0x0DDEFAA6`, exit `0x0DDEFAC2`,
  caller `0x0DDEFBE4`, then `0x0C000000`. Side find: 8d's own
  in_asm log already contained `0xddefa78` — its case-sensitive
  `0DDEFA78` grep missed it (corrected, no blame).
- Carve: NONE (BLOCKED). New evidence for the mode-5-vs-PROM
  re-examination: the 519 B is phase-1 control fill with no SHI
  framing; the program upload (if any) sits behind the
  never-completing reply handshake or is assembled purely from
  scattered immediates. Suite: full `tests/` 665 passed, 38
  skipped, 1 xfailed (2026-10-13).
