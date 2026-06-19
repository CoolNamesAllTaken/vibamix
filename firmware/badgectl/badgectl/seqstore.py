"""Persisted mesh sequence number (per source address).

The badges enforce replay protection on (src, seq), so the laptop must never
reuse a sequence number — persist it across runs and always advance.
"""

from __future__ import annotations

import json
import threading
from pathlib import Path

SEQ_MAX = 0xFFFFFF  # 24-bit


class SeqStore:
    def __init__(self, src: int, path: Path | None = None) -> None:
        self.src = src
        self.path = path or (Path.home() / ".badgectl" / "seq.json")
        self._lock = threading.Lock()
        self._seq = self._load()

    def _read_all(self) -> dict:
        try:
            return json.loads(self.path.read_text())
        except Exception:
            return {}

    def _load(self) -> int:
        return int(self._read_all().get(str(self.src), 0)) & SEQ_MAX

    def _save(self) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            data = self._read_all()
            data[str(self.src)] = self._seq
            self.path.write_text(json.dumps(data, indent=2))
        except Exception:
            pass  # best-effort; a failed save just risks a replayed seq next run

    def peek(self) -> int:
        with self._lock:
            return self._seq

    def advance(self, n: int = 1) -> int:
        with self._lock:
            self._seq = (self._seq + n) & SEQ_MAX
            self._save()
            return self._seq
