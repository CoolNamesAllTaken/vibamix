"""Core data types shared across the app."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


class Status(Enum):
    """Lifecycle of a single slot."""

    ABSENT = auto()     # no device plugged into this port
    IDLE = auto()       # device present, not yet flashed
    FLASHING = auto()
    VERIFYING = auto()
    DONE = auto()
    ERROR = auto()


@dataclass
class UsbDevice:
    """A programmer (CMSIS-DAP probe) discovered on the hub."""

    port_path: str            # "bus-port.port" topology id — deterministic per socket
    vid: int
    pid: int
    serial: str | None
    product: str | None = None

    def selector(self) -> str:
        """Return the probe-rs ``--probe`` selector ``"VID:PID:Serial"``."""
        return f"{self.vid:04x}:{self.pid:04x}:{self.serial}"


@dataclass
class Slot:
    """One UI row, permanently bound to a physical hub port."""

    index: int                # slot number shown to the user
    port_path: str            # the physical port this slot represents
    device: UsbDevice | None = None
    status: Status = Status.ABSENT
    progress: float = 0.0     # 0..100
    message: str = ""
