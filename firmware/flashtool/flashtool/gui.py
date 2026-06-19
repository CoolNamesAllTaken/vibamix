"""PyQt6 front-end.

The slot grid is laid out as one column group per hub, inside a scrollable
area so it never overflows regardless of how many hubs are active. Worker
threads in the controller report progress by calling ``write_event_value`` on a
``QtBridge`` (a drop-in for FreeSimpleGUI's old API) which re-emits everything as
a thread-safe Qt signal delivered on the GUI thread.
"""

from __future__ import annotations

import sys

from PyQt6.QtCore import QObject, Qt, pyqtSignal
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QDialog,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

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


def _mono_font() -> QFont:
    """A 9pt monospace font with a portable fallback when Courier is absent."""
    f = QFont("Courier", 9)
    f.setStyleHint(QFont.StyleHint.Monospace)
    return f

_STATUS_STYLE: dict[Status, tuple[str, str]] = {
    Status.ABSENT:    ("—",       "gray"),
    Status.IDLE:      ("ready",   "#00cc44"),
    Status.FLASHING:  ("flash…",  "#ff9900"),
    Status.VERIFYING: ("verify…", "#00aaff"),
    Status.DONE:      ("✓ done",  "#00ff66"),
    Status.ERROR:     ("✗ error", "#ff3333"),
}


class QtBridge(QObject):
    """Re-exposes the controller's posting API as a thread-safe Qt signal.

    The controller (and its worker threads) call ``write_event_value`` exactly as
    they did with FreeSimpleGUI; here it becomes a ``pyqtSignal`` emit. Qt auto-
    queues cross-thread emits onto the GUI thread, so the slot runs safely there.
    """

    event = pyqtSignal(str, object)  # (event_key, value)

    def write_event_value(self, key: str, value) -> None:
        self.event.emit(key, value)


def _bench_config_options() -> tuple[list[str], dict[str, "Path"]]:
    configs = config.list_bench_configs()
    by_name = {config.config_display_name(p): p for p in configs}
    return list(by_name.keys()), by_name


