/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "dsp56371.h"
#include "xc/trace.h"

/*
 * Audio DSP peer (IC301, DSPC56371AF180) for MAIN's Port-E GPIO bit-bang
 * upload driver (bring-up 0x0C0FD7CC, gates 0x0C0FCA7A/0x0C0FCBB0,
 * transfer 0x0C0FC9DC, sampler 0x0C0FC8FA, collector 0x0C0FC8DC,
 * validator 0x0C0FC71C-87E; Task 8a §2e, 8b §§1-2).
 *
 * What this models (Task 8c step 2):
 *
 * 1. Bootstrap READY. After MAIN releases DSPRST (PEDRL bring-up 0 → 0x40
 *    → 0x140 → 0x1C0 at ~1530 ms, bits 6/8/7 in order; DSPRST releases the
 *    AUDIO DSP with H per the service manual §10.15 level table, and the
 *    power-on sequence "the program is downloaded to IC301" follows
 *    IC103-pin158 → IC301-pin47 RESET release), the DSP is alive and
 *    asserts its host-boot READY levels: HREQ/DSPDREQ (MAIN PE12, IC103
 *    pin 72 = PE12/RXD3 per the 10.2/10.14 schematic+waveform) plus the
 *    two gate inputs PE6 then PE8 that 0x0C0FCA7A/0x0C0FCBB0 poll with
 *    `and #64 / and #0x100` before each transfer.
 *
  *    Pin-behavior sources (public docs only, never committed):
  *    - DSP56371 User Manual Rev 2.1 §4.3/Table 4-2 + Appendix A: SHI-slave
  *      bootstrap (modes 5/7) runs the factory ROM program after RESET
  *      deasserts; the SHI operates in slave mode with the HREQ pin enabled
  *      for receive; the host clocks count+address+data words which the ROM
  *      stores to PRAM and then jumps to. In the UM's SHI context
  *      HREQ is an active-low OUTPUT: the LOW pulse is the per-transfer
  *      assertion (the UM deasserts it at the first clock pulse of the new
  *      transfer, §2.7), not the idle level.
  *    - DSP56371 datasheet Rev 4.1 §12 (SPI Fig 9-12, I2C §13/Fig 13):
  *      HREQ ready/ack timing around SCK/SCL edges.
  *    - Service manual f70e4d.pdf §10.14 panel 5 (IC103 pin 72 DSPDREQ,
  *      1.0 V/div, 20 us/div): the trace idles HIGH with a brief LOW pulse
  *      per transaction — i.e. the quiescent/ready level is HIGH (H), and
  *      LOW is only the momentary per-transfer assertion. §10.15 confirms
  *      the companion reset polarity (DSPRST H = AUDIO DSP reset released).
  *    Applied here as: READY (PE6/PE8/DSPDREQ = 1) once the DSP is alive.
  *    This matches BOTH the waveform idle level AND the observed driver
  *    behavior: gates 0x0C0FCA7A/0x0C0FCBB0 poll `and #64` then `and #0x100`
  *    (Task 8a §2e), i.e. they wait for PE6=1 then PE8=1 before each
  *    transfer — a LOW-means-ready line would spin them forever. There is
  *    no contradiction with the UM's "active-low": that names the transfer
  *    pulse polarity in SHI-bootstrap mode, while this DSP runs its PROM
  *    application behind MAIN's custom GPIO protocol (not SHI SPI), whose
  *    sampled ready level is HIGH.
  *    No DSP payload bytes are produced anywhere in this file.
 *
 * 2. RECORD (observation, not fabrication). Every MAIN-driven Port-E
 *    write (PEDRL data + PEIORL direction) is appended to
 *    $CDJ850_DSP_RECORD as {pc, ms, off, size, val} so the uploaded stream
 *    can be carved offline: the recorded bytes ARE the carve (Task 8c
 *    step 3). Reply DATA bits (the sampler's high nibble) are NOT driven:
 *    reads return the stored latch there (loopback), exactly as without a
 *    DSP — supplying assembled reply bytes would invent DSP payloads.
 *
 * 3. EXPERIMENTAL reply drive (Task 8g, user-authorized, default OFF).
 *    $CDJ850_DSP_REPLY=0xHL (H,L hex digits, each 0-7) makes the model
 *    drive MAIN's PE15:13 reply-DATA lines the way the real DSPC56371
 *    PROM application would: sampler 0x0C0FC8FA assembles its reply byte
 *    at 0x0C7F6D6C from two back-to-back Port-E reads (phase 1 =
 *    `mov.w @(r0,r11),r5` at guest pc=0x0C0FC922, phase 2 =
 *    `mov.w @(r0,r11),r14` at pc=0x0C0FC95C; each `and #0xF000, shad
 *    #-12`, byte = (phase1<<4)|phase2). The model answers phase 2
 *    with L and every other Port-E read (incl. the phase-1 read)
 *    with H (PE12/DSPDREQ stays READY=1 either way), so
 *    the assembled byte is ((H<<1|1)<<4)|(L<<1|1); all other Port-E
 *    reads see H on PE15:13 (the DSP's quiescent DATA). Unset (or any
 *    value outside 0x00-0x77) = OFF = pure loopback, byte-identical to
 *    having no DSP.
 *    Every driven value is traceable to a validator-read constant: the
 *    sample byte merges into the flag block at 0x0C7F6D5C (validator
 *    0x0C0FC7B8-7EA: assemble jsr @0x0C107D9C, and #0xF0, split jsr
 *    @0x0C107D6A, movu.b sample>>4 OR) whose bytes the validator
 *    compares against #1 (0x0C0FC7F4 on [0x0C7F6D5E],
 *    0x0C0FC836 on [0x0C7F6D67]), and the 10 collector bytes at
 *    0x0C7F6D1C feed 0x0C104D64's low-nibble {0,1,5} + nonzero checks
 *    (0x0C104D8C-DB2). Static search = H==L (8 values); the full bound
 *    is the single decisive byte 0x00-0x77 (64 physically distinct
 *    patterns under PE12=1). Ground truth = pending hardware capture,
 *    which REPLACES these values; until then they are the
 *    validator-satisfying set, never committed as always-on behavior.
 *
 * 4. EXPERIMENTAL PE12 pulse (Task 8h, user-authorized, default OFF).
 *    $CDJ850_DSP_PULSE=1 drives PE12 (DSPDREQ = DSP HREQ) LOW only
 *    during the sampler's two phase reads (guest pcs 0x0C0FC922 /
 *    0x0C0FC95C, the executing `mov.w @(r0,r11)` loads — see the
 *    PH1 off-by-two note below; the same PCs the reply drive
 *    discriminates on) and HIGH everywhere else. It composes with $CDJ850_DSP_REPLY: with
 *    both set, the assembled sample byte is ((H<<1|0)<<4)|(L<<1|0)
 *    (PE12=0 clears the LSB of each nibble); with pulse-only, the
 *    loopback latch's PE15:13 stand and only the PE12 LSB is forced
 *    to 0 at the two sampler PCs. Unset (any value but "1") = OFF =
 *    PE12 READY=1 always (Task 8g behavior). Gate-safety: PE6/PE8
 *    are never cleared by this path (the OR of CDJ850_DSP_PE6 /
 *    CDJ850_DSP_PE8 is unconditional once alive), and the sampler
 *    PCs are disjoint from the READY-gate PCs (gates 0x0C0FCA7A /
 *    0x0C0FCBB0, validator gates 0x0C0FC736-5A `and #64` / `and
 *    #0x100`), so the HIGH-waits keep passing; verified live (8h).
 *    Pin-behavior basis, quoted verbatim (reviewer finding from 8g:
 *    diff-quoted, not report-asserted):
 *    - DSP56371 User Manual Rev 2.1, Table 2-7 (Serial Host
 *      Interface Signals), HREQ signal description: "When
 *      configured for the slave mode, HREQ is asserted to indicate
 *      that the SHI is ready for the next data word transfer and
 *      deasserted at the first clock pulse of the new data word
 *      transfer."
 *    - DSP56371 User Manual Rev 2.1, section 7.7.1 "SPI Slave
 *      Mode" (p. 7-19): "The HREQ output pin, if enabled for
 *      receive (HRQE[1:0] = 01), is asserted when the IOSR is
 *      ready for receive and the HRX FIFO is not full; this
 *      operation guarantees that the next received data word is
 *      stored in the FIFO." ... "HREQ is deasserted at the first
 *      clock pulse of the next data word transfer."
 *    - Service manual f70e4d.pdf, section 10.14, p. 91, panel 5
 *      (IC103 pin 72 DSPDREQ, 1.0 V/div, 20 us/div): "the trace
 *      idles HIGH with a brief LOW pulse per transaction" (8c
 *      rendering) - i.e. the quiescent/ready level is HIGH, and
 *      LOW is only the momentary per-transfer assertion.
  *    Scope bound (8h brief): one micro-experiment, at most 2
  *    boots, verdict MOVED-or-NOTHING; no duty-cycle/timing
  *    sweeps, no other pins.
  *
  * 5. EXPERIMENTAL sequenced reply (Task 8j, user-authorized, default
  *    OFF). All prior driving was STATIC (same nibble every strobe),
  *    but the validator runs in a loop collecting 10 bytes and
  *    checking them in ORDER - so a scripted per-strobe SEQUENCE is
  *    still driver-justified and was entirely untried.
  *    $CDJ850_DSP_SEQ=order|echo|combo serves a scripted per-strobe
  *    PE15:13 nibble list cycling over a short pattern, advancing one
  *    step per sampler phase read (guest pcs 0x0C0FC922/0x0C0FC95C).
  *    Non-sampler Port-E reads are untouched (loopback/READY as
  *    before); static $CDJ850_DSP_REPLY is overridden ONLY at the
  *    sampler reads while a sequence is active; $CDJ850_DSP_PULSE
  *    still governs PE12 (the sequencer drives PE15:13 = 0xE0 bits
  *    only, never PE12/PE6/PE8). Unset (or any other value) = OFF =
  *    8g/8h static behavior, byte-identical. Invalid = warn + OFF.
  *    - order: 20-nibble cycle = 10 bytes in validator-check order:
  *      B0=0x51 (merge/[5E] hi=5 passes per the 8g table, collector
  *      byte0 lo=1 in {0,1,5}, 81 inside every range bound), B1=0x11,
  *      B2-B7=0x55, B8=0x55 (85<=153, <=95), B9=0x71 (113<117,
  *      [5E] hi=7). Nibbles (PE15:13) : 2,0,0,0,2,2,2,2,2,2,2,2,
  *      2,2,2,2,2,2,3,0 - every value in {0,2,3}, i.e. drawn from
  *      the 8g static set, no invented payload. (Branch-polarity
  *      correction vs 8g: re-derivation from the image shows the
  *      [02]==0 leg needs byte6==0 - unreachable under PE12=1
  *      (min byte 0x11) - while the [02]!=0 leg needs only
  *      byte0-lo in {0,1,5}; B0=0x51 satisfies the reachable half.)
  *    - echo: each element is a 3-bit window of the last MAIN-driven
  *      upload byte (low byte of the latest PEDRL write - the byte a
  *      real DSP would ACK/echo; documented assumption): step%3==0
  *      -> b[2:0] (identity-echo of the low bits), ==1 -> b[5:3],
  *      ==2 -> b[7:6] (0-3). Before any PEDRL write lands, reads
  *      stay loopback (nothing invented).
  *    - combo: phase-1 reads serve the order nibble (merge-driving
  *      hi from validator constants), phase-2 reads serve the echo
  *      window (ACK-like lo). Scope bound (8j brief): at most 3
  *      boots, verdict MOVED-or-NOTHING; no duty-cycle sweeps, no new
  *    pins, no payloads beyond validator constants + echo transforms.
 *
 * What this does NOT do (honest limits):
 * - No DSP56371 core executes yet: the opcode census needs a complete
 *   recorded image first (step 3), and the validator's accept leg needs
 *   reply bytes from genuine core execution (step 4). Static READY alone
 *   is already proven insufficient (Task 8a §2f straps) — this file
 *   provides the principled home for the handshake plus the record, and
 *   the boot is expected to keep retrying until a genuine reply exists.
 * - Servo DSP (IC201, Toshiba TC94A15FG, SRVRST-owned) is out of scope.
 */

