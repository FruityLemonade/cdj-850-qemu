# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850: pin the C850MAIN.UPD container profile values (size, hash, label)."""
import hashlib
import pathlib

C850 = pathlib.Path("/Users/julius/Documents/cdj850-emu/C850MAIN.UPD")


def test_c850_header():
    from c850_decode import find_main_label, read_upd
    data = read_upd(str(C850))
    assert len(data) == 2156300
    assert hashlib.sha256(data).hexdigest() == "12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7"
    assert find_main_label(data) == 0x60000
    assert data[0x60000:0x60000+12] == b"CDJ-850 MAIN"


def test_prepare_c850():
    # Real entry is launcher.firmware.prepare (brief's `run_steps` does not
    # exist); call signature mirrors test_prepare_names_the_model_on_a_hash_mismatch.
    from launcher import firmware, model
    m = model.load("cdj850")
    out = firmware.prepare(str(C850), model=m, say=lambda s: None)
    assert pathlib.Path(out, "extract", "main_flat.bin").is_file()


def test_c850_sh_coverage():
    from c850_sh_scan import sh2a_only_counts
    counts = sh2a_only_counts("/Users/julius/Documents/cdj850-emu/C850MAIN.UPD")
    assert counts, "scanner found no candidate SH-2A-only opcodes at all (scanner broken?)"


def test_c850_dsp_decode_synthetic():
    # Hand-built record sample: 3 byte-sends with known expected bytes
    # (synthetic vectors only — never firmware bytes), interleaved with
    # strobe/gate noise that the decoder must exclude.
    import struct
    from c850_dsp_decode import SITES, decode_image
    PEDRL, PEIORL = 0x202, 0x206
    CD0E_STROBE = (0x0C0FCD2C, 0x5080, 0x0C0FCD54, 0x5180)

    def word(hi_nib, clock):
        return ((hi_nib << 12) | 0x080 | (0x100 if clock else 0)) & 0xFFFF

    recs = []
    recs.append((0x0C0FD7CC, 6559, PEDRL, 2, 0x01C0))  # bring-up, ignored
    expected = bytearray()
    for hi, lo in ((0x3, 0xC), (0x0, 0x7), (0xA, 0x5)):
        w = [word(hi, False), word(hi, True), word(hi, False),
             word(lo, False), word(lo, True)]
        for pc, val in zip(SITES, w):
            recs.append((pc, 6811, PEDRL, 2, val))
        expected.append((hi << 4) | lo)
        # CD0E strobe pair + unrelated PEDRL/PEIORL noise between sends.
        recs.append((CD0E_STROBE[0], 6811, PEDRL, 2, CD0E_STROBE[1]))
        recs.append((CD0E_STROBE[2], 6811, PEDRL, 2, CD0E_STROBE[3]))
        recs.append((0x0C0FCA02, 6811, PEIORL, 2, 0xF1C0))
        recs.append((0x0C0FC918, 6811, PEDRL, 2, 0x50C0))
    assert decode_image(recs) == bytes(expected) == b"\x3c\x07\xa5"


def test_c850_unpack_map():
    # Packed-MAIN -> SDRAM decoder map (Task 8d): unpack the prepared
    # main_flat.bin and pin length + SHA (reads the local extract only,
    # never the read-only UPD; byte-matches live SDRAM code/rodata).
    import pathlib
    from c850_unpack import UNPACK_LEN, UNPACK_SHA256, decode_main
    import hashlib
    flat = pathlib.Path("extract/cdj850/main_flat.bin")
    declared, image = decode_main(str(flat))
    assert declared == 0x19E6E6
    assert len(image) == UNPACK_LEN == 0x39231E
    assert hashlib.sha256(image).hexdigest() == UNPACK_SHA256
