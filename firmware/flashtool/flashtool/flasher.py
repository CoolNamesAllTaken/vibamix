"""probe-rs flashing backend: one function flashes one device.

This is the only module that knows about the programming tool, so swapping
probe-rs for nrfutil / J-Link later means touching only this file.
"""

from __future__ import annotations

from collections.abc import Callable

from . import config
from .models import UsbDevice

# Called from the worker thread with (percent 0..100, human-readable phase).
ProgressCb = Callable[[float, str], None]


def _command(dev: UsbDevice, firmware: str) -> list[str]:
    """Build the probe-rs download command for a single probe."""
    # pseudocode:
    #   base = [config.PROBE_RS_WRAPPER or config.PROBE_RS_BIN]
    #   return base + ["download", "--chip", config.CHIP,
    #                  "--probe", dev.selector(), firmware]
    raise NotImplementedError


def flash_device(dev: UsbDevice, firmware: str, on_progress: ProgressCb) -> tuple[bool, str]:
    """Flash + verify + reset one device. Returns ``(ok, message)``.

    Pseudocode
    ----------
    1. on_progress(0, "starting")
    2. proc = subprocess.Popen(_command(dev, firmware),
                               stdout=PIPE, stderr=STDOUT, text=True)
    3. for line in proc.stdout:                 # stream, don't block
           pct, msg = _parse_progress(line)
           if pct is not None: on_progress(pct, msg)
    4. rc = proc.wait(timeout=config.FLASH_TIMEOUT_S)
    5. if rc != 0: return (False, f"probe-rs exited {rc}")
    6. run `probe-rs reset --chip CHIP --probe <selector>`
    7. on_progress(100, "done"); return (True, "ok")

    Error handling:
      - subprocess.TimeoutExpired -> proc.kill(); return (False, "timeout")
      - FileNotFoundError         -> return (False, "probe-rs not found")
    """
    raise NotImplementedError


def _parse_progress(line: str) -> tuple[float | None, str]:
    """Extract ``(percent, phase)`` from one line of probe-rs output.

    Returns ``(None, line)`` for lines without a usable percentage. Phases of
    interest: "Erasing", "Programming", "Verifying".
    """
    # pseudocode: regex for "(Erasing|Programming|Verifying).*?(\d+)%"
    raise NotImplementedError
