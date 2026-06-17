"""probe-rs flashing backend: one function flashes one device.

This is the only module that knows about the programming tool, so swapping
probe-rs for nrfutil / J-Link later means touching only this file.
"""

from __future__ import annotations

import os
import re
import subprocess
from collections.abc import Callable

from . import config
from .models import UsbDevice

# Called from the worker thread with (percent 0..100, human-readable phase).
ProgressCb = Callable[[float, str], None]

# probe-rs progress lines look like (with or without a TTY):
#      Erasing ✓ [00:00:01] [####] 131072/131072 @ 107 KiB/s
#   Programming ✓ [00:00:02] [####] 131072/131072 @ 36 KiB/s
#     Verifying ✓ [00:00:00] [####] 131072/131072 @ 512 KiB/s
#      Finished in 3.45s
_PHASE_RE = re.compile(r"(Erasing|Programming|Verifying|Finished)", re.IGNORECASE)
_FRAC_RE = re.compile(r"(\d+)\s*/\s*(\d+)")

# Each phase occupies one third of the 0–99 range; 100 is set on clean exit.
_PHASE_BASE = {"erasing": 0.0, "programming": 33.0, "verifying": 66.0}


def _command(dev: UsbDevice, firmware: str) -> list[str]:
    """Build the probe-rs download command for a single probe."""
    exe = config.PROBE_RS_WRAPPER or config.PROBE_RS_BIN
    return [exe, "download", "--chip", config.CHIP,
            "--binary-format", "hex",
            "--probe", dev.selector(), firmware]


def _reset_command(dev: UsbDevice) -> list[str]:
    exe = config.PROBE_RS_WRAPPER or config.PROBE_RS_BIN
    return [exe, "reset", "--chip", config.CHIP, "--probe", dev.selector()]


def flash_device(
    dev: UsbDevice,
    firmwares: str | list[str],
    on_progress: ProgressCb,
) -> tuple[bool, str]:
    """Flash an ordered list of images, then reset once.  Returns ``(ok, message)``.

    ``firmwares`` is one or more hex paths flashed in order (e.g. bootloader then
    app).  They must target non-overlapping flash regions: probe-rs ``download``
    only erases the sectors each image covers, so later images don't wipe earlier
    ones.  Overall progress is split evenly across the images.
    """
    images = [firmwares] if isinstance(firmwares, str) else list(firmwares)
    if not images:
        return False, "no firmware images to flash"

    n = len(images)
    on_progress(0.0, "starting")
    for i, fw in enumerate(images):
        name = _image_name(fw)
        ok, msg = _flash_one_image(dev, fw, name, i, n, on_progress)
        if not ok:
            return False, msg

    # Reset the target once, after the last image, so the new firmware runs.
    try:
        subprocess.run(
            _reset_command(dev),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except Exception:
        pass  # non-fatal — the flash succeeded even if reset fails

    on_progress(100.0, "done")
    return True, "ok"


def _image_name(firmware: str) -> str:
    """Short label for progress messages — the bootloader build dir vs. the app."""
    norm = os.path.normpath(firmware)
    bl_dir = os.path.normpath(str(config.BOOTLOADER_BUILD_DIR))
    return "bootloader" if norm.startswith(bl_dir + os.sep) else "app"


def _flash_one_image(
    dev: UsbDevice,
    firmware: str,
    name: str,
    index: int,
    total: int,
    on_progress: ProgressCb,
) -> tuple[bool, str]:
    """Run one ``probe-rs download``, scaling its 0–100 onto this image's band."""
    cmd = _command(dev, firmware)
    span = 100.0 / total
    base = index * span

    def scaled(pct: float, msg: str) -> None:
        on_progress(base + (pct / 100.0) * span, f"{name}: {msg}")

    scaled(0.0, "starting")
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except FileNotFoundError:
        return False, "probe-rs not found — install via 'cargo install probe-rs-tools'"

    try:
        for line in _read_lines(proc.stdout):
            pct, msg = _parse_progress(line)
            if pct is not None:
                scaled(pct, msg)
        rc = proc.wait(timeout=config.FLASH_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return False, f"{name}: timeout"

    if rc != 0:
        return False, f"{name}: probe-rs exited {rc}"
    return True, "ok"


def _parse_progress(line: str) -> tuple[float | None, str]:
    """Extract ``(percent, phase)`` from one line of probe-rs output.

    Returns ``(None, line)`` for lines that carry no progress information.
    """
    m = _PHASE_RE.search(line)
    if not m:
        return None, line.strip()

    phase = m.group(1).lower()
    if phase == "finished":
        return 99.0, "finishing"

    base = _PHASE_BASE.get(phase, 0.0)
    frac = _FRAC_RE.search(line)
    if frac:
        done, total = int(frac.group(1)), int(frac.group(2))
        within = (done / total) * 33.0 if total else 0.0
        return base + within, phase

    return base, phase


def _read_lines(stream):
    """Yield non-empty lines split on both \\n and \\r.

    probe-rs uses \\r to overwrite progress lines in a terminal.  When output
    is piped, both terminators can appear, so we split on both.
    """
    buf = ""
    while True:
        ch = stream.read(1)
        if not ch:
            if buf.strip():
                yield buf
            break
        if ch in ("\n", "\r"):
            if buf.strip():
                yield buf
            buf = ""
        else:
            buf += ch
