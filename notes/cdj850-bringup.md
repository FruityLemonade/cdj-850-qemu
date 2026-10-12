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
