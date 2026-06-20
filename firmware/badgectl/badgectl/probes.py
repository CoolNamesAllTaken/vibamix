"""Enumerate attached CMSIS-DAP probes via ``probe-rs list``.

This replaces flashtool's pyusb/USB-topology enumeration: ``probe-rs list``
already prints exactly the ``VID:PID:Serial`` selector that ``--probe`` wants and
the serial the factory-id registry keys on. No libusb, no hub maps — just a flat
list of whatever is plugged in.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass

from . import flashconfig

# Each device line from `probe-rs list` looks like:
#   [0]: Seeed Studio XIAO nrf54 CMSIS-DAP -- 2886:0066-1:65256195 (CMSIS-DAP)
# The `-1` after the pid is a display-only HID-interface index (absent on some
# probes/versions); we skip it and key on vid:pid:serial. Capture the human name
# before "--".
_LINE_RE = re.compile(
    r"^\s*\[\d+\]:\s*(?P<name>.*?)\s*--\s*"
    r"(?P<vid>[0-9a-fA-F]{4}):(?P<pid>[0-9a-fA-F]{4})(?:-\d+)?(?::(?P<serial>[^\s()]+))?",
)


@dataclass(frozen=True)
class Probe:
    """One attached debug probe."""

    vid: str
    pid: str
    serial: str | None
    name: str

    @property
    def selector(self) -> str:
        """The probe-rs ``--probe`` selector: ``VID:PID`` or ``VID:PID:Serial``."""
        base = f"{self.vid}:{self.pid}"
        return f"{base}:{self.serial}" if self.serial else base

    @property
    def label(self) -> str:
        return self.serial or self.selector


def parse(text: str) -> list[Probe]:
    """Parse the stdout of ``probe-rs list`` into Probe records."""
    probes: list[Probe] = []
    for line in text.splitlines():
        m = _LINE_RE.match(line)
        if not m:
            continue
        probes.append(Probe(
            vid=m.group("vid").lower(),
            pid=m.group("pid").lower(),
            serial=m.group("serial"),
            name=m.group("name").strip(),
        ))
    return probes


def list_probes() -> list[Probe]:
    """Return every probe ``probe-rs list`` currently reports (empty on none)."""
    try:
        proc = subprocess.run(
            [flashconfig.PROBE_RS_BIN, "list"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=15,
        )
    except FileNotFoundError as e:
        raise RuntimeError(
            "probe-rs not found — install via 'cargo install probe-rs-tools'") from e
    except subprocess.TimeoutExpired as e:
        raise RuntimeError("probe-rs list timed out") from e
    return parse(proc.stdout)
