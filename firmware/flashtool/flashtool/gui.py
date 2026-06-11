"""FreeSimpleGUI front-end.

The window is rebuilt whenever hub count or config changes. The slot grid
lives inside a scrollable Column bounded to the screen width, so it never
overflows regardless of how many hubs are active.
"""

from __future__ import annotations

import FreeSimpleGUI as sg

from . import config
from .controller import (
    Controller,
    EVT_ALL_DONE,
    EVT_BUILD_DONE,
    EVT_BUILD_LINE,
    EVT_DEVICE_DONE,
    EVT_HOTPLUG,
    EVT_PROGRESS,
    EVT_REBUILD,
)
from .models import Slot, Status

_ROWS = 10
_ROW_HEIGHT_PX = 22  # approximate height per slot row in pixels

_STATUS_STYLE: dict[Status, tuple[str, str]] = {
    Status.ABSENT:    ("—",        "gray"),
    Status.IDLE:      ("ready",    "#00cc44"),
    Status.FLASHING:  ("flash…",   "#ff9900"),
    Status.VERIFYING: ("verify…",  "#00aaff"),
    Status.DONE:      ("✓ done",   "#00ff66"),
    Status.ERROR:     ("✗ error",  "#ff3333"),
}

_screen_size: tuple[int, int] | None = None


def _get_screen_size() -> tuple[int, int]:
    """Return screen dimensions, querying tkinter once and caching the result."""
    global _screen_size
    if _screen_size is None:
        tmp = sg.Window("", [[]], alpha_channel=0, finalize=True, no_titlebar=True)
        _screen_size = tmp.get_screen_size()
        tmp.close()
    return _screen_size


def _bench_config_options() -> tuple[list[str], dict[str, "Path"]]:
    configs = config.list_bench_configs()
    by_name = {config.config_display_name(p): p for p in configs}
    return list(by_name.keys()), by_name


def _slot_elements(slot: Slot) -> list:
    label, color = _STATUS_STYLE[slot.status]
    return [
        sg.Text(f"#{slot.index:>2}", size=(3, 1), font=("Courier", 9),
                justification="right", pad=(1, 1)),
        sg.Text(label, key=f"-ST-{slot.index}-", size=(8, 1),
                text_color=color, font=("Courier", 9), pad=(1, 1)),
        sg.Text("", key=f"-SER-{slot.index}-", size=(9, 1),
                text_color="gray", font=("Courier", 9), pad=(1, 1)),
    ]


