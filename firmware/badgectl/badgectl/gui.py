"""PyQt6 main window: scalable device table + Direct-GATT / Mesh / Batch panels."""

from __future__ import annotations

import asyncio
import datetime
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PyQt6.QtCore import QSortFilterProxyModel, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import (
    QColor,
    QGuiApplication,
    QIcon,
    QImage,
    QKeySequence,
    QPixmap,
    QShortcut,
)
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSlider,
    QSpinBox,
    QSplitter,
    QTableView,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from bleak.exc import BleakDeviceNotFoundError

from . import flash, flashconfig, imageconv, keys, probes, serialreg
from .ble import BadgeLink, Found, Scanner
from .devices import COL_CHECK, COL_NAME, DeviceModel
from .mesh import MeshCrypto, MeshSession
from .seqstore import SeqStore

STYLE = """
QWidget { font-size: 13px; }
QGroupBox {
    border: 1px solid #3a3f4b; border-radius: 8px; margin-top: 10px; padding: 8px;
    font-weight: 600;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #9aa4b2; }
QPushButton {
    background: #2d6cdf; color: white; border: none; border-radius: 6px;
    padding: 6px 12px;
}
QPushButton:hover { background: #3b78e7; }
QPushButton:disabled { background: #444b59; color: #8b93a1; }
QPushButton#ghost { background: #353b47; }
QPushButton#ghost:hover { background: #404757; }
QLineEdit, QPlainTextEdit, QComboBox, QSpinBox {
    background: #20242c; border: 1px solid #3a3f4b; border-radius: 6px; padding: 4px;
}
QComboBox { min-width: 90px; }
QComboBox QAbstractItemView {
    background: #20242c; color: #e6e9ef;
    selection-background-color: #2d6cdf; padding: 2px;
}
QPlainTextEdit#log { font-family: Menlo, Consolas, monospace; font-size: 12px; }
QLabel#status { font-weight: 700; }
QTableView { background: #181b21; gridline-color: #2a2f3a; selection-background-color: #2d6cdf; }
QHeaderView::section { background: #20242c; border: none; padding: 4px; color: #9aa4b2; }
"""

KA_FRESH_S = 5.0  # link considered alive if any badge activity seen within this many seconds
STALE_PRUNE_S = 60.0  # drop scanned badges unheard for this long


def pil_to_pixmap(pil_img) -> QPixmap:
    img = pil_img.convert("L")
    w, h = img.size
    qimg = QImage(img.tobytes(), w, h, w, QImage.Format.Format_Grayscale8)
    return QPixmap.fromImage(qimg.copy())


