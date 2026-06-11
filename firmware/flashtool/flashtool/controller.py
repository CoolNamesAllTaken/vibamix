"""Orchestration: discovery + concurrent flashing, decoupled from the GUI.

The controller never touches widgets. It reports everything by posting events
back to the GUI thread via ``window.write_event_value`` (thread-safe in
FreeSimpleGUI), keeping worker threads and the UI cleanly separated.
"""

from __future__ import annotations

import time
import threading
from concurrent.futures import ThreadPoolExecutor

from . import config, flasher, usb_topology
from .models import Slot

# Event keys posted to the GUI event loop.
EVT_PROGRESS    = "-DEV-PROGRESS-"  # value = (slot_index, pct, message)
EVT_DEVICE_DONE = "-DEV-DONE-"      # value = (slot_index, ok, message)
EVT_ALL_DONE    = "-ALL-DONE-"      # value = summary dict
EVT_HOTPLUG     = "-HOTPLUG-"       # value = list[Slot] — devices changed within same hub topology
EVT_REBUILD     = "-REBUILD-"       # value = list[Slot] — hub count/roots changed, window must rebuild


class Controller:
    """Owns the slot list and drives discovery + flashing."""

    def __init__(self) -> None:
        self.slots: list[Slot] = []
        self._relative_map: dict[int, str] | None = config.RELATIVE_MAP
        self._hub_roots: list[str] = []

    @property
    def num_hubs(self) -> int:
        """Number of hub instances currently reflected in self.slots."""
        if self._relative_map and self.slots:
            return len(self.slots) // len(self._relative_map)
        return max(1, len(self._hub_roots))

    def _detect_hub_roots(self, probes: list) -> list[str]:
        if self._relative_map:
            return usb_topology.find_hub_roots_from_map(probes, self._relative_map)
        return usb_topology.find_hub_roots(probes)

    def load_config(self, path: "Path") -> list[Slot]:
        """Switch to a different bench config file and re-discover."""
        self._relative_map = config.load_bench_config_from(path)
        self._hub_roots = []  # reset so new config discovers from scratch
        return self.discover()

    def discover(self) -> list[Slot]:
        """(Re)enumerate all hubs and bind devices to slots. Fast; main-thread ok.

        Hub roots only grow during a session: once a root is seen it is kept even
        if all boards on that hub are temporarily removed, so the column layout
        stays stable while boards are swapped in and out.
        """
        probes = usb_topology.enumerate_probes()
        fresh = self._detect_hub_roots(probes)
        combined = sorted(
            set(self._hub_roots) | set(fresh), key=usb_topology.path_sort_key
        )
        self._hub_roots = combined
        num_hubs = max(1, len(combined))
        self.slots = usb_topology.build_slots(self._relative_map, combined, num_hubs)
        usb_topology.bind_devices(self.slots, probes)
        return self.slots

    def resolve_firmware(self) -> str:
        """Pick the firmware image from config.ARTIFACT_DIR (newest glob match)."""
        # pseudocode:
        #   matches = sorted(config.ARTIFACT_DIR.glob(config.FIRMWARE_GLOB),
        #                    key=lambda p: p.stat().st_mtime)
        #   if not matches: raise FileNotFoundError(...)
        #   return str(matches[-1])
        raise NotImplementedError

    def flash_all(self, window) -> None:
        """Kick off flashing every present slot WITHOUT blocking the GUI."""
        # pseudocode:
        #   firmware = self.resolve_firmware()
        #   threading.Thread(target=self._run_pool, args=(window, firmware),
        #                    daemon=True).start()
        raise NotImplementedError

    def _run_pool(self, window, firmware: str) -> None:
        """Dispatcher thread: fan out one worker per present slot, bounded pool."""
        # pseudocode:
        #   present = [s for s in self.slots if s.device is not None]
        #   with ThreadPoolExecutor(max_workers=config.MAX_CONCURRENT) as ex:
        #       futures = [ex.submit(self._flash_one, window, s, firmware)
        #                  for s in present]
        #       wait(futures)
        #   window.write_event_value(EVT_ALL_DONE, self._summary())
        raise NotImplementedError

    def _flash_one(self, window, slot: Slot, firmware: str) -> None:
        """Worker: flash a single slot, streaming progress as GUI events."""
        # pseudocode:
        #   def cb(pct, msg):
        #       window.write_event_value(EVT_PROGRESS, (slot.index, pct, msg))
        #   ok, msg = flasher.flash_device(slot.device, firmware, cb)
        #   window.write_event_value(EVT_DEVICE_DONE, (slot.index, ok, msg))
        raise NotImplementedError

    def _summary(self) -> dict:
        """Aggregate per-slot results into a final summary for EVT_ALL_DONE."""
        # pseudocode: count DONE vs ERROR vs ABSENT across self.slots
        raise NotImplementedError

    def start_hotplug_monitor(self, window, interval: float = 1.5) -> None:
        """Start a daemon thread that polls for USB changes and posts events.

        Posts ``EVT_REBUILD`` only when a brand-new hub root is detected (layout
        must grow). Removing all boards from a hub does *not* trigger a rebuild —
        those slots simply become ABSENT, keeping the column layout stable.
        Posts ``EVT_HOTPLUG`` for device-level changes within the existing layout.
        The thread exits silently if the window has been closed (write_event_value
        raises once the underlying Tk root is destroyed).
        """
        def _poll():
            while True:
                time.sleep(interval)
                probes = usb_topology.enumerate_probes()
                fresh = self._detect_hub_roots(probes)
                known = set(self._hub_roots)

                if set(fresh) - known:
                    # A hub we have never seen before appeared → expand and rebuild.
                    combined = sorted(
                        known | set(fresh), key=usb_topology.path_sort_key
                    )
                    self._hub_roots = combined
                    num_hubs = max(1, len(combined))
                    new_slots = usb_topology.build_slots(
                        self._relative_map, combined, num_hubs
                    )
                    usb_topology.bind_devices(new_slots, probes)
                    self.slots = new_slots
                    try:
                        window.write_event_value(EVT_REBUILD, list(self.slots))
                    except Exception:
                        return
                else:
                    # Build against the stable known roots (never shrink).
                    num_hubs = max(1, len(self._hub_roots))
                    new_slots = usb_topology.build_slots(
                        self._relative_map, self._hub_roots, num_hubs
                    )
                    usb_topology.bind_devices(new_slots, probes)
                    if _slots_changed(self.slots, new_slots):
                        self.slots = new_slots
                        try:
                            window.write_event_value(EVT_HOTPLUG, list(self.slots))
                        except Exception:
                            return

        threading.Thread(target=_poll, daemon=True).start()


def _slots_changed(old: list[Slot], new: list[Slot]) -> bool:
    """Return True if any slot changed device presence or serial."""
    if len(old) != len(new):
        return True
    return any(o.device != n.device for o, n in zip(old, new))
