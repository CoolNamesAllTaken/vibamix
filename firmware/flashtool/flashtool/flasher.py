"""probe-rs flashing backend: one function flashes one device.

This is the only module that knows about the programming tool, so swapping
probe-rs for nrfutil / J-Link later means touching only this file.
"""

from __future__ import annotations

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
    firmware: str,
    on_progress: ProgressCb,
) -> tuple[bool, str]:
    """Flash, verify, and reset one device.  Returns ``(ok, message)``."""
    cmd = _command(dev, firmware)
    on_progress(0.0, "starting")
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
                on_progress(pct, msg)
        rc = proc.wait(timeout=config.FLASH_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return False, "timeout"

    if rc != 0:
        return False, f"probe-rs exited {rc}"

    # Reset the target so the new firmware starts running.
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