/* Port-E offsets from 0xFFFE3800 (SH7263 manual §29/§30/§34). */
#define CDJ850_PEDRL_OFF    0x202   /* PEDRL data (word) */
#define CDJ850_PEIORL_OFF   0x206   /* PEIORL direction (word) */
/* DSP-driven input bits on PEDRL reads once alive. */
#define CDJ850_DSP_PE6      0x0040u /* gate input 1 (polled `and #64`) */
#define CDJ850_DSP_PE8      0x0100u /* gate input 2 (polled `and #0x100`) */
#define CDJ850_DSP_PE12     0x1000u /* DSPDREQ = DSP HREQ (UM SHI slave) */
/* Bring-up tracker states (PEDRL values from Task 8a §2e trace). */
#define CDJ850_BRINGUP_S40   0x0040u
#define CDJ850_BRINGUP_S140  0x0140u
#define CDJ850_BRINGUP_S1C0  0x01C0u

/* Record entry, little-endian, 20 bytes, packed (no alignment padding). */
typedef struct QEMU_PACKED {
    uint32_t pc;
    uint64_t ms;
    uint16_t off;
    uint8_t size;
    uint8_t reserved;
    uint32_t val;
} Cdj850DspRec;

/* Sampler 0x0C0FC8FA phase-read PCs (Port-E word reads whose high
 * nibble, PE15:12, becomes the reply byte at 0x0C7F6D6C).
 * NOTE (Task 8h correction): phase 1's executing load is the
 * `mov.w @(r0,r11),r5` at 0x0C0FC922, not the `mov r14,r0` setup at
 * 0x0C0FC920 named in the 8g report (self-caught: pulse-only
 * assembled 0x10 = ph1 nibble 1 = PE12 HIGH at the true read,
 * proving the clear missed). The 8g reply knob is insensitive to
 * this off-by-two: its default branch drives H on every non-PH2
 * Port-E read, which already covered the true ph1 read, so reply
 * behavior is byte-identical before/after the correction. */
