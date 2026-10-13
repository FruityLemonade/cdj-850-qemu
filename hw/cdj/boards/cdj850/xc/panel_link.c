/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "panel_link.h"
#include "trace.h"
#include "vfd.h"

/*
 * CDJ-850 MAIN<->panel link model (panel CPU side).
 *
 * Hardware (service manual signal block diagram, connector CN2003): MAIN
 * (IC103, SH7263) talks to the panel CPU (IC1007) over clock-synchronous
 * serial plus MAIN_XBUSY/PNL_XBUSY handshake GPIOs. The SH7263 manual
 * identifies the serial block MAIN uses, 0xFFFE7000, as SSU channel 0
 * (SSCRH/SSCRL/SSMR/SSER/SSSR/SSTDR/SSRDR), and the handshake input the
 * firmware parks on, 0xFFFE391E bit 2, as Port C pin PC2 (PCPRL).
 *
 * What MAIN's drivers require (all verified against live SDRAM code):
 * - Boot upload (driver 0x0C077C3C): SSU master, clock-synchronous,
 *   MSB-first, TE only; 1140 TX bytes through SSTDR0 (+6) polling SSSR
 *   TDRE (bit 2). TX-only; no receive slot is ever polled here.
 * - Panel commands (driver 0x0C0781xx, 16-bit, 3x): words 0x1604, 0x10FF,
 *   0x11FF through SSTDR0/1 (+6/+7), TEND-polled. No receive slot is read.
 * - Panel exchange (driver 0x0C077570 wrapping 0x0C0777xx, full duplex,
 *   DjControlTask/cddabuffTask): per byte, TDRE-poll, write SSTDR0,
 *   RDRF-poll (SSSR bit 1), read SSRDR0 (+0x0A) into the caller's buffer,
 *   RDRF clears on the read (SSU manual 16.3.5). The SSU driver itself
 *   stores every byte unconditionally, but the CALLERS parse the 6-byte
 *   reply frame and gate progress on it:
 *     DjControlTask (0x0C014822): RX[3] & 0x0F, RX[4] and RX[5] must ALL
 *       be nonzero, else the exchange is discarded and the C2 poll
 *       repeats forever (observed: 247 identical polls with zeroed
 *       replies). A liveness gate: the panel must answer with real
 *       status, not silence.
 *     cddabuffTask (0x0C005F6E): if RX[1] and RX[2] are both nonzero and
 *       RX[0] bit 7 is clear, RX[0]'s high nibble (0x40/0x20/0x10) routes
 *       the big-endian value RX[1]<<8|RX[2] to a status word selected by
 *       the low nibble (0/1/2); RX[0] == 0 dispatches nothing and falls
 *       through to the same tail. So RX[0..2] == 0 ("no event to report")
 *       keeps cddabuff on its idle path either way.
 *   The served frame is therefore {0x00,0x00,0x00,0x01,0x01,0x01}: "no
 *   event" plus minimal nonzero status. Every byte of it is justified by
 *   the parse above; key-press events (RX[0] high nibble + value) are
 *   Task 9's via panel_send_key once a key is actually injected (until
 *   then the panel is idle and reports none).
 * - Handshake ACK (driver 0x0C08E8AE): waits for PC2 = 1 (xc/port.c
 *   straps it; the level is what the driver's own comparison dictates).
 *
 * Identity note: SCIF0 (0xFFFE8000, RX interrupt armed) is a separate
 * ASCII command console ("?", digits, CR per its RX handler), not the
 * panel link; SCIF1 (0xFFFE8800) is TX-side only here. Neither is modelled
 * in this task.
 */

#define PANEL_TX_RING 4096

/* Idle reply frame for the 6-byte status exchange, per the parse above. */
/* TODO(Task 8/9-a): DjControl accept-path stores (0x0C7F663C flag, 0x0C7F65BC
 * dst) read zero despite the gate at 0x0C014822 passing -- either a different
 * live reader among the 6 buffer refs or per-cycle clearing. Task 8 should
 * watch the store writers; park-equivalent here (periodic poll, no wedge). */
/* TODO(Task 9-b): RX-buffer tail byte 0x13 (byte 7) sits beyond consumed
 * [0..5] -- readers only consume [0..5]; if a longer frame exists it is
 * unmapped. Task 9 inherits the tail-byte question with key/FL mapping. */
static const uint8_t panel_idle_frame[6] =
    { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01 };

static struct {
    uint8_t tx_ring[PANEL_TX_RING];
    unsigned tx_head;
    unsigned long tx_count;
    unsigned rx_pos;
    uint8_t pending_key;
    bool key_pending;
    bool key_logged;
    bool rx_logged;
} panel;

void panel_exchange_start(void)
{
    panel.rx_pos = 0;
}

uint8_t panel_ssu_rx_byte(void)
{
    uint8_t b;

    if (!panel.rx_logged) {
        panel.rx_logged = true;
        info_report("cdj850: panel link serving status frames "
                    "({00 00 00 01 01 01} idle)");
    }
    if (panel.key_pending && !panel.key_logged) {
        /*
         * A key is queued (panel_send_key, Task 9's hook), but the
         * key-event frame format is not mapped yet: serving a stray
         * byte would desync the 6-byte frame the callers parse. Hold
         * the idle frame until the format is known.
         */
        panel.key_logged = true;
        info_report("cdj850: panel link key 0x%02x queued but key-event "
                    "format unmapped; holding idle frame",
                    panel.pending_key);
    }
    b = panel_idle_frame[panel.rx_pos];
    panel.rx_pos = (panel.rx_pos + 1) % sizeof(panel_idle_frame);
    return b;
}

void panel_ssu_tx_byte(uint8_t b)
{
    panel.tx_ring[panel.tx_head] = b;
    panel.tx_head = (panel.tx_head + 1) % PANEL_TX_RING;
    panel.tx_count++;
    vfd_note_tx(b);
}

void panel_send_key(uint8_t code)
{
    panel.pending_key = code;
    panel.key_pending = true;
    panel.key_logged = false;
}
