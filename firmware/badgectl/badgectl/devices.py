"""Qt table model for the scanned-device list (scales to hundreds; live updates)."""

from __future__ import annotations

import time
from dataclasses import dataclass

from PyQt6.QtCore import QAbstractTableModel, QModelIndex, Qt

COL_CHECK, COL_NAME, COL_ADDR, COL_RSSI, COL_SEEN, COL_STATUS, COL_KA = range(7)
HEADERS = ["", "Name", "Address", "RSSI", "Seen", "Status", "KA"]


@dataclass
class Device:
    address: str
    name: str = ""
    rssi: int = 0
    last_seen: float = 0.0
    is_proxy: bool = False
    device: object = None          # live BLEDevice handle from the scan
    checked: bool = False
    status: str = "idle"           # idle/queued/connecting/working/done/failed
    ka_age: float | None = None    # secs since last badge->app keepalive (active conn only)


class DeviceModel(QAbstractTableModel):
    def __init__(self) -> None:
        super().__init__()
        self._rows: list[Device] = []
        self._index: dict[str, int] = {}

    # --- Qt model API ---
    def rowCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._rows)

    def columnCount(self, parent=QModelIndex()) -> int:
        return len(HEADERS)

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return HEADERS[section]
        return None

    def flags(self, index):
        if not index.isValid():
            return Qt.ItemFlag.NoItemFlags
        f = Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable
        if index.column() == COL_CHECK:
            f |= Qt.ItemFlag.ItemIsUserCheckable
        return f

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid():
            return None
        d = self._rows[index.row()]
        col = index.column()
        if role == Qt.ItemDataRole.CheckStateRole and col == COL_CHECK:
            return Qt.CheckState.Checked if d.checked else Qt.CheckState.Unchecked
        if role == Qt.ItemDataRole.DisplayRole:
            if col == COL_NAME:
                return d.name + (" [proxy]" if d.is_proxy and not d.name.startswith("vibamix-") else "")
            if col == COL_ADDR:
                return d.address
            if col == COL_RSSI:
                return str(d.rssi) if d.rssi else ""
            if col == COL_SEEN:
                return f"{int(time.monotonic() - d.last_seen)}s" if d.last_seen else ""
            if col == COL_STATUS:
                return d.status
            if col == COL_KA:
                if d.ka_age is None:
                    return ""
                return "●" if d.ka_age < 3 else "○"
        return None

    def setData(self, index, value, role=Qt.ItemDataRole.EditRole) -> bool:
        if role == Qt.ItemDataRole.CheckStateRole and index.column() == COL_CHECK:
            self._rows[index.row()].checked = int(value) == Qt.CheckState.Checked.value
            self.dataChanged.emit(index, index, [role])
            return True
        return False

    # --- app API ---
    def upsert(self, address, name, rssi, is_proxy, device) -> None:
        i = self._index.get(address)
        if i is None:
            row = len(self._rows)
            self.beginInsertRows(QModelIndex(), row, row)
            self._index[address] = row
            self._rows.append(Device(address=address, name=name, rssi=rssi,
                                     last_seen=time.monotonic(), is_proxy=is_proxy,
                                     device=device))
            self.endInsertRows()
        else:
            d = self._rows[i]
            if name:
                d.name = name
            d.rssi = rssi
            d.last_seen = time.monotonic()
            if device is not None:
                d.device = device
            self.dataChanged.emit(self.index(i, 0), self.index(i, len(HEADERS) - 1))

    def remove(self, address: str) -> None:
        """Drop a discovered badge from the list (reindexes the shifted rows)."""
        i = self._index.get(address)
        if i is None:
            return
        self.beginRemoveRows(QModelIndex(), i, i)
        del self._rows[i]
        del self._index[address]
        for j in range(i, len(self._rows)):
            self._index[self._rows[j].address] = j
        self.endRemoveRows()

    def prune_stale(self, max_age: float, protect: set[str] | None = None) -> int:
        """Remove devices not seen within `max_age` seconds. `protect` addresses are kept
        regardless (e.g. the active connection, which stops advertising while connected)."""
        protect = protect or set()
        now = time.monotonic()
        stale = [d.address for d in self._rows
                 if d.address not in protect
                 and d.last_seen
                 and now - d.last_seen > max_age]
        for addr in stale:
            self.remove(addr)   # reuses existing reindex + beginRemoveRows logic
        return len(stale)

    def device_at(self, row: int) -> Device:
        return self._rows[row]

    def checked_devices(self) -> list[Device]:
        return [d for d in self._rows if d.checked]

    def _emit_cell(self, address: str, col: int) -> None:
        i = self._index.get(address)
        if i is not None:
            self.dataChanged.emit(self.index(i, col), self.index(i, col))

    def set_status(self, address: str, status: str) -> None:
        i = self._index.get(address)
        if i is not None:
            self._rows[i].status = status
            self._emit_cell(address, COL_STATUS)

    def set_ka(self, address: str, age: float | None) -> None:
        i = self._index.get(address)
        if i is not None:
            self._rows[i].ka_age = age
            self._emit_cell(address, COL_KA)

    def refresh_ages(self) -> None:
        if self._rows:
            self.dataChanged.emit(self.index(0, COL_SEEN),
                                  self.index(len(self._rows) - 1, COL_SEEN))
