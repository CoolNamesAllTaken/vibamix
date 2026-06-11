"""FreeSimpleGUI front-end.

The window is rebuilt whenever hub count or config changes. The slot grid
lives inside a scrollable Column bounded to the screen width, so it never
overflows regardless of how many hubs are active.
"""

from __future__ import annotations

import FreeSimpleGUI as sg

from . import calibrate, config
from .controller import (
    Controller,
    EVT_ALL_DONE,
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


def build_window(slots: list[Slot], num_hubs: int) -> sg.Window:
    """Build a window sized to the current hub/slot count, bounded by screen width."""
    screen_w, screen_h = _get_screen_size()
    names, _ = _bench_config_options()
    current = config.config_display_name(config._BENCH_CONFIG)
    slots_per_hub = len(slots) // num_hubs if num_hubs else len(slots)
    cols_per_hub = max(1, slots_per_hub // _ROWS)

    toolbar = [
        sg.Combo(names, default_value=current, key="-CONFIG-",
                 enable_events=True, readonly=True, size=(20, 1)),
        sg.Text(f"Hubs: {num_hubs}", key="-HUB-COUNT-", pad=((10, 4), 0)),
        sg.Button("Discover", size=(9, 1), pad=((10, 4), 0)),
        sg.Button("Flash All", size=(9, 1), disabled=True),
        sg.Text("firmware:", pad=((12, 4), 0)),
        sg.Text("—", key="-FW-", size=(24, 1), text_color="gray",
                font=("Courier", 9)),
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


def _event_loop(window: sg.Window, ctrl: Controller) -> str:
    """Run the event loop. Returns ``"rebuild"`` or ``"exit"``."""
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

        elif event == "Discover":
            def _prompt(msg):
                sg.popup_ok(msg, title="Discover hub")

            slot_map = calibrate.run_slot_calibration(prompt_fn=_prompt)
            if slot_map:
                relative_map = calibrate.abs_to_relative(slot_map)
                name = sg.popup_get_text("Save config as (e.g. my_hub):",
                                         title="Save calibration")
                if name and name.strip():
                    dest = config._CONFIG_DIR / f"bench_{name.strip()}.json"
                    calibrate.write_bench_config(relative_map, dest)
                ctrl._relative_map = relative_map
            return "rebuild"

        elif event == "Flash All":
            sg.popup("Flash All not yet implemented.")

        elif event == EVT_HOTPLUG:
            for slot in values[event]:
                update_slot(window, slot)

        elif event == EVT_PROGRESS:
            idx, pct, msg = values[event]
            pass

        elif event == EVT_DEVICE_DONE:
            idx, ok, msg = values[event]
            slot = ctrl.slots[idx]
            update_slot(window, slot)

        elif event == EVT_ALL_DONE:
            summary = values[event]
            sg.popup(f"Done.  {summary}")
            window["Flash All"].update(disabled=False)


def run() -> None:
    ctrl = Controller()

    while True:
        slots = ctrl.discover()
        window = build_window(slots, ctrl.num_hubs)
        ctrl.start_hotplug_monitor(window, interval=0.5)

        action = _event_loop(window, ctrl)
        window.close()

        if action == "exit":
            break