class MainWindow(QMainWindow):
    # Emitted from the flash worker threads; delivered (queued) on the Qt thread.
    flash_progress = pyqtSignal(str, float, str)   # probe label, percent, phase
    flash_result = pyqtSignal(str, bool, str)      # probe label, ok, message
    flash_finished = pyqtSignal(int, int)          # ok count, total
    reset_finished = pyqtSignal(int, int)          # ok count, total (probe-rs reset)

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("badgectl — vibamix")
        self.setWindowIcon(QIcon(str(Path(__file__).parent / "assets" / "icon.png")))
        # Fit the host laptop's *available* display (excludes menu bar / dock /
        # taskbar) so the window never opens taller than a short screen, while
        # keeping the roomy default on large monitors.
        avail = QGuiApplication.primaryScreen().availableGeometry()
        self.setMinimumSize(min(800, avail.width()), min(480, avail.height()))
        self.resize(min(1280, avail.width()), min(800, avail.height()))
        self.setStyleSheet(STYLE)

        self.link = BadgeLink()
        self.crypto = MeshCrypto(keys.NET_KEY, keys.APP_KEY, keys.IV_INDEX)
        self._seqstore: SeqStore | None = None
        self._led = (255, 80, 0)
        self._slot_img: bytes | None = None
        self._idimg: bytes | None = None

        # device table + scanner
        self.model = DeviceModel()
        self.proxy = QSortFilterProxyModel()
        self.proxy.setSourceModel(self.model)
        self.proxy.setFilterKeyColumn(-1)  # all columns
        self.proxy.setFilterCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.scanner = Scanner(self._on_scan_update)

        # connection / keepalive state
        self._active: str | None = None     # address of the interactively-connected badge
        self._hb_task: asyncio.Task | None = None
        self._last_rx = 0.0     # last badge->app keepalive notify (for the age label)
        self._last_link = 0.0   # last successful GATT round-trip (any direction) — link health
        self._batch_running = False
        self._batch_cancel = False

        # flash (SWD bulk-flash) state
        self._flash_running = False
        self._fl_probes: list[probes.Probe] = []
        self._fl_rows: dict[str, int] = {}      # probe label -> table row
        self._fl_status: dict[str, str] = {}    # probe label -> last status text

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.addLayout(self._build_topbar())

        main_split = QSplitter(Qt.Orientation.Horizontal)
        main_split.addWidget(self._build_device_pane())
        main_split.addWidget(self._build_action_pane())
        main_split.setStretchFactor(0, 3)
        main_split.setStretchFactor(1, 5)

        outer = QSplitter(Qt.Orientation.Vertical)
        outer.addWidget(main_split)
        self.log = QPlainTextEdit()
        self.log.setObjectName("log")
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        outer.addWidget(self.log)
        outer.setStretchFactor(0, 5)
        outer.setStretchFactor(1, 1)
        root.addWidget(outer)

        self._hb_timer = QTimer(self)   # mesh auto-heartbeat (unrelated to GATT keepalive)
        self._hb_timer.setInterval(1_000)
        self._hb_timer.timeout.connect(lambda: self._go(self._send_heartbeat()))

        self._ui_timer = QTimer(self)   # refresh "seen" ages + keepalive freshness
        self._ui_timer.setInterval(500)
        self._ui_timer.timeout.connect(self._tick_ui)
        self._ui_timer.start()

        self._set_connected(False)
        self._log("Ready. Start scan, pick a badge (wake it with its button), then Connect.")

    # ---------- async plumbing ----------
    def _go(self, coro) -> None:
        asyncio.ensure_future(self._guard(coro))

    async def _guard(self, coro) -> None:
        try:
            await coro
        except Exception as e:  # noqa: BLE001
            self._log(f"ERROR: {e}")
            self._log(traceback.format_exc().strip())

    def _log(self, msg: str) -> None:
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        self.log.appendPlainText(f"[{ts}] {msg}")

    # ---------- top bar ----------
    def _build_topbar(self) -> QHBoxLayout:
        bar = QHBoxLayout()
        self.scan_btn = QPushButton("Start scan")
        self.scan_btn.setObjectName("ghost")
        self.scan_btn.clicked.connect(lambda: self._go(self._toggle_scan()))
        self.filter_edit = QLineEdit()
        self.filter_edit.setPlaceholderText("Filter (name / address)…")
        self.filter_edit.textChanged.connect(self.proxy.setFilterFixedString)
        self.count_lbl = QLabel("0 devices")
        self.count_lbl.setStyleSheet("color:#9aa4b2;")
        bar.addWidget(self.scan_btn)
        bar.addWidget(self.filter_edit, 1)
        bar.addWidget(self.count_lbl)
        return bar

    # ---------- device pane (left) ----------
    def _build_device_pane(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)
        self.table = QTableView()
        self.table.setModel(self.proxy)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setSortingEnabled(True)
        self.table.verticalHeader().setVisible(False)
        hdr = self.table.horizontalHeader()
        hdr.setSectionResizeMode(COL_NAME, QHeaderView.ResizeMode.Stretch)
        self.table.setColumnWidth(COL_CHECK, 28)
        self.table.selectionModel().selectionChanged.connect(self._on_row_selected)
        v.addWidget(self.table)
        # Delete/Backspace on the table forgets the selected badge.
        for _seq in (QKeySequence.StandardKey.Delete, QKeySequence(Qt.Key.Key_Backspace)):
            sc = QShortcut(_seq, self.table)
            sc.setContext(Qt.ShortcutContext.WidgetShortcut)
            sc.activated.connect(self._forget_selected)

        row = QHBoxLayout()
        self.check_all_btn = QPushButton("Check all (filtered)")
        self.check_all_btn.setObjectName("ghost")
        self.check_all_btn.clicked.connect(lambda: self._check_visible(True))
        self.uncheck_all_btn = QPushButton("Uncheck all")
        self.uncheck_all_btn.setObjectName("ghost")
        self.uncheck_all_btn.clicked.connect(lambda: self._check_visible(False))
        self.forget_btn = QPushButton("Forget")
        self.forget_btn.setObjectName("ghost")
        self.forget_btn.setEnabled(False)
        self.forget_btn.clicked.connect(self._forget_selected)
        row.addWidget(self.check_all_btn)
        row.addWidget(self.uncheck_all_btn)
        row.addWidget(self.forget_btn)
        row.addStretch()
        v.addLayout(row)
        return w

    def _forget_selected(self) -> None:
        """Drop the selected badge from the scan list. Note: while a scan is
        running it reappears on the next advertisement — most useful when stopped."""
        d = self._selected_device()
        if d is None:
            return
        self.model.remove(d.address)
        self.count_lbl.setText(f"{self.model.rowCount()} devices")

    def _check_visible(self, checked: bool) -> None:
        for r in range(self.proxy.rowCount()):
            src = self.proxy.mapToSource(self.proxy.index(r, COL_CHECK))
            self.model.setData(src, Qt.CheckState.Checked.value if checked
                               else Qt.CheckState.Unchecked.value,
                               Qt.ItemDataRole.CheckStateRole)

    def _selected_device(self):
        idxs = self.table.selectionModel().selectedRows()
        if not idxs:
            return None
        return self.model.device_at(self.proxy.mapToSource(idxs[0]).row())

    def _on_row_selected(self, *_):
        d = self._selected_device()
        self.sel_lbl.setText(f"{d.name}  ({d.address[:8]}…)" if d else "—")
        self.connect_btn.setEnabled(d is not None and not self.link.connected and not self._batch_running)
        self.forget_btn.setEnabled(d is not None)

    # ---------- action pane (right) ----------
    def _scrollable(self, w: QWidget) -> QScrollArea:
        """Wrap a tab's content so it scrolls internally instead of clipping on
        short screens. With setWidgetResizable the content keeps its top-aligned
        full-height look and only grows a scrollbar when the pane is too short."""
        sa = QScrollArea()
        sa.setWidgetResizable(True)
        sa.setFrameShape(QFrame.Shape.NoFrame)
        sa.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        sa.setWidget(w)
        return sa

    def _build_action_pane(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)
        v.addWidget(self._build_conn_header())
        tabs = QTabWidget()
        tabs.addTab(self._build_gatt_tab(), "Direct (GATT)")
        tabs.addTab(self._scrollable(self._build_mesh_tab()), "Mesh")
        tabs.addTab(self._scrollable(self._build_batch_tab()), "Batch")
        tabs.addTab(self._scrollable(self._build_flash_tab()), "Flash")
        v.addWidget(tabs, 1)
        return w

    def _dot(self) -> QLabel:
        lbl = QLabel("●")
        lbl.setStyleSheet("color:#555b66;")
        return lbl

    def _build_conn_header(self) -> QGroupBox:
        g = QGroupBox("Connection")
        gl = QGridLayout(g)
        self.sel_lbl = QLabel("—")
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(lambda: self._go(self._connect()))
        self.disconnect_btn = QPushButton("Disconnect")
        self.disconnect_btn.setObjectName("ghost")
        self.disconnect_btn.clicked.connect(lambda: self._go(self._disconnect()))
        self.status = QLabel("● disconnected")
        self.status.setObjectName("status")
        self.status.setStyleSheet("color:#e06c5b;")
        self.mtu_lbl = QLabel("MTU —")
        self.mtu_lbl.setStyleSheet("color:#9aa4b2;")
        self.ka_dot = self._dot()
        self.ka_age = QLabel("")
        self.ka_age.setStyleSheet("color:#9aa4b2;")

        gl.addWidget(QLabel("Selected:"), 0, 0)
        gl.addWidget(self.sel_lbl, 0, 1, 1, 3)
        gl.addWidget(self.connect_btn, 0, 4)
        gl.addWidget(self.disconnect_btn, 0, 5)
        gl.addWidget(self.status, 1, 0, 1, 2)
        gl.addWidget(self.mtu_lbl, 1, 2)
        gl.addWidget(QLabel("Link"), 1, 3)
        gl.addWidget(self.ka_dot, 1, 4)     # badge → app keepalive (link health)
        gl.addWidget(self.ka_age, 1, 5)
        return g

    def _set_connected(self, on: bool) -> None:
        self.status.setText("● connected" if on else "● disconnected")
        self.status.setStyleSheet("color:#5bd07a;" if on else "color:#e06c5b;")
        self.disconnect_btn.setEnabled(on)
        self.connect_btn.setEnabled(not on and self._selected_device() is not None and not self._batch_running)
        for wdg in self._action_widgets:
            wdg.setEnabled(on)
        if on:
            self.mtu_lbl.setText(f"MTU {self.link.mtu}")
        else:
            self.mtu_lbl.setText("MTU —")
            self._hb_timer.stop()
            self.ka_dot.setStyleSheet("color:#555b66;")
            self.ka_age.setText("")

    # ---------- scanning ----------
    async def _toggle_scan(self) -> None:
        if self.scanner.running:
            await self.scanner.stop()
            self.scan_btn.setText("Start scan")
            self._log("Scan stopped.")
        else:
            await self.scanner.start()
            self.scan_btn.setText("Stop scan")
            self._log("Scanning continuously — wake badges with their button.")

    def _on_scan_update(self, address, name, rssi, is_proxy, device) -> None:
        self.model.upsert(address, name, rssi, is_proxy, device)
        self.count_lbl.setText(f"{self.model.rowCount()} devices")

    def _tick_ui(self) -> None:
        self.model.refresh_ages()
        if self.scanner.running:
            protect = {self._active} if self._active else set()
            if self.model.prune_stale(STALE_PRUNE_S, protect):
                self.count_lbl.setText(f"{self.model.rowCount()} devices")
        if self._active and self.link.connected:
            now = time.monotonic()
            rx_age = now - self._last_rx                          # true notify age (label)
            link_age = now - max(self._last_rx, self._last_link)  # any link activity (dot)
            self.ka_dot.setStyleSheet("color:#5bd07a;" if link_age < KA_FRESH_S else "color:#e06c5b;")
            self.ka_age.setText(f"badge {rx_age:.0f}s")
            self.model.set_ka(self._active, link_age)

    # ---------- connect / keepalive ----------
    async def _connect(self) -> None:
        d = self._selected_device()
        if d is None:
            self._log("Select a device row first.")
            return
        self._log(f"Connecting to {d.name} ({d.address}) …")
        found = Found(name=d.name, address=d.address, is_proxy=d.is_proxy, device=d.device)
        try:
            await self.link.connect(found, on_disconnect=self._on_bleak_disconnect)
        except BleakDeviceNotFoundError:
            self._log("Badge not found — press its button to wake it, then scan again.")
            self.model.set_status(d.address, "idle")
            return
        self._active = d.address
        self.model.set_status(d.address, "connected")
        self._set_connected(True)
        self._log(f"Connected. ATT MTU = {self.link.mtu}.")
        await self._load_frames()
        await self._start_keepalive()

    async def _start_keepalive(self) -> None:
        self._last_rx = self._last_link = time.monotonic()
        try:
            await self.link.subscribe_keepalive(self._on_ka_rx)
        except Exception as e:  # noqa: BLE001
            self._log(f"keepalive subscribe failed: {e}")
        self._hb_task = asyncio.ensure_future(self._heartbeat_loop())

    async def _heartbeat_loop(self) -> None:
        while self.link.connected:
            try:
                await self.link.send_keepalive()
            except Exception:  # noqa: BLE001
                break
            self._last_link = time.monotonic()
            await asyncio.sleep(1.0)

    def _on_ka_rx(self, _code: int) -> None:
        now = time.monotonic()
        self._last_rx = now
        self._last_link = now

    def _stop_keepalive(self) -> None:
        if self._hb_task is not None:
            self._hb_task.cancel()
            self._hb_task = None

    async def _disconnect(self) -> None:
        self._stop_keepalive()
        await self.link.disconnect()
        if self._active:
            self.model.set_status(self._active, "idle")
            self.model.set_ka(self._active, None)
        self._active = None
        self._set_connected(False)
        self._log("Disconnected.")

    def _on_bleak_disconnect(self, _client) -> None:
        # Called by bleak when the link drops on its own; marshal to the UI.
        QTimer.singleShot(0, self._after_drop)

    def _after_drop(self) -> None:
        if self._batch_running:
            return  # batch manages its own connect/disconnect lifecycle
        if self._active is None and not self.link.connected:
            return
        self._stop_keepalive()
        if self._active:
            self.model.set_status(self._active, "idle")
            self.model.set_ka(self._active, None)
        self._active = None
        self._set_connected(False)
        self._log("Link dropped.")

    # ---------- Direct (GATT) tab ----------
    # Per-badge persistent config, organized into dedicated sections for the
    # identity frame, text frames, and image frames (plus firmware). Each frame
    # section holds its own edit controls, LED config, and a "show on badge" button.
    def _build_gatt_tab(self) -> QWidget:
        self._gatt_actions = []
        inner = QTabWidget()
        inner.addTab(self._scrollable(self._build_id_subtab()), "Identity")
        inner.addTab(self._scrollable(self._build_text_subtab()), "Text frames")
        inner.addTab(self._scrollable(self._build_image_subtab()), "Image frames")
        inner.addTab(self._scrollable(self._build_fw_subtab()), "Firmware")
        return inner

    # -- shared per-frame LED + display helpers (state keyed by `which`) --
    def _update_led_swatch(self, which: str) -> None:
        r, g, b = getattr(self, f"_{which}_led_color")
        getattr(self, f"_{which}_led_swatch").setStyleSheet(
            f"background: rgb({r},{g},{b}); border:1px solid #3a3f4b; border-radius:4px;")

    def _pick_led_color(self, which: str) -> None:
        c = QColorDialog.getColor(QColor(*getattr(self, f"_{which}_led_color")), self, "Frame LED color")
        if c.isValid():
            setattr(self, f"_{which}_led_color", (c.red(), c.green(), c.blue()))
            self._update_led_swatch(which)

    def _led_group(self, which: str, kind: int, idx_getter) -> QGroupBox:
        g = QGroupBox("LED while this frame is active")
        gl = QHBoxLayout(g)
        anim = QComboBox()
        for _code, _nm in keys.ANIM_NAMES.items():
            anim.addItem(_nm, _code)
        anim.setCurrentIndex(anim.findData(keys.ANIM_SOLID))
        anim.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToContents)
        anim.setMinimumContentsLength(8)  # fits "Rainbow"/"Sparkle"
        setattr(self, f"_{which}_led_anim", anim)
        sw = QLabel()
        sw.setFixedSize(40, 24)
        setattr(self, f"_{which}_led_swatch", sw)
        setattr(self, f"_{which}_led_color", (255, 255, 255))
        self._update_led_swatch(which)
        pick = QPushButton("Pick…")
        pick.setObjectName("ghost")
        pick.clicked.connect(lambda: self._pick_led_color(which))
        send = QPushButton("Set LED")
        send.clicked.connect(lambda: self._go(self._send_frame_led(which, kind, idx_getter())))
        gl.addWidget(QLabel("Anim"))
        gl.addWidget(anim)
        gl.addWidget(sw)
        gl.addWidget(pick)
        gl.addStretch()
        gl.addWidget(send)
        self._gatt_actions += [pick, send]
        return g

    def _display_button(self, kind: int, idx_getter, label: str) -> QPushButton:
        b = QPushButton(label)
        b.clicked.connect(lambda: self._go(self._do_display(kind, idx_getter())))
        self._gatt_actions.append(b)
        return b

    async def _send_frame_led(self, which: str, kind: int, idx: int) -> None:
        anim = getattr(self, f"_{which}_led_anim").currentData()
        r, g, b = getattr(self, f"_{which}_led_color")
        if kind == keys.DISP_KIND_IDENTITY:
            # Identity LED is part of the identity meta (name + ID + LED).
            await self.link.write_identity_meta(self.g_name.text(), self.g_attendee.text(),
                                                anim, r, g, b)
        elif kind == keys.DISP_KIND_TEXT:
            await self.link.write_text_led(idx, anim, r, g, b)
        else:
            await self.link.write_image_led(idx, anim, r, g, b)
        self._log(f"{which} LED: idx={idx} anim={anim} #{r:02x}{g:02x}{b:02x}.")

    async def _do_display(self, kind: int, idx: int) -> None:
        await self.link.display(kind, idx)
        self._log(f"Force-display kind={kind} idx={idx} (connection banner stays).")

    def _apply_loaded_led(self, which: str, led: dict) -> None:
        combo = getattr(self, f"_{which}_led_anim")
        i = combo.findData(led["anim"])
        combo.setCurrentIndex(i if i >= 0 else combo.findData(keys.ANIM_SOLID))
        setattr(self, f"_{which}_led_color", (led["r"], led["g"], led["b"]))
        self._update_led_swatch(which)

    def _build_id_subtab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        g = QGroupBox("Name + ID (loaded from the badge on connect)")
        fl = QFormLayout(g)
        self.g_name = QLineEdit()
        self.g_name.setPlaceholderText("Attendee name")
        self.g_attendee = QLineEdit()
        self.g_attendee.setPlaceholderText("Table / attendee ID (≤10 chars)")
        save = QPushButton("Save name / ID / LED")
        save.clicked.connect(lambda: self._go(self._save_identity()))
        fl.addRow("Name", self.g_name)
        fl.addRow("ID string", self.g_attendee)
        fl.addRow("", save)
        v.addWidget(g)

        v.addWidget(self._led_group("id", keys.DISP_KIND_IDENTITY, lambda: 0))

        g = QGroupBox("Identity image (fills the frame behind the name/ID banner)")
        gl = QGridLayout(g)
        self.g_idimg_fmt = QComboBox()
        self.g_idimg_fmt.addItem("2-bit grayscale", keys.FMT_GRAY2)
        self.g_idimg_fmt.addItem("1-bit B/W", keys.FMT_BW)
        self.g_idimg_fmt.currentIndexChanged.connect(
            lambda: self._reload_image(self.g_idimg_path, self.g_idimg_preview,
                                       self._idimg_fmt_name(), "_idimg", True))
        self.g_idimg_path = QLineEdit()
        self.g_idimg_path.setReadOnly(True)
        ibrowse = QPushButton("Browse…")
        ibrowse.setObjectName("ghost")
        ibrowse.clicked.connect(
            lambda: self._pick_image(self.g_idimg_path, self.g_idimg_preview,
                                     self._idimg_fmt_name(), "_idimg", True))
        self.g_idimg_preview = QLabel()
        self.g_idimg_preview.setFixedSize(264, 176)
        self.g_idimg_preview.setStyleSheet("background:#11141a;border:1px solid #3a3f4b;border-radius:6px;")
        self.g_idimg_prog = QProgressBar()
        idload = QPushButton("Load current")
        idload.setObjectName("ghost")
        idload.clicked.connect(lambda: self._go(self._load_identity_pixels()))
        iup = QPushButton("Upload")
        iup.clicked.connect(lambda: self._go(self._upload_identity_image()))
        gl.addWidget(QLabel("Format"), 0, 0)
        gl.addWidget(self.g_idimg_fmt, 0, 1)
        gl.addWidget(self.g_idimg_path, 1, 0)
        gl.addWidget(ibrowse, 1, 1)
        gl.addWidget(self.g_idimg_preview, 2, 0, 1, 2, Qt.AlignmentFlag.AlignHCenter)
        gl.addWidget(self.g_idimg_prog, 3, 0, 1, 2)
        gl.addWidget(idload, 4, 0)
        gl.addWidget(iup, 4, 1)
        v.addWidget(g)

        row = QHBoxLayout()
        row.addStretch()
        row.addWidget(self._display_button(keys.DISP_KIND_IDENTITY, lambda: 0, "Show identity on badge"))
        v.addLayout(row)
        v.addStretch()

        self._gatt_actions += [self.g_name, self.g_attendee, save, ibrowse, idload, iup]
        return w

    def _build_text_subtab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        g = QGroupBox("Edit text frame (loaded from the badge when you pick a frame)")
        fl = QFormLayout(g)
        self.g_scr_idx = QSpinBox()
        self.g_scr_idx.setRange(0, 19)
        self.g_scr_idx.valueChanged.connect(
            lambda: self._load_if_connected(self._load_text_frame(self.g_scr_idx.value())))
        self.g_scr_hdr = QLineEdit()
        self.g_scr_body = QPlainTextEdit()
        self.g_scr_body.setFixedHeight(80)
        sb = QPushButton("Store")
        sb.clicked.connect(lambda: self._go(self._gatt_set_screen()))
        fl.addRow("Frame (0–19)", self.g_scr_idx)
        fl.addRow("Title", self.g_scr_hdr)
        fl.addRow("Body", self.g_scr_body)
        fl.addRow("", sb)
        v.addWidget(g)

        v.addWidget(self._led_group("txt", keys.DISP_KIND_TEXT, lambda: self.g_scr_idx.value()))

        row = QHBoxLayout()
        row.addStretch()
        row.addWidget(self._display_button(keys.DISP_KIND_TEXT, lambda: self.g_scr_idx.value(),
                                           "Show this text frame"))
        v.addLayout(row)
        v.addStretch()

        self._gatt_actions += [sb]
        return w

    def _build_image_subtab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        g = QGroupBox("Edit image frame (LED loaded from the badge when you pick a slot)")
        gl = QGridLayout(g)
        self.g_slot_idx = QSpinBox()
        self.g_slot_idx.setRange(0, 3)
        self.g_slot_idx.valueChanged.connect(
            lambda: self._load_if_connected(self._load_image_frame(self.g_slot_idx.value())))
        self.g_slot_fmt = QComboBox()
        self.g_slot_fmt.addItem("1-bit B/W", keys.FMT_BW)
        self.g_slot_fmt.addItem("2-bit grayscale", keys.FMT_GRAY2)
        self.g_slot_fmt.currentIndexChanged.connect(
            lambda: self._reload_image(self.g_slot_path, self.g_slot_preview, self._slot_fmt_name(), "_slot_img", True)
        )
        self.g_slot_path = QLineEdit()
        self.g_slot_path.setReadOnly(True)
        sbrowse = QPushButton("Browse…")
        sbrowse.setObjectName("ghost")
        sbrowse.clicked.connect(
            lambda: self._pick_image(self.g_slot_path, self.g_slot_preview, self._slot_fmt_name(), "_slot_img", True)
        )
        self.g_slot_preview = QLabel()
        self.g_slot_preview.setFixedSize(264, 176)
        self.g_slot_preview.setStyleSheet("background:#11141a;border:1px solid #3a3f4b;border-radius:6px;")
        self.g_slot_prog = QProgressBar()
        iload = QPushButton("Load current")
        iload.setObjectName("ghost")
        iload.clicked.connect(lambda: self._go(self._load_image_pixels(self.g_slot_idx.value())))
        sup = QPushButton("Store")
        sup.clicked.connect(lambda: self._go(self._upload_slot()))
        gl.addWidget(QLabel("Frame"), 0, 0)
        gl.addWidget(self.g_slot_idx, 0, 1)
        gl.addWidget(QLabel("Format"), 0, 2)
        gl.addWidget(self.g_slot_fmt, 0, 3)
        gl.addWidget(self.g_slot_path, 1, 0, 1, 3)
        gl.addWidget(sbrowse, 1, 3)
        gl.addWidget(self.g_slot_preview, 2, 0, 1, 4, Qt.AlignmentFlag.AlignHCenter)
        gl.addWidget(self.g_slot_prog, 3, 0, 1, 4)
        gl.addWidget(iload, 4, 0)
        gl.addWidget(sup, 4, 3)
        v.addWidget(g)

        v.addWidget(self._led_group("img", keys.DISP_KIND_IMAGE, lambda: self.g_slot_idx.value()))

        row = QHBoxLayout()
        row.addStretch()
        row.addWidget(self._display_button(keys.DISP_KIND_IMAGE, lambda: self.g_slot_idx.value(),
                                           "Show this image frame"))
        v.addLayout(row)
        v.addStretch()

        self._gatt_actions += [sbrowse, iload, sup]
        return w

    def _build_fw_subtab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        # Firmware OTA (direct-XIP A/B)
        g = QGroupBox("Firmware update / OTA (f0de0009)")
        gl = QGridLayout(g)
        self.g_ota_path = QLineEdit()
        self.g_ota_path.setReadOnly(True)
        self.g_ota_path.setPlaceholderText("vibamix.ota bundle (build/vibamix.ota)")
        obrowse = QPushButton("Browse…")
        obrowse.setObjectName("ghost")
        obrowse.clicked.connect(self._pick_ota)
        self.g_ota_prog = QProgressBar()
        oup = QPushButton("Upload + reboot")
        oup.clicked.connect(lambda: self._go(self._upload_ota()))
        warn = QLabel("Pick the .ota bundle (both slots). Reads which slot is inactive, "
                      "extracts + sends that slot's image; badge verifies, reboots into it "
                      "(~5 s), then auto-reverts if it fails.")
        warn.setWordWrap(True)
        warn.setStyleSheet("color:#9aa4b2;")
        gl.addWidget(self.g_ota_path, 0, 0)
        gl.addWidget(obrowse, 0, 1)
        gl.addWidget(self.g_ota_prog, 1, 0, 1, 2)
        gl.addWidget(warn, 2, 0, 1, 2)
        gl.addWidget(oup, 3, 1)
        v.addWidget(g)
        v.addStretch()

        self._gatt_actions += [obrowse, oup]
        return w

    def _slot_fmt_name(self) -> str:
        return "bw" if self.g_slot_fmt.currentData() == keys.FMT_BW else "gray2"

    def _idimg_fmt_name(self) -> str:
        return "bw" if self.g_idimg_fmt.currentData() == keys.FMT_BW else "gray2"

    def _pick_image(self, path_edit, preview, fmt, attr, _is_gatt) -> None:
        fn, _ = QFileDialog.getOpenFileName(self, "Pick image", "", "Images (*.png *.jpg *.jpeg *.bmp *.gif)")
        if not fn:
            return
        path_edit.setText(fn)
        self._reload_image(path_edit, preview, fmt, attr, _is_gatt)

    def _reload_image(self, path_edit, preview, fmt, attr, _is_gatt) -> None:
        path = path_edit.text()
        if not path:
            return
        try:
            if fmt == "bw":
                buf = imageconv.to_bw(path, dither=True)
            else:
                buf = imageconv.to_gray2(path)
            setattr(self, attr, buf)
            preview.setPixmap(pil_to_pixmap(imageconv.unpack_preview(buf, fmt)))
        except Exception as e:  # noqa: BLE001
            self._log(f"image error: {e}")

    def _pick_ota(self) -> None:
        fn, _ = QFileDialog.getOpenFileName(self, "Pick .ota bundle", "", "OTA bundle (*.ota)")
        if fn:
            self.g_ota_path.setText(fn)

    async def _upload_ota(self) -> None:
        ota = self.g_ota_path.text()
        if not ota:
            self._log("Pick the .ota bundle first (build/vibamix.ota).")
            return
        active, inactive, ver = await self.link.read_ota_status()
        self._log(f"OTA: active slot {active} (v{ver}); sending bundle image for inactive slot {inactive} …")
        self.g_ota_prog.setValue(0)
        slot, version = await self.link.ota_update_file(
            ota, on_progress=lambda d, n: self.g_ota_prog.setValue(int(d * 100 / n)))
        self._log(f"OTA: slot {slot} image (v{version}) sent. Badge verifies + reboots (~5 s); "
                  "auto-reverts if it can't confirm. Reconnect after.")

    def _led_state(self, which: str):
        return (getattr(self, f"_{which}_led_anim").currentData(),
                *getattr(self, f"_{which}_led_color"))

    async def _save_identity(self) -> None:
        anim, r, g, b = self._led_state("id")
        await self.link.write_identity_meta(self.g_name.text(), self.g_attendee.text(), anim, r, g, b)
        self._log(f"Saved identity: name={self.g_name.text()!r} id={self.g_attendee.text()!r}.")

    async def _gatt_set_screen(self) -> None:
        idx = self.g_scr_idx.value()
        anim, r, g, b = self._led_state("txt")
        await self.link.write_text_frame(idx, anim, r, g, b,
                                         self.g_scr_hdr.text(), self.g_scr_body.toPlainText())
        self._log(f"Stored text frame {idx}.")

    async def _upload_slot(self) -> None:
        if not self._slot_img:
            self._log("Pick an image for the frame first.")
            return
        slot = self.g_slot_idx.value()
        fmt = self.g_slot_fmt.currentData()
        anim, r, g, b = self._led_state("img")
        self._log(f"Storing image frame {slot} ({'BW' if fmt == keys.FMT_BW else 'gray2'}) …")
        await self.link.write_image_frame(
            slot, fmt, anim, r, g, b, self._slot_img,
            on_progress=lambda d, n: self.g_slot_prog.setValue(int(d * 100 / n))
        )
        self._log(f"Image frame {slot} stored + displayed.")

    async def _upload_identity_image(self) -> None:
        if not self._idimg:
            self._log("Pick an identity image first.")
            return
        fmt = self.g_idimg_fmt.currentData()
        self._log(f"Uploading identity image ({'BW' if fmt == keys.FMT_BW else 'gray2'}) …")
        await self.link.upload_identity_image(
            self._idimg, fmt,
            on_progress=lambda d, n: self.g_idimg_prog.setValue(int(d * 100 / n))
        )
        self._log("Identity image stored; identity frame redrawn.")

    # ---------- auto-load current data ----------
    def _load_if_connected(self, coro) -> None:
        """Schedule a frame load only when connected; else drop the coroutine."""
        if self.link.connected:
            self._go(coro)
        else:
            coro.close()

    async def _load_frames(self) -> None:
        """Pull the badge's current config into the edit tabs on connect."""
        try:
            ident = await self.link.read_identity()
            self.g_name.setText(ident["name"])
            self.g_attendee.setText(ident["attendee"])
            self._apply_loaded_led("id", ident["led"])
            await self._load_text_frame(self.g_scr_idx.value())
            await self._load_image_frame(self.g_slot_idx.value())
            self._log("Loaded current badge config into the tabs.")
        except Exception as e:  # noqa: BLE001
            self._log(f"load error: {e}")

    async def _load_text_frame(self, idx: int) -> None:
        t = await self.link.read_text_frame(idx)
        self.g_scr_hdr.setText(t["header"])
        self.g_scr_body.setPlainText(t["body"])
        self._apply_loaded_led("txt", t["led"])

    async def _load_image_frame(self, slot: int) -> None:
        im = await self.link.read_image_frame(slot, want_pixels=False)
        self._apply_loaded_led("img", im["led"])
        self.g_slot_preview.clear()
        if im["present"]:
            fmt = "BW" if im["fmt"] == keys.FMT_BW else "gray2"
            self.g_slot_path.setText(f"(on badge: {fmt} {im['w']}x{im['h']} — Load current to preview)")
        else:
            self.g_slot_path.setText("(empty)")

    def _show_preview(self, label, fmt_code: int, pixels: bytes) -> None:
        fmt = "bw" if fmt_code == keys.FMT_BW else "gray2"
        pm = pil_to_pixmap(imageconv.unpack_preview(pixels, fmt)).scaled(
            label.size(), Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation)
        label.setPixmap(pm)

    async def _load_identity_pixels(self) -> None:
        self.g_idimg_prog.setValue(0)
        ident = await self.link.read_identity(
            want_pixels=True,
            on_progress=lambda d, n: self.g_idimg_prog.setValue(int(d * 100 / n)))
        img = ident["image"]
        if img["present"] and img.get("pixels"):
            self._show_preview(self.g_idimg_preview, img["fmt"], img["pixels"])
            self._log(f"Loaded identity image ({img['w']}x{img['h']}).")
        else:
            self.g_idimg_prog.setValue(0)
            self._log("No identity image stored.")

    async def _load_image_pixels(self, slot: int) -> None:
        self.g_slot_prog.setValue(0)
        im = await self.link.read_image_frame(
            slot, want_pixels=True,
            on_progress=lambda d, n: self.g_slot_prog.setValue(int(d * 100 / n)))
        if im["present"] and im.get("pixels"):
            self._show_preview(self.g_slot_preview, im["fmt"], im["pixels"])
            self._log(f"Loaded image frame {slot} ({im['w']}x{im['h']}).")
        else:
            self.g_slot_prog.setValue(0)
            self._log(f"Image frame {slot} is empty.")

    # ---------- Mesh tab ----------
    def _build_mesh_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        info = QLabel(
            "Broadcasts to ALL badges. Connect to a config-mode badge first (press "
            "its button) — it acts as the gateway and re-originates these onto the "
            "mesh. Receiving badges must be awake (boot window or their own config mode).")
        info.setWordWrap(True)
        info.setStyleSheet("color:#9aa4b2;")
        v.addWidget(info)

        # PARTY MODE: one-click rainbow-sparkle @ full brightness on the whole fleet.
        # Toggles: a second click releases brightness control and sends every badge
        # back to its identity frame. The button pulses a scrolling rainbow while on.
        self._party_on = False
        self._party_phase = 0.0
        self._party_timer = QTimer(self)
        self._party_timer.timeout.connect(self._party_pulse)
        self.party_btn = QPushButton("🦄  PARTY MODE  🌈")
        self.party_btn.setObjectName("party")
        self.party_btn.setMinimumHeight(48)
        self.party_btn.setStyleSheet(self._party_style(0.0))
        self.party_btn.clicked.connect(self._toggle_party)
        v.addWidget(self.party_btn)

        g = QGroupBox("Event heartbeat (mesh — keep badges awake)")
        gl = QHBoxLayout(g)
        self.m_event = QLineEdit()
        self.m_event.setMaxLength(8)   # >8 bytes would segment the 1 Hz flood
        self.m_event.setPlaceholderText("event name (≤8)")
        self.m_event.setFixedWidth(140)
        hb = QPushButton("Send now")
        hb.clicked.connect(lambda: self._go(self._send_heartbeat()))
        self.m_hb_auto = QCheckBox("Auto every 1 s")
        self.m_hb_auto.stateChanged.connect(self._toggle_hb_auto)
        gl.addWidget(QLabel("Event"))
        gl.addWidget(self.m_event)
        gl.addWidget(hb)
        gl.addWidget(self.m_hb_auto)
        gl.addStretch()
        v.addWidget(g)

        # LED override: broadcast a live animation + color to every badge. Not
        # stored — the next frame display (or reboot) restores each frame's own LED.
        g = QGroupBox("LED — override all badges (live, not stored)")
        gl = QHBoxLayout(g)
        self.m_swatch = QLabel()
        self.m_swatch.setFixedSize(40, 24)
        self._update_swatch()
        pick = QPushButton("Pick…")
        pick.setObjectName("ghost")
        pick.clicked.connect(self._pick_color)
        self.m_anim = QComboBox()
        for _code, _nm in keys.ANIM_NAMES.items():
            self.m_anim.addItem(_nm, _code)
        self.m_anim.setCurrentIndex(self.m_anim.findData(keys.ANIM_SOLID))
        self.m_anim.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToContents)
        self.m_anim.setMinimumContentsLength(8)  # fits "Rainbow"/"Sparkle"
        cb = QPushButton("Send LED")
        cb.clicked.connect(lambda: self._go(self._mesh_send(
            keys.OP_SET_LED, bytes([self.m_anim.currentData()]) + bytes(self._led))))
        gl.addWidget(self.m_swatch)
        gl.addWidget(pick)
        gl.addWidget(QLabel("Animation"))
        gl.addWidget(self.m_anim)
        gl.addStretch()
        gl.addWidget(cb)
        v.addWidget(g)

        # Brightness override: pin LED brightness on every badge, overriding the
        # ambient-light auto-adjust. Lasts for the current LED state only — the next
        # animation/color/frame change resumes auto-brightness.
        g = QGroupBox("Brightness — override all badges (current LED state only)")
        gl = QHBoxLayout(g)
        self.m_bright = QSlider(Qt.Orientation.Horizontal)
        self.m_bright.setRange(0, 255)
        self.m_bright.setValue(255)
        self.m_bright_val = QLabel("255")
        self.m_bright.valueChanged.connect(
            lambda v: self.m_bright_val.setText(str(v)))
        bb = QPushButton("Send Brightness")
        bb.clicked.connect(lambda: self._go(self._mesh_send(
            keys.OP_SET_BRIGHTNESS, bytes([self.m_bright.value()]))))
        # Hand brightness back to each badge's ambient-light auto-adjust without
        # changing the running animation/color.
        rb = QPushButton("Release (auto)")
        rb.setObjectName("ghost")
        rb.clicked.connect(lambda: self._go(self._mesh_send(
            keys.OP_RELEASE_BRIGHTNESS, b"")))
        gl.addWidget(QLabel("Level"))
        gl.addWidget(self.m_bright)
        gl.addWidget(self.m_bright_val)
        gl.addWidget(bb)
        gl.addWidget(rb)
        v.addWidget(g)

        # Config mode: remotely enter/exit config mode (QR onboarding screen +
        # connectable advertiser). Best-effort — only reaches badges that are
        # currently awake (a sleeping badge's radio is off; press its button).
        g = QGroupBox("Config mode — toggle on all awake badges")
        gl = QHBoxLayout(g)
        cm_on = QPushButton("Enter config mode")
        cm_on.clicked.connect(lambda: self._go(self._mesh_send(
            keys.OP_SET_CONFIG_MODE, bytes([1]))))
        cm_off = QPushButton("Exit config mode")
        cm_off.setObjectName("ghost")
        cm_off.clicked.connect(lambda: self._go(self._mesh_send(
            keys.OP_SET_CONFIG_MODE, bytes([0]))))
        gl.addWidget(QLabel("Awake badges only"))
        gl.addStretch()
        gl.addWidget(cm_on)
        gl.addWidget(cm_off)
        v.addWidget(g)

        # Show text: draw a text frame straight to every badge's panel. Not stored
        # (a reboot returns to the identity frame). To store a frame, use the Direct tab.
        g = QGroupBox("Show text — draw on all badges (not stored)")
        gl = QFormLayout(g)
        self.m_scr_hdr = QLineEdit()
        self.m_scr_body = QPlainTextEdit()
        self.m_scr_body.setFixedHeight(60)
        msb = QPushButton("Show")
        msb.clicked.connect(lambda: self._go(self._mesh_show_text()))
        gl.addRow("Title", self.m_scr_hdr)
        gl.addRow("Body", self.m_scr_body)
        gl.addRow("", msb)
        v.addWidget(g)

        # Display a stored frame: tell every badge to show a frame it already has
        # stored (identity, or an image/text frame by index). No content is sent —
        # badges that don't have that frame stored just ignore it.
        g = QGroupBox("Display a stored frame — on all badges")
        gl = QHBoxLayout(g)
        self.m_disp_kind = QComboBox()
        self.m_disp_kind.addItem("Identity", keys.DISP_KIND_IDENTITY)
        self.m_disp_kind.addItem("Text frame", keys.DISP_KIND_TEXT)
        self.m_disp_kind.addItem("Image frame", keys.DISP_KIND_IMAGE)
        self.m_disp_idx = QSpinBox()
        self.m_disp_idx.setRange(0, 19)
        self.m_disp_kind.currentIndexChanged.connect(self._sync_disp_idx)
        dsb = QPushButton("Show on all")
        dsb.clicked.connect(lambda: self._go(self._mesh_display_frame()))
        gl.addWidget(QLabel("Frame"))
        gl.addWidget(self.m_disp_kind)
        gl.addWidget(QLabel("Index"))
        gl.addWidget(self.m_disp_idx)
        gl.addStretch()
        gl.addWidget(dsb)
        v.addWidget(g)
        self._sync_disp_idx()
        v.addStretch()

        self._mesh_actions = [hb, pick, cb, msb, dsb, self.m_hb_auto]
        return w

    def _sync_disp_idx(self) -> None:
        # Identity has no index; text frames are 0-19, image frames 0-3.
        kind = self.m_disp_kind.currentData()
        self.m_disp_idx.setEnabled(kind != keys.DISP_KIND_IDENTITY)
        self.m_disp_idx.setMaximum(3 if kind == keys.DISP_KIND_IMAGE else 19)

    async def _mesh_display_frame(self) -> None:
        kind = self.m_disp_kind.currentData()
        idx = 0 if kind == keys.DISP_KIND_IDENTITY else self.m_disp_idx.value()
        await self._mesh_send(keys.OP_DISPLAY, bytes([kind, idx]))

    # ---------- Batch tab ----------
    def _build_batch_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)
        info = QLabel(
            "Run one GATT action on every CHECKED device, one at a time "
            "(connect → do → disconnect). The action uses the values you set on the "
            "Direct (GATT) tab. For broadcasting to everyone at once, use the Mesh tab.")
        info.setWordWrap(True)
        info.setStyleSheet("color:#9aa4b2;")
        v.addWidget(info)

        g = QGroupBox("Batch action")
        gl = QGridLayout(g)
        self.batch_action = QComboBox()
        self.batch_action.addItem("Set name (GATT Name field)", "name")
        self.batch_action.addItem("Store text screen (GATT Text screen)", "screen")
        self.batch_action.addItem("Upload image slot (GATT Image slot)", "slot")
        self.batch_action.addItem("Display identity frame", "display")
        self.batch_action.addItem("Firmware OTA (.ota bundle)", "ota")
        self.batch_run = QPushButton("Run on checked")
        self.batch_run.clicked.connect(lambda: self._go(self._run_batch()))
        self.batch_cancel_btn = QPushButton("Cancel")
        self.batch_cancel_btn.setObjectName("ghost")
        self.batch_cancel_btn.setEnabled(False)
        self.batch_cancel_btn.clicked.connect(self._cancel_batch)
        self.batch_prog = QProgressBar()
        self.batch_status = QLabel("idle")
        self.batch_status.setStyleSheet("color:#9aa4b2;")
        gl.addWidget(QLabel("Action"), 0, 0)
        gl.addWidget(self.batch_action, 0, 1, 1, 2)
        gl.addWidget(self.batch_run, 1, 1)
        gl.addWidget(self.batch_cancel_btn, 1, 2)
        gl.addWidget(self.batch_prog, 2, 0, 1, 3)
        gl.addWidget(self.batch_status, 3, 0, 1, 3)
        v.addWidget(g)
        v.addStretch()
        return w

    # ---------- Flash tab (SWD bulk-flash over probe-rs) ----------
    def _build_flash_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)
        info = QLabel(
            "Flash the bootloader + a bootable app to EVERY attached probe over "
            "SWD (probe-rs), in parallel. Build the images first with "
            "vibamix_zephyr/scripts/build_slots.sh — the app must be the trailered "
            "slotA.bin so the bootloader's CRC check passes and the board boots "
            "standalone. Independent of the BLE connection above. Or use Reset all "
            "to soft-reboot every attached board (probe-rs reset) without reflashing "
            "— stored content is preserved.")
        info.setWordWrap(True)
        info.setStyleSheet("color:#9aa4b2;")
        v.addWidget(info)

        g = QGroupBox("Firmware images")
        gl = QGridLayout(g)
        self.fl_bl_path = QLineEdit(str(flashconfig.DEFAULT_BOOTLOADER_HEX))
        bl_browse = QPushButton("Browse")
        bl_browse.setObjectName("ghost")
        bl_browse.clicked.connect(
            lambda: self._browse_into(self.fl_bl_path, "Bootloader .hex", "HEX (*.hex)"))
        self.fl_app_path = QLineEdit(str(flashconfig.DEFAULT_APP_BIN))
        app_browse = QPushButton("Browse")
        app_browse.setObjectName("ghost")
        app_browse.clicked.connect(
            lambda: self._browse_into(self.fl_app_path, "App slotA.bin", "BIN (*.bin)"))
        self.fl_factory = QCheckBox("Write per-unit factory id (mesh unicast / config code)")
        self.fl_factory.setChecked(True)
        self.fl_erase = QCheckBox(
            "Full chip erase first (factory-clean — wipes mesh provisioning / stored content)")
        self.fl_erase.setChecked(False)
        gl.addWidget(QLabel("Bootloader (.hex → 0x0)"), 0, 0)
        gl.addWidget(self.fl_bl_path, 0, 1)
        gl.addWidget(bl_browse, 0, 2)
        gl.addWidget(QLabel("App slot A (.bin → 0xE000)"), 1, 0)
        gl.addWidget(self.fl_app_path, 1, 1)
        gl.addWidget(app_browse, 1, 2)
        gl.addWidget(self.fl_factory, 2, 0, 1, 3)
        gl.addWidget(self.fl_erase, 3, 0, 1, 3)
        v.addWidget(g)

        self.fl_table = QTableWidget(0, 3)
        self.fl_table.setHorizontalHeaderLabels(["Serial", "Probe", "Status"])
        self.fl_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.fl_table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.fl_table.verticalHeader().setVisible(False)
        fhdr = self.fl_table.horizontalHeader()
        fhdr.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        v.addWidget(self.fl_table, 1)

        row = QHBoxLayout()
        self.fl_refresh = QPushButton("Refresh probes")
        self.fl_refresh.setObjectName("ghost")
        self.fl_refresh.clicked.connect(self._refresh_probes)
        self.fl_reset = QPushButton("Reset all")
        self.fl_reset.setObjectName("ghost")
        self.fl_reset.clicked.connect(self._reset_all)
        self.fl_flash = QPushButton("Flash all")
        self.fl_flash.clicked.connect(self._flash_all)
        row.addWidget(self.fl_refresh)
        row.addStretch()
        row.addWidget(self.fl_reset)
        row.addWidget(self.fl_flash)
        v.addLayout(row)

        self.flash_progress.connect(self._on_flash_progress)
        self.flash_result.connect(self._on_flash_result)
        self.flash_finished.connect(self._on_flash_finished)
        self.reset_finished.connect(self._on_reset_finished)

        self._fl_timer = QTimer(self)   # poll for plugged/unplugged probes
        self._fl_timer.setInterval(2_000)
        self._fl_timer.timeout.connect(self._refresh_probes)
        self._fl_timer.start()
        # Defer the first scan to the event loop: this tab is built during
        # __init__ before self.log exists, and _refresh_probes may log.
        QTimer.singleShot(0, self._refresh_probes)
        return w

    def _browse_into(self, edit: QLineEdit, title: str, filt: str) -> None:
        fn, _ = QFileDialog.getOpenFileName(self, title, edit.text() or "", filt)
        if fn:
            edit.setText(fn)

    def _set_probe_status(self, label: str, text: str) -> None:
        self._fl_status[label] = text
        row = self._fl_rows.get(label)
        if row is None or row >= self.fl_table.rowCount():
            return
        item = self.fl_table.item(row, 2)
        if item is None:
            item = QTableWidgetItem()
            self.fl_table.setItem(row, 2, item)
        item.setText(text)

    def _refresh_probes(self) -> None:
        if self._flash_running:
            return  # don't disturb the table mid-run (probes re-enumerate on reset)
        try:
            found = probes.list_probes()
        except RuntimeError as e:
            self._fl_timer.stop()   # probe-rs missing/broken — stop hammering it
            self._log(f"probe list: {e}")
            self.fl_flash.setEnabled(False)
            return
        self._fl_probes = found
        self._fl_rows = {}
        self.fl_table.setRowCount(len(found))
        for i, p in enumerate(found):
            self._fl_rows[p.label] = i
            self.fl_table.setItem(i, 0, QTableWidgetItem(p.serial or "—"))
            self.fl_table.setItem(i, 1, QTableWidgetItem(p.name))
            self.fl_table.setItem(i, 2, QTableWidgetItem(self._fl_status.get(p.label, "idle")))
        self.fl_flash.setEnabled(bool(found))

    def _flash_all(self) -> None:
        if self._flash_running:
            return
        bl = self.fl_bl_path.text().strip()
        app = self.fl_app_path.text().strip()
        for what, path in (("bootloader", bl), ("app", app)):
            if not path or not Path(path).is_file():
                self._log(f"Flash: {what} image not found — {path or '(empty)'}")
                return
        targets = list(self._fl_probes)
        if not targets:
            self._log("Flash: no probes detected.")
            return
        images = [
            flash.Image(bl, "hex", "bootloader"),
            flash.Image(app, "bin", "app", flashconfig.APP_SLOT_A_ADDR),
        ]
        want_factory = self.fl_factory.isChecked()
        want_erase = self.fl_erase.isChecked()

        self._flash_running = True
        self._fl_timer.stop()
        self.fl_flash.setEnabled(False)
        self.fl_reset.setEnabled(False)
        self.fl_refresh.setEnabled(False)
        for p in targets:
            self._set_probe_status(p.label, "queued")
        self._log(f"Flashing {len(targets)} probe(s) … "
                  f"factory id {'ON' if want_factory else 'off'}, "
                  f"erase {'ON' if want_erase else 'off'}.")

        t = threading.Thread(
            target=self._flash_worker, args=(targets, images, want_factory, want_erase),
            daemon=True)
        t.start()

    def _flash_worker(self, targets, images, want_factory: bool, want_erase: bool) -> None:
        """Runs off the Qt thread: one probe-rs flash per probe, bounded pool."""
        def one(p: probes.Probe) -> bool:
            try:
                fid = serialreg.assign_for(p.serial) if want_factory else None
            except RuntimeError as e:
                self.flash_result.emit(p.label, False, str(e))
                return False
            ok, msg = flash.flash_device(
                p.selector, images,
                lambda pct, phase: self.flash_progress.emit(p.label, pct, phase),
                fid, erase=want_erase)
            detail = msg if not ok else (f"id 0x{fid:04x}" if fid is not None else "ok")
            self.flash_result.emit(p.label, ok, detail)
            return ok

        ok_count = 0
        try:
            with ThreadPoolExecutor(max_workers=flashconfig.MAX_CONCURRENT) as ex:
                ok_count = sum(1 for r in ex.map(one, targets) if r)
        finally:
            self.flash_finished.emit(ok_count, len(targets))

    def _on_flash_progress(self, label: str, pct: float, phase: str) -> None:
        self._set_probe_status(label, f"{phase} {pct:.0f}%")

    def _on_flash_result(self, label: str, ok: bool, msg: str) -> None:
        self._set_probe_status(label, f"done ({msg})" if ok else f"FAILED: {msg}")
        self._log(f"flash {label}: {'ok — ' + msg if ok else 'FAILED — ' + msg}")

    def _on_flash_finished(self, ok: int, total: int) -> None:
        self._flash_running = False
        self.fl_refresh.setEnabled(True)
        self.fl_flash.setEnabled(True)
        self.fl_reset.setEnabled(True)
        self._fl_timer.start()
        self._log(f"Flash complete: {ok}/{total} ok.")

    def _reset_all(self) -> None:
        """Soft-reset every attached probe (probe-rs reset) — no reflash."""
        if self._flash_running:
            return
        targets = list(self._fl_probes)
        if not targets:
            self._log("Reset: no probes detected.")
            return
        self._flash_running = True   # shared busy flag: flash/reset can't overlap
        self._fl_timer.stop()
        self.fl_flash.setEnabled(False)
        self.fl_reset.setEnabled(False)
        self.fl_refresh.setEnabled(False)
        for p in targets:
            self._set_probe_status(p.label, "queued")
        self._log(f"Resetting {len(targets)} probe(s) …")

        t = threading.Thread(target=self._reset_worker, args=(targets,), daemon=True)
        t.start()

    def _reset_worker(self, targets) -> None:
        """Runs off the Qt thread: one probe-rs reset per probe, bounded pool."""
        def one(p: probes.Probe) -> bool:
            self.flash_progress.emit(p.label, 0.0, "resetting")
            ok, msg = flash.reset_device(p.selector)
            self.flash_result.emit(p.label, ok, msg)
            return ok

        ok_count = 0
        try:
            with ThreadPoolExecutor(max_workers=flashconfig.MAX_CONCURRENT) as ex:
                ok_count = sum(1 for r in ex.map(one, targets) if r)
        finally:
            self.reset_finished.emit(ok_count, len(targets))

    def _on_reset_finished(self, ok: int, total: int) -> None:
        self._flash_running = False
        self.fl_refresh.setEnabled(True)
        self.fl_flash.setEnabled(True)
        self.fl_reset.setEnabled(True)
        self._fl_timer.start()
        self._log(f"Reset complete: {ok}/{total} ok.")

    def _cancel_batch(self) -> None:
        self._batch_cancel = True
        self._log("Batch cancel requested …")

    async def _do_batch_action(self, action: str) -> None:
        if action == "name":
            anim, r, g, b = self._led_state("id")
            await self.link.write_identity_meta(self.g_name.text(), self.g_attendee.text(), anim, r, g, b)
        elif action == "screen":
            anim, r, g, b = self._led_state("txt")
            await self.link.write_text_frame(self.g_scr_idx.value(), anim, r, g, b,
                                             self.g_scr_hdr.text(), self.g_scr_body.toPlainText())
        elif action == "slot":
            if not self._slot_img:
                raise RuntimeError("pick an image on the GATT Image-frame section first")
            anim, r, g, b = self._led_state("img")
            await self.link.write_image_frame(self.g_slot_idx.value(), self.g_slot_fmt.currentData(),
                                              anim, r, g, b, self._slot_img)
        elif action == "display":
            await self.link.display(keys.DISP_KIND_IDENTITY, 0)
        elif action == "ota":
            if not self.g_ota_path.text():
                raise RuntimeError("pick the .ota bundle on the GATT tab first")
            await self.link.ota_update_file(self.g_ota_path.text())

    async def _run_batch(self) -> None:
        if self.link.connected:
            self._log("Disconnect the interactive connection before running a batch.")
            return
        targets = self.model.checked_devices()
        if not targets:
            self._log("Check some devices in the table first.")
            return
        action = self.batch_action.currentData()
        self._batch_cancel = False
        self._batch_running = True
        self.batch_run.setEnabled(False)
        self.batch_cancel_btn.setEnabled(True)
        self.connect_btn.setEnabled(False)
        self.batch_prog.setMaximum(len(targets))
        self._log(f"Batch '{action}' on {len(targets)} device(s) …")

        done = ok = 0
        for d in targets:
            if self._batch_cancel:
                break
            self.batch_status.setText(f"{d.name}: connecting …")
            self.model.set_status(d.address, "connecting")
            found = Found(name=d.name, address=d.address, is_proxy=d.is_proxy, device=d.device)
            try:
                await self.link.connect(found)   # no drop callback during batch
                self.model.set_status(d.address, "working")
                await self._do_batch_action(action)
                self.model.set_status(d.address, "done")
                ok += 1
            except Exception as e:  # noqa: BLE001
                self.model.set_status(d.address, "failed")
                self._log(f"batch {d.address}: {e}")
            finally:
                try:
                    await self.link.disconnect()
                except Exception:  # noqa: BLE001
                    pass
            done += 1
            self.batch_prog.setValue(done)

        self._batch_running = False
        self.batch_run.setEnabled(True)
        self.batch_cancel_btn.setEnabled(False)
        self._on_row_selected()
        self.batch_status.setText(f"done: {ok}/{len(targets)} ok"
                                  + (" (cancelled)" if self._batch_cancel else ""))
        self._log(f"Batch complete: {ok}/{len(targets)} ok.")

    # ---------- "Current data" (read-back) tab ----------
    @property
    def _action_widgets(self):
        return (getattr(self, "_gatt_actions", [])
                + getattr(self, "_mesh_actions", []))

    def _update_swatch(self) -> None:
        r, g, b = self._led
        self.m_swatch.setStyleSheet(f"background: rgb({r},{g},{b}); border:1px solid #3a3f4b; border-radius:4px;")

    def _pick_color(self) -> None:
        c = QColorDialog.getColor(QColor(*self._led), self, "LED color")
        if c.isValid():
            self._led = (c.red(), c.green(), c.blue())
            self._update_swatch()

    def _toggle_hb_auto(self) -> None:
        if self.m_hb_auto.isChecked():
            self._hb_timer.start()
            self._log("Auto event-heartbeat ON (every 1 s).")
        else:
            self._hb_timer.stop()
            self._log("Auto event-heartbeat OFF.")

    # mesh helpers — inject via the connected config-mode badge (the gateway),
    # which re-originates each vendor-model payload onto the mesh group.
    async def _mesh_send(self, op: int, params: bytes) -> None:
        if not self.link.connected:
            self._log("Not connected — connect to a config-mode badge to use it as the mesh gateway.")
            return
        await self.link.mesh_tx(keys.vendor_opcode(op) + params)
        self._last_link = time.monotonic()  # a completed acked write proves the link is up
        self._log(f"mesh op 0x{op:02x} broadcast ({len(params)}B params)")

    # ---------- PARTY MODE 🦄🌈 ----------
    def _party_style(self, phase: float) -> str:
        # A scrolling rainbow gradient for the button background. `phase` (0..1)
        # offsets the hue of every stop, so animating it makes the rainbow flow.
        n = 6
        stops = []
        for i in range(n + 1):
            hue = (phase + i / n) % 1.0
            c = QColor.fromHsvF(hue, 0.85, 1.0)
            stops.append(f"stop:{i / n:.3f} {c.name()}")
        grad = "qlineargradient(x1:0, y1:0, x2:1, y2:0, " + ", ".join(stops) + ")"
        return (
            f"QPushButton#party {{ background: {grad}; color: white; "
            "font-size: 18px; font-weight: 800; border: none; border-radius: 10px; "
            "padding: 10px; }"
        )

    def _party_pulse(self) -> None:
        self._party_phase = (self._party_phase + 0.02) % 1.0
        self.party_btn.setStyleSheet(self._party_style(self._party_phase))

    def _toggle_party(self) -> None:
        self._party_on = not self._party_on
        if self._party_on:
            self.party_btn.setText("🎉  PARTY MODE: ON — click to stop  🎉")
            self._party_timer.start(60)   # ~17 fps rainbow scroll
            self._go(self._party_start())
        else:
            self._party_timer.stop()
            self.party_btn.setText("🦄  PARTY MODE  🌈")
            self.party_btn.setStyleSheet(self._party_style(0.0))
            self._go(self._party_stop())

    async def _party_start(self) -> None:
        # Rainbow sparkle on every badge, pinned to full brightness. The pattern
        # ignores the color bytes, so they're arbitrary here.
        await self._mesh_send(keys.OP_SET_LED,
                              bytes([keys.ANIM_RAINBOW_SPARKLE, 255, 255, 255]))
        await self._mesh_send(keys.OP_SET_BRIGHTNESS, bytes([255]))
        self._log("PARTY MODE ON — rainbow sparkle @ full brightness on all badges 🎉")

    async def _party_stop(self) -> None:
        # Hand brightness back to ambient auto-adjust and send every badge home.
        await self._mesh_send(keys.OP_RELEASE_BRIGHTNESS, b"")
        await self._mesh_send(keys.OP_DISPLAY,
                              bytes([keys.DISP_KIND_IDENTITY, 0]))
        self._log("PARTY MODE OFF — brightness released, badges back to identity.")

    async def _send_heartbeat(self) -> None:
        # Carry the event name (≤8 bytes keeps the access payload unsegmented, so
        # each 1 Hz beat is a single mesh flood). Badges latch it for the bar.
        name = self.m_event.text().encode("utf-8")[:8]
        await self._mesh_send(keys.OP_HEARTBEAT, name)

    async def _mesh_show_text(self) -> None:
        # Draw a text frame on every badge (title + chunked body). Ephemeral — the
        # firmware renders it straight to the panel and never stores it. Pace the
        # chunks so the gateway's mesh adv buffers don't overflow.
        await self._mesh_send(keys.OP_SHOW_TEXT_HDR, self.m_scr_hdr.text().encode("utf-8")[:47])
        body = self.m_scr_body.toPlainText().encode("utf-8")
        chunk = 96
        segs = [body[i : i + chunk] for i in range(0, len(body), chunk)] or [b""]
        for seq, seg in enumerate(segs):
            last = 1 if seq == len(segs) - 1 else 0
            await self._mesh_send(keys.OP_SHOW_TEXT_BODY, bytes([seq, last]) + seg)
            await asyncio.sleep(0.03)
        self._log(f"mesh show text sent in {len(segs)} body chunk(s).")
