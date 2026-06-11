"""Orchestration: discovery + concurrent flashing, decoupled from the GUI.

The controller never touches widgets. It reports everything by posting events
back to the GUI thread via ``window.write_event_value`` (thread-safe in
FreeSimpleGUI), keeping worker threads and the UI cleanly separated.
"""

from __future__ import annotations

import time
import threading
from concurrent.futures import ThreadPoolExecutor

from . import builder, config, flasher, usb_topology
from .models import Slot, Status

# Event keys posted to the GUI event loop.
EVT_PROGRESS    = "-DEV-PROGRESS-"  # value = (slot_index, pct, message)
EVT_DEVICE_DONE = "-DEV-DONE-"      # value = (slot_index, ok, message)
EVT_ALL_DONE    = "-ALL-DONE-"      # value = {"done": int, "error": int, "absent": int}
EVT_HOTPLUG     = "-HOTPLUG-"       # value = list[Slot] — devices changed within same hub topology
EVT_REBUILD     = "-REBUILD-"       # value = list[Slot] — hub count/roots changed, window must rebuild
EVT_BUILD_LINE  = "-BUILD-LINE-"    # value = str — one line of west build output
EVT_BUILD_DONE  = "-BUILD-DONE-"    # value = (ok: bool, hex_path: str | None)


class Controller:
    """Owns the slot list and drives discovery + flashing."""

    def __init__(self) -> None:
        self.slots: list[Slot] = []
        self._relative_map: dict[int, str] | None = config.RELATIVE_MAP
        self._hub_roots: list[str] = []
        self.firmware: str | None = None  # path to the hex to flash; set by build or browse

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
        """Return the firmware path to flash.

        Uses ``self.firmware`` if set (by a build or Browse), otherwise falls
        back to the newest hex found in ``config.ARTIFACT_DIR``.
        """
        if self.firmware:
            return self.firmware
        matches = sorted(
            config.ARTIFACT_DIR.glob(config.FIRMWARE_GLOB),
            key=lambda p: p.stat().st_mtime,
        )
        if not matches:
            raise FileNotFoundError(
                f"No firmware found in {config.ARTIFACT_DIR} "
                f"matching {config.FIRMWARE_GLOB!r} — build first or click Browse"
            )
        return str(matches[-1])

    def flash_all(self, window) -> None:
        """Kick off flashing every present slot without blocking the GUI."""
        try:
            firmware = self.resolve_firmware()
        except FileNotFoundError as exc:
            window.write_event_value(EVT_ALL_DONE, {"error": str(exc)})
            return
        present = [s for s in self.slots if s.device is not None]
        if not present:
            return
        for s in present:
            s.status = Status.FLASHING
        window.write_event_value(EVT_HOTPLUG, list(self.slots))
        threading.Thread(
            target=self._run_pool,
            args=(window, firmware, present),
            daemon=True,
        ).start()

    def _run_pool(self, window, firmware: str, present: list[Slot]) -> None:
        """Dispatcher thread: fan out one worker per slot, bounded pool."""
        with ThreadPoolExecutor(max_workers=config.MAX_CONCURRENT) as ex:
            futures = [ex.submit(self._flash_one, window, s, firmware) for s in present]
            for f in futures:
                try:
                    f.result()
                except Exception:
                    pass
        try:
            window.write_event_value(EVT_ALL_DONE, self._summary())
        except Exception:
            pass

    def _flash_one(self, window, slot: Slot, firmware: str) -> None:
        """Worker: flash one slot, streaming progress back as GUI events."""
        def cb(pct: float, msg: str) -> None:
            slot.progress = pct
            slot.message = msg
            if "verify" in msg:
                slot.status = Status.VERIFYING
            try:
                window.write_event_value(EVT_PROGRESS, (slot.index, pct, msg))
            except Exception:
                pass

        ok, msg = flasher.flash_device(slot.device, firmware, cb)
        slot.status = Status.DONE if ok else Status.ERROR
        slot.message = msg
        try:
            window.write_event_value(EVT_DEVICE_DONE, (slot.index, ok, msg))
        except Exception:
            pass

    def _summary(self) -> dict:
        done   = sum(1 for s in self.slots if s.status is Status.DONE)
        error  = sum(1 for s in self.slots if s.status is Status.ERROR)
        absent = sum(1 for s in self.slots if s.status is Status.ABSENT)
        return {"done": done, "error": error, "absent": absent}

    def build(self, window) -> None:
        """Run ``west build`` in a background thread, streaming output as events."""
        def _run():
            def emit(line: str) -> None:
                try:
                    window.write_event_value(EVT_BUILD_LINE, line)
                except Exception:
                    pass

            ok = builder.build(on_output=emit)
            hex_path = str(builder.firmware_hex()) if ok else None
            if ok and hex_path:
                self.firmware = hex_path
            try:
                window.write_event_value(EVT_BUILD_DONE, (ok, hex_path))
            except Exception:
                pass

        threading.Thread(target=_run, daemon=True).start()

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