#define CDJ850_SAMPLER_PH1_PC 0x0C0FC922u /* phase 1: byte bits 7:4 */
#define CDJ850_SAMPLER_PH2_PC 0x0C0FC95Cu /* phase 2: byte bits 3:0 */

typedef struct {
    bool alive;
    unsigned bringup;   /* 0, 0x40, 0x140 seen (in order) */
    uint16_t pedrl;     /* last full-word PEDRL value seen */
    FILE *rec;
    uint64_t n_rec;
    uint64_t n_reads;
    int reply_hi;       /* -1 = OFF (loopback); else 0-7, phase-1 PE15:13 */
    int reply_lo;       /* -1 = OFF (loopback); else 0-7, phase-2 PE15:13 */
    bool pulse_pe12;    /* Task 8h: PE12 LOW only at sampler phase reads */
    int seq_mode;       /* Task 8j: 0=OFF, 1=order, 2=echo, 3=combo */
    unsigned seq_pos;   /* per-strobe position (advances on sampler reads) */
    uint8_t last_sent;  /* low byte of latest MAIN PEDRL write (echo src) */
    bool have_sent;     /* a PEDRL byte has been snooped since reset */
} Cdj850Dsp;

static Cdj850Dsp cdj850_dsp;

void cdj850_dsp_init(void)
{
    const char *rep;
    const char *pulse;

    memset(&cdj850_dsp, 0, sizeof(cdj850_dsp));
    cdj850_dsp.reply_hi = -1;
    cdj850_dsp.reply_lo = -1;
    /* Experimental reply drive (Task 8g): $CDJ850_DSP_REPLY=0xHL,
     * H,L in 0-7. Default OFF. Parsed once; invalid = OFF + warn. */
    rep = getenv("CDJ850_DSP_REPLY");
    if (rep && *rep) {
        char *end = NULL;
        unsigned long k = strtoul(rep, &end, 0);

        if (end && *end == '\0' && k <= 0x77 &&
            ((k >> 4) & 0xF) <= 7 && (k & 0xF) <= 7) {
            cdj850_dsp.reply_hi = (int)((k >> 4) & 0xF);
            cdj850_dsp.reply_lo = (int)(k & 0xF);
            info_report("cdj850: dsp reply: EXPERIMENTAL drive ON "
                        "(CDJ850_DSP_REPLY=0x%02lx: PE15:13 ph1=%d ph2=%d, "
                        "PE12 READY stays 1; hardware capture replaces this)",
                        k, cdj850_dsp.reply_hi, cdj850_dsp.reply_lo);
        } else {
            info_report("cdj850: dsp reply: ignoring bad CDJ850_DSP_REPLY "
                        "'%s' (want 0x00-0x77), OFF", rep);
        }
    }
    /* Experimental PE12 pulse (Task 8h): $CDJ850_DSP_PULSE=1 drives
     * PE12 LOW only at the sampler phase reads, HIGH elsewhere.
     * Default OFF. Parsed once; any other value = OFF (no warn: the
     * unset case is the normal path). */
    pulse = getenv("CDJ850_DSP_PULSE");
    if (pulse && strcmp(pulse, "1") == 0) {
        cdj850_dsp.pulse_pe12 = true;
        info_report("cdj850: dsp pulse: EXPERIMENTAL PE12 pulse ON "
                    "(CDJ850_DSP_PULSE=1: PE12 LOW only at sampler "
                    "pcs 0x0C0FC922/0x0C0FC95C, HIGH elsewhere; "
                    "PE6/PE8 READY untouched; hardware capture "
                    "replaces this)");
    }
    /* Experimental sequenced reply (Task 8j): $CDJ850_DSP_SEQ =
     * order|echo|combo. Default OFF. Parsed once; invalid = OFF + warn. */
    {
        const char *seq = getenv("CDJ850_DSP_SEQ");

        if (seq && *seq) {
            if (strcmp(seq, "order") == 0) {
                cdj850_dsp.seq_mode = 1;
            } else if (strcmp(seq, "echo") == 0) {
                cdj850_dsp.seq_mode = 2;
            } else if (strcmp(seq, "combo") == 0) {
                cdj850_dsp.seq_mode = 3;
            } else {
                info_report("cdj850: dsp seq: ignoring bad CDJ850_DSP_SEQ "
                            "'%s' (want order|echo|combo), OFF", seq);
                cdj850_dsp.seq_mode = 0;
            }
            if (cdj850_dsp.seq_mode) {
                info_report("cdj850: dsp seq: EXPERIMENTAL sequence ON "
                            "(CDJ850_DSP_SEQ=%s: per-strobe PE15:13 pattern, "
                            "static REPLY overridden at sampler reads only; "
                            "hardware capture replaces this)", seq);
            }
        }
    }
}