class MainWindow(QWidget):
    """Top-level window: toolbar + scrollable per-hub slot grid."""

    def __init__(self, ctrl: Controller) -> None:
        super().__init__()
        self.ctrl = ctrl
        self.bridge = QtBridge()
        self.bridge.event.connect(self._on_event)
        self._build_log: list[str] = []  # accumulates west build output
        self._status_labels: dict[int, QLabel] = {}
        self._serial_labels: dict[int, QLabel] = {}

        self.setWindowTitle("flashtool")
        self.resize(1100, 600)

        root = QVBoxLayout(self)
        root.addLayout(self._build_toolbar())

        self._scroll = QScrollArea()
        self._scroll.setWidgetResizable(True)
        root.addWidget(self._scroll, stretch=1)

        self._populate_grid(ctrl.discover())
        ctrl.start_hotplug_monitor(self.bridge, interval=0.5)

    # -- toolbar --------------------------------------------------------------

    def _build_toolbar(self) -> QHBoxLayout:
        bar = QHBoxLayout()
        names, _ = _bench_config_options()
        current = config.config_display_name(config._BENCH_CONFIG)

        self._config_combo = QComboBox()
        self._config_combo.addItems(names)
        if current in names:
            self._config_combo.setCurrentText(current)
        self._config_combo.currentTextChanged.connect(self._on_config_changed)
        bar.addWidget(self._config_combo)

        self._hub_label = QLabel()
        bar.addWidget(self._hub_label)

        self._build_btn = QPushButton("Build")
        self._build_btn.clicked.connect(self._on_build)
        bar.addWidget(self._build_btn)

        self._flash_btn = QPushButton("Flash All")
        self._flash_btn.clicked.connect(self._on_flash_all)
        bar.addWidget(self._flash_btn)

        bar.addWidget(QLabel("firmware:"))
        self._fw_label = QLabel()
        self._fw_label.setFont(_mono_font())
        bar.addWidget(self._fw_label)

        self._browse_btn = QPushButton("Browse…")
        self._browse_btn.clicked.connect(self._on_browse)
        bar.addWidget(self._browse_btn)

        bar.addStretch(1)
        self._refresh_firmware_label()
        return bar

    def _refresh_firmware_label(self) -> None:
        # Flash All always writes bootloader + app, so the label reflects both.
        fw = self.ctrl.firmware
        label = f"bl + {fw.split('/')[-1]}" if fw else "—"
        color = "white" if fw else "gray"
        self._fw_label.setText(label)
        self._fw_label.setStyleSheet(f"color: {color};")
        self._flash_btn.setEnabled(fw is not None)

    def _set_firmware(self, path: str) -> None:
        self.ctrl.firmware = path
        self._refresh_firmware_label()

    # -- slot grid ------------------------------------------------------------

    def _populate_grid(self, slots: list[Slot]) -> None:
        """(Re)build the slot grid widget for the current hub/slot layout."""
        num_hubs = self.ctrl.num_hubs
        self._hub_label.setText(f"Hubs: {num_hubs}")
        self._status_labels.clear()
        self._serial_labels.clear()

        container = QWidget()
        grid = QGridLayout(container)
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(2)
        grid.setContentsMargins(8, 8, 8, 8)

        slots_per_hub = len(slots) // num_hubs if num_hubs else len(slots)
        cols_per_hub = max(1, slots_per_hub // _ROWS)

        for row in range(_ROWS):
            grid_col = 0
            for hub in range(num_hubs):
                if hub > 0:
                    sep = QFrame()
                    sep.setFrameShape(QFrame.Shape.VLine)
                    grid.addWidget(sep, row, grid_col)
                    grid_col += 1
                for col in range(cols_per_hub):
                    idx = hub * slots_per_hub + col * _ROWS + row
                    if idx >= len(slots):
                        continue
                    for w in self._slot_widgets(slots[idx]):
                        grid.addWidget(w, row, grid_col)
                        grid_col += 1

        grid.setRowStretch(_ROWS, 1)
        grid.setColumnStretch(grid.columnCount(), 1)
        self._scroll.setWidget(container)

    def _slot_widgets(self, slot: Slot) -> list[QWidget]:
        idx = QLabel(f"#{slot.index:>2}")
        idx.setFont(_mono_font())
        idx.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        status = QLabel()
        status.setFont(_mono_font())
        status.setMinimumWidth(56)
        self._status_labels[slot.index] = status

        serial = QLabel()
        serial.setFont(_mono_font())
        serial.setStyleSheet("color: gray;")
        serial.setMinimumWidth(72)
        self._serial_labels[slot.index] = serial

        self._render_slot(slot)
        return [idx, status, serial]

    def _render_slot(self, slot: Slot) -> None:
        label, color = _STATUS_STYLE[slot.status]
        status = self._status_labels.get(slot.index)
        if status is not None:
            status.setText(label)
            status.setStyleSheet(f"color: {color};")
        serial = self._serial_labels.get(slot.index)
        if serial is not None:
            serial.setText(slot.device.serial if slot.device and slot.device.serial else "")

    # -- button handlers ------------------------------------------------------

    def _on_config_changed(self, name: str) -> None:
        _, by_name = _bench_config_options()
        path = by_name.get(name)
        if path:
            self.ctrl.load_config(path)
            self._populate_grid(self.ctrl.slots)

    def _on_build(self) -> None:
        self._build_log.clear()
        self._build_btn.setEnabled(False)
        self._fw_label.setText("building bl+app…")
        self._fw_label.setStyleSheet("color: gray;")
        self.ctrl.build(self.bridge)

    def _on_browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Select firmware hex", "",
            "Hex files (*.hex);;All files (*.*)",
        )
        if path:
            self._set_firmware(path)

    def _on_flash_all(self) -> None:
        self._flash_btn.setEnabled(False)
        self.ctrl.flash_all(self.bridge)

    # -- event dispatch (mirrors the old FreeSimpleGUI event loop) ------------

    def _on_event(self, key: str, value) -> None:
        if key == EVT_REBUILD:
            self._populate_grid(self.ctrl.slots)

        elif key == EVT_HOTPLUG:
            for slot in value:
                self._render_slot(slot)

        elif key == EVT_PROGRESS:
            idx, _pct, _msg = value
            self._render_slot(self.ctrl.slots[idx])

        elif key == EVT_DEVICE_DONE:
            idx, _ok, _msg = value
            self._render_slot(self.ctrl.slots[idx])

        elif key == EVT_BUILD_LINE:
            self._build_log.append(value)

        elif key == EVT_BUILD_DONE:
            ok, hex_path = value
            self._build_btn.setEnabled(True)
            if ok and hex_path:
                self._set_firmware(hex_path)
            else:
                self._fw_label.setText("build failed")
                self._fw_label.setStyleSheet("color: #ff3333;")
                _show_scrolled("".join(self._build_log[-30:]), "Build output", self)

        elif key == EVT_ALL_DONE:
            summary = value
            self._flash_btn.setEnabled(True)
            if "error" in summary and isinstance(summary["error"], str):
                QMessageBox.critical(self, "Flash error", summary["error"])
            else:
                done, err = summary.get("done", 0), summary.get("error", 0)
                QMessageBox.information(
                    self, "Flash complete", f"Done: {done}   Errors: {err}")


def _show_scrolled(text: str, title: str, parent: QWidget) -> None:
    dlg = QDialog(parent)
    dlg.setWindowTitle(title)
    dlg.resize(700, 400)
    layout = QVBoxLayout(dlg)
    view = QPlainTextEdit()
    view.setReadOnly(True)
    view.setFont(_mono_font())
    view.setPlainText(text)
    layout.addWidget(view)
    dlg.exec()


def run() -> None:
    ctrl = Controller()

    # Pre-populate firmware from the default build location if it already exists.
    try:
        ctrl.firmware = ctrl.resolve_firmware()
    except FileNotFoundError:
        pass

    app = QApplication(sys.argv)
    window = MainWindow(ctrl)
    window.show()
    sys.exit(app.exec())
