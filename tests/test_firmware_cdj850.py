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