static void cdj850_dsp_open_rec(void)
{
    const char *path;

    if (cdj850_dsp.rec) {
        return;
    }
    path = getenv("CDJ850_DSP_RECORD");
    if (!path || !*path) {
        return;
    }
    cdj850_dsp.rec = fopen(path, "wb");
    if (!cdj850_dsp.rec) {
        info_report("cdj850: dsp record: cannot open %s", path);
        return;
    }
    info_report("cdj850: dsp record: capturing Port-E writes to %s", path);
}

void cdj850_dsp_snoop_write(hwaddr off, uint64_t val, unsigned size,
                            uint32_t pc, uint64_t ms)
{
    /* Bring-up tracker: PEDRL 0 → 0x40 → 0x140 → 0x1C0 releases DSPRST. */
    if (!cdj850_dsp.alive &&
        (off == CDJ850_PEDRL_OFF ||
         (off < CDJ850_PEDRL_OFF &&
          off + size > CDJ850_PEDRL_OFF))) {
        unsigned shift = (CDJ850_PEDRL_OFF + 2 - off - size) * 8;
        uint16_t word = (uint16_t)((val >> shift) & 0xFFFFu);

        if (size == 2 && off == CDJ850_PEDRL_OFF) {
            cdj850_dsp.pedrl = word;
        }
        if (word == CDJ850_BRINGUP_S40) {
            cdj850_dsp.bringup = 0x40;
        } else if (word == CDJ850_BRINGUP_S140 &&
                   cdj850_dsp.bringup == 0x40) {
            cdj850_dsp.bringup = 0x140;
        } else if (word == CDJ850_BRINGUP_S1C0 &&
                   cdj850_dsp.bringup == 0x140) {
            cdj850_dsp.alive = true;
            info_report("cdj850: dsp bootstrap: DSPRST released "
                        "(PEDRL 0x40→0x140→0x1C0), HREQ/DSPDREQ+PE6/PE8 "
                        "READY asserted at pc=0x%08x %" PRIu64 " ms",
                        pc, ms);
        }
    }
    /* Task 8j echo source: low byte of the latest MAIN PEDRL write is
     * the most recent upload byte a real DSP would ACK/echo. Any write
     * overlapping the PEDRL low byte refreshes it (assumption: the DSP
     * echoes uploads; documented in the file header). */
    if (off <= CDJ850_PEDRL_OFF + 1 &&
        off + size > CDJ850_PEDRL_OFF + 1) {
        unsigned shift = (CDJ850_PEDRL_OFF + 2 - off - size) * 8;

        cdj850_dsp.last_sent = (uint8_t)((val >> shift) & 0xFFu);
        cdj850_dsp.have_sent = true;
    }
    /* Record MAIN-driven Port-E data/direction writes (both halves). */    if (off == CDJ850_PEDRL_OFF || off == CDJ850_PEIORL_OFF) {
        Cdj850DspRec r;

        cdj850_dsp_open_rec();
        if (cdj850_dsp.rec) {
            r.pc = pc;
            r.ms = ms;
            r.off = (uint16_t)off;
            r.size = (uint8_t)size;
            r.reserved = 0;
            r.val = (uint32_t)val;
            if (fwrite(&r, 1, sizeof(r), cdj850_dsp.rec) != sizeof(r)) {
                info_report("cdj850: dsp record: write failed");
                fclose(cdj850_dsp.rec);
                cdj850_dsp.rec = NULL;
            } else {
                cdj850_dsp.n_rec++;
                if ((cdj850_dsp.n_rec & 16383) == 0) {
                    fflush(cdj850_dsp.rec);
                }
            }
        }
    }
}

