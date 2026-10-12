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