def build_window(slots: list[Slot], num_hubs: int, firmware: str | None = None) -> sg.Window:
    """Build a window sized to the current hub/slot count, bounded by screen width."""
    screen_w, screen_h = _get_screen_size()
    names, _ = _bench_config_options()
    current = config.config_display_name(config._BENCH_CONFIG)
    slots_per_hub = len(slots) // num_hubs if num_hubs else len(slots)
    cols_per_hub = max(1, slots_per_hub // _ROWS)

    fw_label = firmware.split("/")[-1] if firmware else "—"
    fw_color = "white" if firmware else "gray"

    toolbar = [
        sg.Combo(names, default_value=current, key="-CONFIG-",
                 enable_events=True, readonly=True, size=(20, 1)),
        sg.Text(f"Hubs: {num_hubs}", key="-HUB-COUNT-", pad=((10, 4), 0)),
        sg.Button("Build", size=(7, 1), pad=((10, 4), 0)),
        sg.Button("Flash All", size=(9, 1), pad=((4, 4), 0), disabled=firmware is None),
        sg.Text("firmware:", pad=((12, 4), 0)),
        sg.Text(fw_label, key="-FW-", size=(30, 1), text_color=fw_color,
                font=("Courier", 9)),
        sg.Button("Browse…", size=(8, 1), pad=((4, 4), 0)),
    ]

    rows = []
    for row in range(_ROWS):
        line: list = []
        for hub in range(num_hubs):
            if hub > 0:
                line += [sg.Text(" ‖ ", text_color="gray", pad=(2, 1))]
            for col in range(cols_per_hub):
                idx = hub * slots_per_hub + col * _ROWS + row
                if idx >= len(slots):
                    break
                if col > 0:
                    line += [sg.Text("|", text_color="gray", pad=(2, 1))]
                line += _slot_elements(slots[idx])
        rows.append(line)

    grid_h = _ROWS * _ROW_HEIGHT_PX + 8
    grid = sg.Column(rows, size=(screen_w - 40, grid_h),
                     scrollable=True, vertical_scroll_only=False)

    return sg.Window("flashtool", [toolbar, [grid]],
                     finalize=True, resizable=True)


def update_slot(window, slot: Slot) -> None:
    label, color = _STATUS_STYLE[slot.status]
    window[f"-ST-{slot.index}-"].update(label, text_color=color)
    serial = slot.device.serial if slot.device and slot.device.serial else ""
    window[f"-SER-{slot.index}-"].update(serial)


def _set_firmware(window, ctrl: Controller, path: str) -> None:
    """Update controller + toolbar label with a new firmware path."""
    ctrl.firmware = path
    label = path.split("/")[-1] if path else "—"
    window["-FW-"].update(label, text_color="white")
    window["Flash All"].update(disabled=False)


def _event_loop(window: sg.Window, ctrl: Controller) -> str:
    """Run the event loop. Returns ``"rebuild"`` or ``"exit"``."""
    _build_log: list[str] = []  # accumulates west build output for error reporting

    while True:
        event, values = window.read(timeout=200)

        if event in (sg.WIN_CLOSED, "Exit"):
            return "exit"

        elif event == EVT_REBUILD:
            return "rebuild"

        elif event == "-CONFIG-":
            _, by_name = _bench_config_options()
            path = by_name.get(values["-CONFIG-"])
            if path:
                ctrl.load_config(path)
                return "rebuild"

        elif event == "Build":
            _build_log.clear()
            window["Build"].update(disabled=True)
            window["-FW-"].update("building…", text_color="gray")
            ctrl.build(window)

        elif event == EVT_BUILD_LINE:
            _build_log.append(values[event])

        elif event == EVT_BUILD_DONE:
            ok, hex_path = values[event]
            window["Build"].update(disabled=False)
            if ok and hex_path:
                _set_firmware(window, ctrl, hex_path)
            else:
                window["-FW-"].update("build failed", text_color="#ff3333")
                tail = "".join(_build_log[-30:])
                sg.popup_scrolled(tail, title="Build output", size=(80, 24))

        elif event == "Browse…":
            path = sg.popup_get_file(
                "Select firmware hex",
                file_types=(("Hex files", "*.hex"), ("All files", "*.*")),
                no_window=True,
            )
            if path:
                _set_firmware(window, ctrl, path)

        elif event == "Flash All":
            window["Flash All"].update(disabled=True)
            ctrl.flash_all(window)

        elif event == EVT_HOTPLUG:
            for slot in values[event]:
                update_slot(window, slot)

        elif event == EVT_PROGRESS:
            idx, pct, msg = values[event]
            update_slot(window, ctrl.slots[idx])

        elif event == EVT_DEVICE_DONE:
            idx, ok, msg = values[event]
            update_slot(window, ctrl.slots[idx])

        elif event == EVT_ALL_DONE:
            summary = values[event]
            if "error" in summary and isinstance(summary["error"], str):
                sg.popup_error(summary["error"], title="Flash error")
            else:
                done, err = summary.get("done", 0), summary.get("error", 0)
                sg.popup(f"Done: {done}   Errors: {err}", title="Flash complete")
            window["Flash All"].update(disabled=False)


def run() -> None:
    ctrl = Controller()

    # Pre-populate firmware from the default build location if it already exists.
    try:
        ctrl.firmware = ctrl.resolve_firmware()
    except FileNotFoundError:
        pass

    slots = ctrl.discover()
    window = build_window(slots, ctrl.num_hubs, ctrl.firmware)
    ctrl.start_hotplug_monitor(window, interval=0.5)

    while True:
        action = _event_loop(window, ctrl)
        if action == "exit":
            window.close()
            break
        # Rebuild: open the new window before closing the old one so there is
        # no blank-screen moment that looks like a crash.
        old_window = window
        slots = ctrl.discover()
        window = build_window(slots, ctrl.num_hubs, ctrl.firmware)
        ctrl.start_hotplug_monitor(window, interval=0.5)
        old_window.close()
