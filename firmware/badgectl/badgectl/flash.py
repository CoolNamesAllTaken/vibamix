"""probe-rs flashing backend: one function flashes one probe.

Ported/trimmed from flashtool's ``flasher.py``. The only module that knows about
the programming tool, so swapping probe-rs for nrfutil / J-Link later means
touching only this file.

Unlike flashtool (which flashed the plain app ``zephyr.hex``), the app image here
is the *trailered* ``slotA.bin`` flashed as raw ``bin`` at the slot-A offset: the
custom direct-XIP bootloader CRC-verifies the slot via its 32-byte VIMG trailer
before booting, so a plain (un-trailered, address-less hex) app would only run
under an attached debugger, not standalone. See firmware/README.md.
"""

from __future__ import annotations

import os
import re
import struct
import subprocess
import tempfile
from collections.abc import Callable
from dataclasses import dataclass

from . import flashconfig

# Called from the worker thread with (percent 0..100, human-readable phase).
ProgressCb = Callable[[float, str], None]

# probe-rs progress lines look like (with or without a TTY):
#      Erasing ✓ [00:00:01] [####] 131072/131072 @ 107 KiB/s
#   Programming ✓ [00:00:02] [####] 131072/131072 @ 36 KiB/s
#     Verifying ✓ [00:00:00] [####] 131072/131072 @ 512 KiB/s
#      Finished in 3.45s
_PHASE_RE = re.compile(r"(Erasing|Programming|Verifying|Finished)", re.IGNORECASE)
_FRAC_RE = re.compile(r"(\d+)\s*/\s*(\d+)")
# Each phase occupies one third of an image's 0–99 band.
_PHASE_BASE = {"erasing": 0.0, "programming": 33.0, "verifying": 66.0}


@dataclass(frozen=True)
class Image:
    """One firmware image to flash.

    ``fmt`` is ``"hex"`` (self-addressing Intel HEX) or ``"bin"`` (raw, needs
    ``address``). ``name`` is a short label used in progress messages.
    """

    path: str
    fmt: str
    name: str
    address: int | None = None


def _download_cmd(selector: str, img: Image) -> list[str]:
    cmd = [flashconfig.PROBE_RS_BIN, "download", "--chip", flashconfig.CHIP,
           "--binary-format", img.fmt]
    if img.address is not None:
        cmd += ["--base-address", hex(img.address)]
    cmd += ["--probe", selector, img.path]
    return cmd


def _reset_cmd(selector: str) -> list[str]:
    return [flashconfig.PROBE_RS_BIN, "reset", "--chip", flashconfig.CHIP,
            "--probe", selector]


def _write_factory_id(selector: str, factory_id: int) -> tuple[bool, str]:
    """Write the 8-byte factory record (magic, id, ~id) to the factory partition."""
    if not (1 <= factory_id <= flashconfig.FACTORY_ID_MAX):
        return False, f"factory id {factory_id} out of range"

    blob = struct.pack("<IHH", flashconfig.FACTORY_ID_MAGIC, factory_id,
                       (~factory_id) & 0xFFFF)
    path = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(blob)
            path = f.name
        img = Image(path, "bin", "factory id", flashconfig.FACTORY_ID_ADDR)
        proc = subprocess.run(
            _download_cmd(selector, img),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        if proc.returncode != 0:
            return False, f"factory id: probe-rs exited {proc.returncode}"
        return True, "ok"
    except FileNotFoundError:
        return False, "probe-rs not found"
    except subprocess.TimeoutExpired:
        return False, "factory id: timeout"
    finally:
        if path:
            try:
                os.unlink(path)
            except OSError:
                pass


def flash_device(
    selector: str,
    images: list[Image],
    on_progress: ProgressCb,
    factory_id: int | None = None,
) -> tuple[bool, str]:
    """Flash an ordered list of images to one probe, then reset once.

    Returns ``(ok, message)``. Images must target non-overlapping flash regions:
    probe-rs ``download`` only erases the sectors each image covers, so later
    images don't wipe earlier ones. Overall progress is split evenly across them.

    If ``factory_id`` is given, the per-unit factory record is written after the
    images and before the reset, so the board boots with its assigned id.
    """
    if not images:
        return False, "no firmware images to flash"

    n = len(images)
    on_progress(0.0, "starting")
    for i, img in enumerate(images):
        ok, msg = _flash_one(selector, img, i, n, on_progress)
        if not ok:
            return False, msg

    if factory_id is not None:
        on_progress(99.0, "factory id")
        ok, msg = _write_factory_id(selector, factory_id)
        if not ok:
            return False, msg

    # Reset the target once, after the last image, so the new firmware runs.
    try:
        subprocess.run(_reset_cmd(selector), stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=10)
    except Exception:
        pass  # non-fatal — the flash succeeded even if reset fails

    on_progress(100.0, "done")
    return True, "ok"


def _flash_one(
    selector: str,
    img: Image,
    index: int,
    total: int,
    on_progress: ProgressCb,
) -> tuple[bool, str]:
    """Run one ``probe-rs download``, scaling its 0–100 onto this image's band."""
    span = 100.0 / total
    base = index * span

    def scaled(pct: float, msg: str) -> None:
        on_progress(base + (pct / 100.0) * span, f"{img.name}: {msg}")

    scaled(0.0, "starting")
    try:
        proc = subprocess.Popen(
            _download_cmd(selector, img),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    except FileNotFoundError:
        return False, "probe-rs not found — install via 'cargo install probe-rs-tools'"

    try:
        for line in _read_lines(proc.stdout):
            pct, msg = _parse_progress(line)
            if pct is not None:
                scaled(pct, msg)
        rc = proc.wait(timeout=flashconfig.FLASH_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return False, f"{img.name}: timeout"

    if rc != 0:
        return False, f"{img.name}: probe-rs exited {rc}"
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
    """Yield non-empty lines split on both ``\\n`` and ``\\r``.

    probe-rs uses ``\\r`` to overwrite progress lines in a terminal; when output
    is piped, both terminators can appear, so split on both.
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
