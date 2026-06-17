"""PyQt6 main window: Direct-GATT and Mesh control panels."""

from __future__ import annotations

import asyncio
import datetime
import traceback

from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QColor, QImage, QPixmap
from PyQt6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QSplitter,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from bleak.exc import BleakDeviceNotFoundError

from . import imageconv, keys
from .ble import BadgeLink, scan
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
"""


def pil_to_pixmap(pil_img) -> QPixmap:
    img = pil_img.convert("L")
    w, h = img.size
    qimg = QImage(img.tobytes(), w, h, w, QImage.Format.Format_Grayscale8)
    return QPixmap.fromImage(qimg.copy())


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("badgectl — vibamix GATT + Mesh")
        self.resize(900, 760)
        self.setStyleSheet(STYLE)

        self.link = BadgeLink()
        self.crypto = MeshCrypto(keys.NET_KEY, keys.APP_KEY, keys.IV_INDEX)
        self._seqstore: SeqStore | None = None
        self._led = (255, 80, 0)
        self._gatt_img: bytes | None = None
        self._slot_img: bytes | None = None

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.addLayout(self._build_topbar())

        tabs = QTabWidget()
        tabs.addTab(self._build_gatt_tab(), "Direct (GATT)")
        tabs.addTab(self._build_mesh_tab(), "Mesh")

        split = QSplitter(Qt.Orientation.Vertical)
        split.addWidget(tabs)
        self.log = QPlainTextEdit()
        self.log.setObjectName("log")
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        split.addWidget(self.log)
        split.setStretchFactor(0, 4)
        split.setStretchFactor(1, 1)
        root.addWidget(split)

        self._hb_timer = QTimer(self)
        self._hb_timer.setInterval(60_000)
        self._hb_timer.timeout.connect(lambda: self._go(self._send_heartbeat()))

        self._set_connected(False)
        self._log("Ready. Wake a badge (press its button), then Scan + Connect.")

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
        self.scan_btn = QPushButton("Scan")
        self.scan_btn.setObjectName("ghost")
        self.scan_btn.clicked.connect(lambda: self._go(self._scan()))
        self.dev_combo = QComboBox()
        self.dev_combo.setMinimumWidth(220)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(lambda: self._go(self._connect()))
        self.disconnect_btn = QPushButton("Disconnect")
        self.disconnect_btn.setObjectName("ghost")
        self.disconnect_btn.clicked.connect(lambda: self._go(self._disconnect()))
        self.status = QLabel("● disconnected")
        self.status.setObjectName("status")
        self.status.setStyleSheet("color:#e06c5b;")
        self.mtu_lbl = QLabel("MTU —")

        bar.addWidget(self.scan_btn)
        bar.addWidget(self.dev_combo, 1)
        bar.addWidget(self.connect_btn)
        bar.addWidget(self.disconnect_btn)
        bar.addStretch()
        bar.addWidget(self.mtu_lbl)
        bar.addWidget(self.status)
        return bar

    def _set_connected(self, on: bool) -> None:
        self.status.setText("● connected" if on else "● disconnected")
        self.status.setStyleSheet("color:#5bd07a;" if on else "color:#e06c5b;")
        self.connect_btn.setEnabled(not on)
        self.disconnect_btn.setEnabled(on)
        for w in self._action_widgets:
            w.setEnabled(on)
        if on:
            self.mtu_lbl.setText(f"MTU {self.link.mtu}")
        else:
            self.mtu_lbl.setText("MTU —")
            self._hb_timer.stop()

    async def _scan(self) -> None:
        self._log("Scanning …")
        self.dev_combo.clear()
        found = await scan()
        if not found:
            self._log("No vibamix badges found. Is one awake (button pressed)?")
            return
        for f in found:
            tag = " [proxy]" if f.is_proxy else ""
            self.dev_combo.addItem(f"{f.name}{tag}  —  {f.address}", f)
        self._log(f"Found {len(found)} device(s).")

    async def _connect(self) -> None:
        found = self.dev_combo.currentData()
        if found is None:
            self._log("Pick a device first (Scan).")
            return
        self._log(f"Connecting to {found.address} …")
        try:
            await self.link.connect(found)
        except BleakDeviceNotFoundError:
            self._log(
                "Badge not found — press its button to wake it, then Scan again. "
                "(It sleeps ~3 min after the last activity / ~5 s after a cold boot.)"
            )
            return
        self._set_connected(True)
        self._log(f"Connected. ATT MTU = {self.link.mtu}.")

    async def _disconnect(self) -> None:
        await self.link.disconnect()
        self._set_connected(False)
        self._log("Disconnected.")

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
        self.g_ota_path.setPlaceholderText("build dir containing slotA.bin / slotB.bin")
        obrowse = QPushButton("Browse…")
        obrowse.setObjectName("ghost")
        obrowse.clicked.connect(self._pick_ota)
        self.g_ota_prog = QProgressBar()
        oup = QPushButton("Upload + reboot")
        oup.clicked.connect(lambda: self._go(self._upload_ota()))
        warn = QLabel("Reads which slot is inactive and sends that slot's image. "
                      "Badge verifies, reboots into it (~5 s), then auto-reverts if it fails.")
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
        d = QFileDialog.getExistingDirectory(
            self, "Pick build dir (contains slotA.bin / slotB.bin)"
        )
        if d:
            self.g_ota_path.setText(d)

    async def _upload_ota(self) -> None:
        build_dir = self.g_ota_path.text()
        if not build_dir:
            self._log("Pick the build dir containing slotA.bin / slotB.bin first.")
            return
        active, inactive, ver = await self.link.read_ota_status()
        self._log(f"OTA: active slot {active} (v{ver}); sending image for inactive slot {inactive} …")
        self.g_ota_prog.setValue(0)
        slot = await self.link.ota_update_auto(
            build_dir, on_progress=lambda d, n: self.g_ota_prog.setValue(int(d * 100 / n)))
        self._log(f"OTA: slot {slot} image sent. Badge verifies + reboots (~5 s); "
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

        g = QGroupBox("Heartbeat (keep awake)")
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

    @property
    def _action_widgets(self):
        return getattr(self, "_gatt_actions", []) + getattr(self, "_mesh_actions", [])

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
            self._log("Auto-heartbeat ON (every 60 s).")
        else:
            self._hb_timer.stop()
            self._log("Auto-heartbeat OFF.")

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
            self._log("Not connected (mesh needs a proxy badge).")
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
