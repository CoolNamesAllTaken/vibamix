"""PyQt6 main window: scalable device table + Direct-GATT / Mesh / Batch panels."""

from __future__ import annotations

import asyncio
import datetime
import time
import traceback

from PyQt6.QtCore import QSortFilterProxyModel, Qt, QTimer
from PyQt6.QtGui import QColor, QImage, QPixmap
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QFileDialog,
    QFormLayout,
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
    QSpinBox,
    QSplitter,
    QTableView,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from bleak.exc import BleakDeviceNotFoundError

from . import imageconv, keys
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
QPlainTextEdit#log { font-family: Menlo, Consolas, monospace; font-size: 12px; }
QLabel#status { font-weight: 700; }
QTableView { background: #181b21; gridline-color: #2a2f3a; selection-background-color: #2d6cdf; }
QHeaderView::section { background: #20242c; border: none; padding: 4px; color: #9aa4b2; }
"""

KA_FRESH_S = 3.0  # keepalive considered alive if seen within this many seconds


def pil_to_pixmap(pil_img) -> QPixmap:
    img = pil_img.convert("L")
    w, h = img.size
    qimg = QImage(img.tobytes(), w, h, w, QImage.Format.Format_Grayscale8)
    return QPixmap.fromImage(qimg.copy())


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("badgectl — vibamix")
        self.resize(1280, 800)
        self.setStyleSheet(STYLE)

        self.link = BadgeLink()
        self.crypto = MeshCrypto(keys.NET_KEY, keys.APP_KEY, keys.IV_INDEX)
        self._seqstore: SeqStore | None = None
        self._led = (255, 80, 0)
        self._gatt_img: bytes | None = None
        self._slot_img: bytes | None = None

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
        self._last_rx = 0.0
        self._batch_running = False
        self._batch_cancel = False

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
        self._hb_timer.setInterval(60_000)
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

        row = QHBoxLayout()
        self.check_all_btn = QPushButton("Check all (filtered)")
        self.check_all_btn.setObjectName("ghost")
        self.check_all_btn.clicked.connect(lambda: self._check_visible(True))
        self.uncheck_all_btn = QPushButton("Uncheck all")
        self.uncheck_all_btn.setObjectName("ghost")
        self.uncheck_all_btn.clicked.connect(lambda: self._check_visible(False))
        row.addWidget(self.check_all_btn)
        row.addWidget(self.uncheck_all_btn)
        row.addStretch()
        v.addLayout(row)
        return w

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

    # ---------- action pane (right) ----------
    def _build_action_pane(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)
        v.addWidget(self._build_conn_header())
        tabs = QTabWidget()
        tabs.addTab(self._build_gatt_tab(), "Direct (GATT)")
        tabs.addTab(self._build_mesh_tab(), "Mesh")
        tabs.addTab(self._build_readback_tab(), "Current data")
        tabs.addTab(self._build_batch_tab(), "Batch")
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
        if self._active and self.link.connected:
            now = time.monotonic()
            rx_age = now - self._last_rx
            self.ka_dot.setStyleSheet("color:#5bd07a;" if rx_age < KA_FRESH_S else "color:#e06c5b;")
            self.ka_age.setText(f"badge {rx_age:.0f}s")
            self.model.set_ka(self._active, rx_age)

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
        await self._start_keepalive()

    async def _start_keepalive(self) -> None:
        self._last_rx = time.monotonic()
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
            await asyncio.sleep(1.0)

    def _on_ka_rx(self, _code: int) -> None:
        self._last_rx = time.monotonic()

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

    # ---------- GATT tab ----------
    def _build_gatt_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        # Name
        g = QGroupBox("Set name (f0de0003)")
        gl = QHBoxLayout(g)
        self.g_name = QLineEdit()
        self.g_name.setPlaceholderText("Attendee name")
        b = QPushButton("Send")
        b.clicked.connect(lambda: self._go(self._gatt_set_name()))
        gl.addWidget(self.g_name, 1)
        gl.addWidget(b)
        v.addWidget(g)

        # Render-only image
        g = QGroupBox("Render-only image (f0de0002, 1bpp)")
        gl = QGridLayout(g)
        self.g_img_path = QLineEdit()
        self.g_img_path.setReadOnly(True)
        browse = QPushButton("Browse…")
        browse.setObjectName("ghost")
        browse.clicked.connect(lambda: self._pick_image(self.g_img_path, self.g_img_preview, "bw", "_gatt_img", True))
        self.g_img_dither = QCheckBox("Floyd–Steinberg dither")
        self.g_img_dither.setChecked(True)
        self.g_img_dither.stateChanged.connect(lambda: self._reload_image(self.g_img_path, self.g_img_preview, "bw", "_gatt_img", True))
        self.g_img_preview = QLabel()
        self.g_img_preview.setFixedSize(264, 176)
        self.g_img_preview.setStyleSheet("background:#11141a;border:1px solid #3a3f4b;border-radius:6px;")
        self.g_img_prog = QProgressBar()
        up = QPushButton("Upload")
        up.clicked.connect(lambda: self._go(self._upload_render_image()))
        gl.addWidget(self.g_img_path, 0, 0)
        gl.addWidget(browse, 0, 1)
        gl.addWidget(self.g_img_dither, 1, 0)
        gl.addWidget(self.g_img_preview, 2, 0, 1, 2, Qt.AlignmentFlag.AlignHCenter)
        gl.addWidget(self.g_img_prog, 3, 0, 1, 2)
        gl.addWidget(up, 4, 1)
        v.addWidget(g)

        # Text screen
        g = QGroupBox("Store text screen (f0de0004)")
        gl = QFormLayout(g)
        self.g_scr_idx = QSpinBox()
        self.g_scr_idx.setRange(0, 19)
        self.g_scr_hdr = QLineEdit()
        self.g_scr_body = QPlainTextEdit()
        self.g_scr_body.setFixedHeight(70)
        sb = QPushButton("Send")
        sb.clicked.connect(lambda: self._go(self._gatt_set_screen()))
        gl.addRow("Index (0–19)", self.g_scr_idx)
        gl.addRow("Header", self.g_scr_hdr)
        gl.addRow("Body", self.g_scr_body)
        gl.addRow("", sb)
        v.addWidget(g)

        # Image slot
        g = QGroupBox("Store image slot (f0de0005)")
        gl = QGridLayout(g)
        self.g_slot_idx = QSpinBox()
        self.g_slot_idx.setRange(0, 3)
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
        sup = QPushButton("Upload")
        sup.clicked.connect(lambda: self._go(self._upload_slot()))
        gl.addWidget(QLabel("Slot"), 0, 0)
        gl.addWidget(self.g_slot_idx, 0, 1)
        gl.addWidget(QLabel("Format"), 0, 2)
        gl.addWidget(self.g_slot_fmt, 0, 3)
        gl.addWidget(self.g_slot_path, 1, 0, 1, 3)
        gl.addWidget(sbrowse, 1, 3)
        gl.addWidget(self.g_slot_preview, 2, 0, 1, 4, Qt.AlignmentFlag.AlignHCenter)
        gl.addWidget(self.g_slot_prog, 3, 0, 1, 4)
        gl.addWidget(sup, 4, 3)
        v.addWidget(g)

        # Display
        g = QGroupBox("Display stored screen (f0de0006)")
        gl = QHBoxLayout(g)
        self.g_disp_kind = QComboBox()
        self.g_disp_kind.addItem("Text screen", keys.DISP_KIND_TEXT)
        self.g_disp_kind.addItem("Image slot", keys.DISP_KIND_IMAGE)
        self.g_disp_idx = QSpinBox()
        self.g_disp_idx.setRange(0, 19)
        db = QPushButton("Show")
        db.clicked.connect(lambda: self._go(self._gatt_display()))
        gl.addWidget(QLabel("Kind"))
        gl.addWidget(self.g_disp_kind)
        gl.addWidget(QLabel("Index"))
        gl.addWidget(self.g_disp_idx)
        gl.addStretch()
        gl.addWidget(db)
        v.addWidget(g)

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

        self._gatt_actions = [self.g_name, b, browse, up, sb, sbrowse, sup, db, obrowse, oup]
        return w

    def _slot_fmt_name(self) -> str:
        return "bw" if self.g_slot_fmt.currentData() == keys.FMT_BW else "gray2"

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
                buf = imageconv.to_bw(path, dither=self.g_img_dither.isChecked() if attr == "_gatt_img" else True)
            else:
                buf = imageconv.to_gray2(path)
            setattr(self, attr, buf)
            preview.setPixmap(pil_to_pixmap(imageconv.unpack_preview(buf, fmt)))
        except Exception as e:  # noqa: BLE001
            self._log(f"image error: {e}")

    async def _upload_render_image(self) -> None:
        if not self._gatt_img:
            self._log("Pick an image first.")
            return
        self._log("Uploading render-only image …")
        await self.link.upload_image(self._gatt_img, on_progress=lambda d, n: self.g_img_prog.setValue(int(d * 100 / n)))
        self._log("Image upload complete (badge refreshes ~2 s).")

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

    async def _gatt_set_name(self) -> None:
        await self.link.set_name(self.g_name.text())
        self._log(f"GATT name: {self.g_name.text()!r}")

    async def _gatt_set_screen(self) -> None:
        idx = self.g_scr_idx.value()
        await self.link.set_screen(idx, self.g_scr_hdr.text(), self.g_scr_body.toPlainText())
        self._log(f"Stored text screen {idx}.")

    async def _upload_slot(self) -> None:
        if not self._slot_img:
            self._log("Pick an image for the slot first.")
            return
        slot = self.g_slot_idx.value()
        fmt = self.g_slot_fmt.currentData()
        self._log(f"Uploading slot {slot} ({'BW' if fmt == keys.FMT_BW else 'gray2'}) …")
        await self.link.upload_image_slot(
            slot, fmt, self._slot_img, on_progress=lambda d, n: self.g_slot_prog.setValue(int(d * 100 / n))
        )
        self._log(f"Slot {slot} stored + displayed.")

    async def _gatt_display(self) -> None:
        kind = self.g_disp_kind.currentData()
        idx = self.g_disp_idx.value()
        await self.link.display(kind, idx)
        self._log(f"Display kind={kind} idx={idx}.")

    # ---------- Mesh tab ----------
    def _build_mesh_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        g = QGroupBox("Routing")
        gl = QGridLayout(g)
        self.m_target = QComboBox()
        self.m_target.addItem("All badges (group 0xC000)", keys.GROUP_ADDR)
        self.m_target.addItem("Unicast…", -1)
        self.m_unicast = QLineEdit("0001")
        self.m_unicast.setFixedWidth(80)
        self.m_src = QLineEdit("0001")
        self.m_src.setFixedWidth(80)
        self.m_seq = QLabel("seq —")
        gl.addWidget(QLabel("Target"), 0, 0)
        gl.addWidget(self.m_target, 0, 1)
        gl.addWidget(QLabel("Unicast (hex)"), 0, 2)
        gl.addWidget(self.m_unicast, 0, 3)
        gl.addWidget(QLabel("Src (hex)"), 1, 0)
        gl.addWidget(self.m_src, 1, 1)
        gl.addWidget(self.m_seq, 1, 3)
        v.addWidget(g)

        g = QGroupBox("Event heartbeat (mesh — keep badges awake)")
        gl = QHBoxLayout(g)
        hb = QPushButton("Send now")
        hb.clicked.connect(lambda: self._go(self._send_heartbeat()))
        self.m_hb_auto = QCheckBox("Auto every 60 s")
        self.m_hb_auto.stateChanged.connect(self._toggle_hb_auto)
        gl.addWidget(hb)
        gl.addWidget(self.m_hb_auto)
        gl.addStretch()
        v.addWidget(g)

        g = QGroupBox("Set name / fun fact")
        gl = QGridLayout(g)
        self.m_name = QLineEdit()
        nb = QPushButton("Send name")
        nb.clicked.connect(lambda: self._go(self._mesh_send(keys.OP_SET_NAME, self.m_name.text().encode("utf-8"))))
        self.m_fact = QLineEdit()
        fb = QPushButton("Send fact")
        fb.clicked.connect(lambda: self._go(self._mesh_send(keys.OP_SET_FUN_FACT, self.m_fact.text().encode("utf-8"))))
        gl.addWidget(self.m_name, 0, 0)
        gl.addWidget(nb, 0, 1)
        gl.addWidget(self.m_fact, 1, 0)
        gl.addWidget(fb, 1, 1)
        v.addWidget(g)

        g = QGroupBox("LED color")
        gl = QHBoxLayout(g)
        self.m_swatch = QLabel()
        self.m_swatch.setFixedSize(40, 24)
        self._update_swatch()
        pick = QPushButton("Pick…")
        pick.setObjectName("ghost")
        pick.clicked.connect(self._pick_color)
        cb = QPushButton("Send color")
        cb.clicked.connect(lambda: self._go(self._mesh_send(keys.OP_SET_LED_COLOR, bytes(self._led))))
        gl.addWidget(self.m_swatch)
        gl.addWidget(pick)
        gl.addStretch()
        gl.addWidget(cb)
        v.addWidget(g)

        g = QGroupBox("Text screen")
        gl = QFormLayout(g)
        self.m_scr_idx = QSpinBox()
        self.m_scr_idx.setRange(0, 19)
        self.m_scr_hdr = QLineEdit()
        self.m_scr_body = QPlainTextEdit()
        self.m_scr_body.setFixedHeight(60)
        msb = QPushButton("Send")
        msb.clicked.connect(lambda: self._go(self._mesh_set_screen()))
        gl.addRow("Index", self.m_scr_idx)
        gl.addRow("Header", self.m_scr_hdr)
        gl.addRow("Body", self.m_scr_body)
        gl.addRow("", msb)
        v.addWidget(g)

        g = QGroupBox("Display stored screen")
        gl = QHBoxLayout(g)
        self.m_disp_kind = QComboBox()
        self.m_disp_kind.addItem("Text screen", keys.DISP_KIND_TEXT)
        self.m_disp_kind.addItem("Image slot", keys.DISP_KIND_IMAGE)
        self.m_disp_idx = QSpinBox()
        self.m_disp_idx.setRange(0, 19)
        mdb = QPushButton("Show")
        mdb.clicked.connect(lambda: self._go(self._mesh_send(keys.OP_DISPLAY, bytes([self.m_disp_kind.currentData(), self.m_disp_idx.value()]))))
        gl.addWidget(QLabel("Kind"))
        gl.addWidget(self.m_disp_kind)
        gl.addWidget(QLabel("Index"))
        gl.addWidget(self.m_disp_idx)
        gl.addStretch()
        gl.addWidget(mdb)
        v.addWidget(g)
        v.addStretch()

        self._mesh_actions = [hb, nb, fb, cb, pick, msb, mdb, self.m_hb_auto]
        return w

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
        self.batch_action.addItem("Display stored screen (GATT Display)", "display")
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

    def _cancel_batch(self) -> None:
        self._batch_cancel = True
        self._log("Batch cancel requested …")

    async def _do_batch_action(self, action: str) -> None:
        if action == "name":
            await self.link.set_name(self.g_name.text())
        elif action == "screen":
            await self.link.set_screen(self.g_scr_idx.value(), self.g_scr_hdr.text(),
                                       self.g_scr_body.toPlainText())
        elif action == "slot":
            if not self._slot_img:
                raise RuntimeError("pick an image on the GATT Image-slot section first")
            await self.link.upload_image_slot(self.g_slot_idx.value(),
                                              self.g_slot_fmt.currentData(), self._slot_img)
        elif action == "display":
            await self.link.display(self.g_disp_kind.currentData(), self.g_disp_idx.value())
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
    def _build_readback_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        read_btn = QPushButton("Read from badge")
        read_btn.clicked.connect(lambda: self._go(self._read_all()))
        v.addWidget(read_btn)

        g = QGroupBox("Identity")
        fl = QFormLayout(g)
        self.rb_name = QLabel("—")
        self.rb_fact = QLabel("—")
        self.rb_fact.setWordWrap(True)
        self.rb_attendee = QLabel("—")
        self.rb_swatch = QLabel()
        self.rb_swatch.setFixedSize(40, 18)
        self.rb_swatch.setStyleSheet("background:#2a2f3a;border:1px solid #3a3f4b;border-radius:4px;")
        self.rb_showing = QLabel("—")
        fl.addRow("Name", self.rb_name)
        fl.addRow("Fun fact", self.rb_fact)
        fl.addRow("Table / ID", self.rb_attendee)
        fl.addRow("LED color", self.rb_swatch)
        fl.addRow("Currently showing", self.rb_showing)
        v.addWidget(g)

        g = QGroupBox("Text screens")
        gv = QVBoxLayout(g)
        self.rb_screens = QPlainTextEdit()
        self.rb_screens.setReadOnly(True)
        self.rb_screens.setFixedHeight(110)
        gv.addWidget(self.rb_screens)
        row = QHBoxLayout()
        self.rb_scr_idx = QSpinBox()
        self.rb_scr_idx.setRange(0, 19)
        view_btn = QPushButton("View body")
        view_btn.clicked.connect(lambda: self._go(self._read_screen_body()))
        row.addWidget(QLabel("Screen"))
        row.addWidget(self.rb_scr_idx)
        row.addWidget(view_btn)
        row.addStretch()
        gv.addLayout(row)
        self.rb_body = QPlainTextEdit()
        self.rb_body.setReadOnly(True)
        self.rb_body.setFixedHeight(90)
        gv.addWidget(self.rb_body)
        v.addWidget(g)

        g = QGroupBox("Image slots")
        grid = QGridLayout(g)
        self.rb_slot_imgs = []
        self.rb_slot_lbls = []
        for i in range(4):
            thumb = QLabel()
            thumb.setFixedSize(132, 88)
            thumb.setStyleSheet("background:#11141a;border:1px solid #3a3f4b;border-radius:4px;")
            cap = QLabel(f"slot {i}: —")
            cap.setStyleSheet("color:#9aa4b2;")
            grid.addWidget(thumb, 0, i)
            grid.addWidget(cap, 1, i)
            self.rb_slot_imgs.append(thumb)
            self.rb_slot_lbls.append(cap)
        v.addWidget(g)
        v.addStretch()

        self._readback_actions = [read_btn, view_btn]
        return w

    async def _read_all(self) -> None:
        if not self.link.connected:
            self._log("Connect to a badge first.")
            return
        self._log("Reading config from badge…")
        snap = await self.link.read_config_snapshot()
        self.rb_name.setText(snap["name"] or "—")
        self.rb_fact.setText(snap["fun_fact"] or "—")
        self.rb_attendee.setText(snap["attendee"] or "—")
        c = snap["color"]
        if c["has"]:
            self.rb_swatch.setStyleSheet(
                f"background: rgb({c['r']},{c['g']},{c['b']});"
                "border:1px solid #3a3f4b;border-radius:4px;")
        else:
            self.rb_swatch.setStyleSheet("background:#2a2f3a;border:1px solid #3a3f4b;border-radius:4px;")
        d = snap["display"]
        if d["has"]:
            kind = "Text screen" if d["kind"] == keys.DISP_KIND_TEXT else "Image slot"
            self.rb_showing.setText(f"{kind} {d['idx']}")
        else:
            self.rb_showing.setText("—")

        lines = [
            f"{i:2d}: {s['title'] or '(no title)'}"
            for i, s in enumerate(snap["screens"]) if s["present"]
        ]
        self.rb_screens.setPlainText("\n".join(lines) if lines else "(no screens stored)")

        for i, slot in enumerate(snap["slots"]):
            if i >= len(self.rb_slot_imgs):
                break
            if not slot["present"]:
                self.rb_slot_imgs[i].clear()
                self.rb_slot_lbls[i].setText(f"slot {i}: empty")
                continue
            self.rb_slot_lbls[i].setText(f"slot {i}: reading…")
            img = await self.link.read_image(i)
            if img.get("present"):
                fmt = "bw" if img["fmt"] == keys.FMT_BW else "gray2"
                pil = imageconv.unpack_preview(img["pixels"], fmt)
                pm = pil_to_pixmap(pil).scaled(
                    self.rb_slot_imgs[i].size(),
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation)
                self.rb_slot_imgs[i].setPixmap(pm)
                self.rb_slot_lbls[i].setText(f"slot {i}: {fmt} {img['w']}x{img['h']}")
            else:
                self.rb_slot_imgs[i].clear()
                self.rb_slot_lbls[i].setText(f"slot {i}: empty")
        self._log("Config read complete.")

    async def _read_screen_body(self) -> None:
        if not self.link.connected:
            self._log("Connect to a badge first.")
            return
        idx = self.rb_scr_idx.value()
        s = await self.link.read_screen(idx)
        if not s["present"]:
            self.rb_body.setPlainText(f"(screen {idx} is empty)")
            return
        self.rb_body.setPlainText(f"[{s['header']}]\n\n{s['body']}")
        self._log(f"Read screen {idx}.")

    @property
    def _action_widgets(self):
        return (getattr(self, "_gatt_actions", [])
                + getattr(self, "_mesh_actions", [])
                + getattr(self, "_readback_actions", []))

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
            self._log("Auto event-heartbeat ON (every 60 s).")
        else:
            self._hb_timer.stop()
            self._log("Auto event-heartbeat OFF.")

    # mesh helpers
    def _session(self) -> MeshSession:
        src = int(self.m_src.text(), 16)
        if self._seqstore is None or self._seqstore.src != src:
            self._seqstore = SeqStore(src)
        return MeshSession(self.crypto, src, self._seqstore)

    def _dst(self) -> int:
        d = self.m_target.currentData()
        return int(self.m_unicast.text(), 16) if d == -1 else d

    async def _mesh_send(self, op: int, params: bytes) -> None:
        if not self.link.connected:
            self._log("Not connected (mesh needs a connected proxy badge).")
            return
        sess = self._session()
        dst = self._dst()
        access = keys.vendor_opcode(op) + params
        writes = sess.build(dst, access, self.link.mtu)
        for pdu in writes:
            await self.link.proxy_write(pdu)
        self.m_seq.setText(f"seq {self._seqstore.peek()}")
        self._log(f"mesh op 0x{op:02x} → 0x{dst:04x} ({len(writes)} proxy PDU(s), {len(params)}B params)")

    async def _send_heartbeat(self) -> None:
        await self._mesh_send(keys.OP_HEARTBEAT, b"")

    async def _mesh_set_screen(self) -> None:
        # Mirror the firmware's app-chunked screen push over mesh.
        idx = self.m_scr_idx.value()
        await self._mesh_send(keys.OP_SCREEN_HDR, bytes([idx]) + self.m_scr_hdr.text().encode("utf-8")[:47])
        body = self.m_scr_body.toPlainText().encode("utf-8")
        chunk = 96
        segs = [body[i : i + chunk] for i in range(0, len(body), chunk)] or [b""]
        for seq, seg in enumerate(segs):
            last = 1 if seq == len(segs) - 1 else 0
            await self._mesh_send(keys.OP_SCREEN_BODY, bytes([idx, seq, last]) + seg)
        self._log(f"mesh text screen {idx} sent in {len(segs)} body chunk(s).")
