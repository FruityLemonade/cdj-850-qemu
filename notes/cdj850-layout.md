# CDJ-850 MAIN container layout (C850MAIN.UPD)

Source: `/Users/julius/Documents/cdj850-emu/C850MAIN.UPD` (read-only, never committed).
Container: flat big-endian binary flash image, no manifest, no S-records.
Profile: size 2156300 bytes, SHA-256
`12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7`.
Label: 32-byte field at flat offset 0x60000:
`CDJ-850 MAIN` + `1.13` (cf. `main_decode.LABEL_SIZE`).

Carved with `scripts/firmware/c850_decode.py::carve_regions()`:

| region | flat range          | size (bytes) | size (hex) | SHA-256                                                        |
|--------|---------------------|--------------|------------|----------------------------------------------------------------|
| boot   | 0x000000–0x040000   | 262144       | 0x40000    | eb4f9f9885d6157d7521b13a18b20f685706152fabfe3ef2c3f6c9a5cd51d58c |
| erased | 0x040000–0x060000   | 131072       | 0x20000    | b5a41c3758763bbec72769fab4a2533bf2db0b6312d93d25a695f9e4b9e02260 |
| main   | 0x060000–0x200000   | 1703936      | 0x1A0000   | 51457aadf771fb7027569df8e3def364f0e456c7a2af2ec03a51b43efbd5c2de |
| tail   | 0x200000–0x20E70C   | 59148        | 0xE70C     | d9fca9e35615d60b4f50471d08483ff3af7363aabe41a2f44f14ec36d5fd8080 |

Survey assertions (2026-10-12, re-verified here):

- `erased` region is all `0xFF` (erased flash).
- `tail` region is all `0x00`.
- `main` region starts with the 32-byte label at its offset 0 (flat 0x60000).

`main` SHA-256 above was computed with
`python3 -c "import hashlib; print(hashlib.sha256(open('main.bin','rb').read()).hexdigest())"`
on the carved output in a temp dir (never under `extract/`).

## DSP init image (Task 8c, RECORDED — not carved from flash)

Source: MAIN's Port-E bit-bang upload driver, observed live through the
`dsp56371.c` record buffer (`CDJ850_DSP_RECORD`), three independent boots
(20 s / 100 s / 25 s) — every byte driven by MAIN, none invented.
Decode: byte-send `0x0C0FCC68` emits 5 PEDRL words per call
(`0x0C0FCC86/0xB2/0xC4/0xD6/0xF8` sites); byte =
`((w[0]>>12)&0xF)<<4 | ((w[3]>>12)&0xF`.

- Bytes: 519 (259 `(index,data)` control pairs + trailing `0x00` pad).
  Head pairs `(139,160),(140,128),(144,13),(145,136),…` match the static
  `0x0C0FDA2C` 36-pair sequencer indices (Task 8b §1e) with LIVE data
  (authoritative over the static delay-slot parse where they differ);
  body walks banks `0xF0–0xF8` then `0xD7` with a repeated offset
  sequence — a DSP control-register/memory fill, NOT DSP56371 opcodes
  (24-bit-word census: 173/173 distinct, no vectors/branches/NOP —
  address-structured init data).
- SHA-256: `7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064`.
- Local copy (untracked, reproducible via the record env):
  `extract/cdj850/dsp_init_image.bin` (519 B).
- Census decision: opcode census N/A (not code) → no
  interpreter-vs-vendor core call from this image; the genuine reply
  needs DSP-side execution semantics (Task 8c report).

## Packed-MAIN → SDRAM decoder map (Task 8d, VERIFIED)

Carved with `scripts/firmware/c850_unpack.py::decode_main()` (same
Okumura/Yoshizaki codec as `lzss_decode.py`: ring 4096, init `0x20`,
pos 4078; unpack the whole `main_flat.bin`, not the stream alone —
the 6390-B pad is consumed by the decoder overrun exactly as the
bootloader consumes it):

- Packed field: `main_flat` `0x20–0x19E70A` (4-B BE32 length
  `0x0019E6E6` + stream) = UPD flat `0x060020–0x1FE70A`.
- Unpacked: SDRAM `0x0C000000–0x0C39231E` (3744542 B, `0x39231E`).
- SHA-256 (unpacked):
  `26c8bfe933afc1e08a05dc65513303f1343a570d1e87f2734d60316e87c01591`.
- Remainder: `main_flat` `0x19E70A–0x1A0000` (6390 B checksum + zero
  pad; head bytes `FB71`, rest zeros).
- Verification (2026-10-13): output matches a live-boot 32 MB SDRAM
  dump over code/rodata (spot-checked at `0/0x100/0x1000/0x10000/
  0x100000/0x18BF2C`; BSS/heap diverge at runtime from `0x1C14DC`
  as expected); driver code (`0x0C0FC9DC`, gates, send, sampler,
  `0x0C0FCD6C–0x0C0FCE52`, callers) disassembles identically from
  the unpack as from live SDRAM dumps.
- Negative search (same date): the 519-B init image (head
  `8BA08C80…`) and the 284-B `0x0CE4B6F4` candidate header
  (`000B000000330057…`) are both ABSENT from the packed AND
  unpacked bytes — no contiguous DSP program blob lives in MAIN.
  (The candidate sits at SDRAM offset `0xE4B6F4`, beyond the
  `0x39231E` image — runtime heap, not image.)