uint64_t cdj850_dsp_adjust_read(hwaddr off, unsigned size, uint64_t v,
                                uint32_t pc)
{
    unsigned i;

    if (!cdj850_dsp.alive) {
        return v;
    }
    cdj850_dsp.n_reads++;
    /* OR the DSP-driven READY levels over PEDRL-overlapping bytes.
     * Task 8h pulse: at the two sampler phase-read PCs with
     * CDJ850_DSP_PULSE=1, PE12 is driven LOW (cleared) while PE8
     * stays HIGH; everywhere else PE12 stays READY=1. PE6 (low
     * byte) is never touched by either path. */
    for (i = 0; i < size; i++) {
        hwaddr a = off + i;

        if (a == CDJ850_PEDRL_OFF) {
            /* High byte: PE8 (bit0 of byte) + PE12 (bit4 of byte). */
            unsigned shift = (size - 1 - i) * 8;

            if (cdj850_dsp.pulse_pe12 &&
                (pc == CDJ850_SAMPLER_PH1_PC ||
                 pc == CDJ850_SAMPLER_PH2_PC)) {
                v |= (uint64_t)(CDJ850_DSP_PE8 >> 8) << shift;
                v &= ~((uint64_t)((CDJ850_DSP_PE12 >> 8) & 0xFF) << shift);
            } else {
                v |= (uint64_t)(CDJ850_DSP_PE8 | CDJ850_DSP_PE12) >>
                     8 << shift;
            }
        } else if (a == CDJ850_PEDRL_OFF + 1) {
            /* Low byte: PE6. */
            v |= (uint64_t)CDJ850_DSP_PE6 << ((size - 1 - i) * 8);
        }
    }
    /* Experimental reply drive (Task 8g, default OFF): the DSP owns
     * PE15:13 DATA. The sampler's two phase reads get H/L; every other
     * Port-E read sees the quiescent H. PE12/READY is never cleared.
     * Task 8j: an active sequence overrides the static H/L at the
     * sampler reads (only there); echo elements need a snooped upload
     * byte first, else that strobe stays loopback. */
    {
        /* Order pattern: 10 validator-ordered bytes (B0=0x51, B1=0x11,
         * B2-B8=0x55, B9=0x71) as PE15:13 nibbles; see header §5. */
        static const int order_pat[20] = {
            2, 0, 0, 0, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2, 3, 0,
        };
        bool at_ph1 = (pc == CDJ850_SAMPLER_PH1_PC);
        bool at_ph2 = (pc == CDJ850_SAMPLER_PH2_PC);
        unsigned shift;

        if (cdj850_dsp.seq_mode && (at_ph1 || at_ph2)) {
            unsigned pos = cdj850_dsp.seq_pos++;
            int nib3 = -1;

            if (cdj850_dsp.seq_mode == 1) {
                nib3 = order_pat[pos % 20];
            } else if (cdj850_dsp.seq_mode == 2) {
                if (cdj850_dsp.have_sent) {
                    uint8_t b = cdj850_dsp.last_sent;

                    if (pos % 3 == 0) {
                        nib3 = b & 7;
                    } else if (pos % 3 == 1) {
                        nib3 = (b >> 3) & 7;
                    } else {
                        nib3 = (b >> 6) & 3;
                    }
                }
            } else {
                if (at_ph1) {
                    nib3 = order_pat[pos % 20];
                } else if (cdj850_dsp.have_sent) {
                    uint8_t b = cdj850_dsp.last_sent;

                    if (pos % 3 == 0) {
                        nib3 = b & 7;
                    } else if (pos % 3 == 1) {
                        nib3 = (b >> 3) & 7;
                    } else {
                        nib3 = (b >> 6) & 3;
                    }
                }
            }
            if (nib3 >= 0) {
                for (i = 0; i < size; i++) {
                    if (off + i != CDJ850_PEDRL_OFF) {
                        continue;
                    }
                    shift = (size - 1 - i) * 8;
                    v &= ~((uint64_t)0xE0 << shift);
                    v |= (uint64_t)((nib3 & 7) << 5) << shift;
                }
            }
        } else if (cdj850_dsp.reply_hi >= 0) {
            unsigned shift2;
            int nib3b;

            for (i = 0; i < size; i++) {
                if (off + i != CDJ850_PEDRL_OFF) {
                    continue;
                }
                shift2 = (size - 1 - i) * 8;
                nib3b = cdj850_dsp.reply_hi;
                if (pc == CDJ850_SAMPLER_PH2_PC) {
                    nib3b = cdj850_dsp.reply_lo;
                }
                v &= ~((uint64_t)0xE0 << shift2);
                v |= (uint64_t)((nib3b & 7) << 5) << shift2;
            }
        }
    }
    return v;
}
