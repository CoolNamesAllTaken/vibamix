"""Unit tests for the multi-image flash flow — no hardware required.

probe-rs is stubbed out via monkeypatch; these tests cover that two images are
flashed in order, progress is scaled into per-image bands, and the target is
reset exactly once after the last image.
"""

from __future__ import annotations

import pytest

from flashtool import config, flasher
from flashtool.models import UsbDevice


class _FakeProc:
    """Minimal stand-in for subprocess.Popen yielding canned probe-rs lines."""

    def __init__(self, lines: list[str]):
        self.stdout = iter(lines)
        self._rc = 0

    def wait(self, timeout=None):
        return self._rc

    def kill(self):  # pragma: no cover - timeout path not exercised here
        pass


_PROBE_LINES = [
    "     Erasing ✓ [00:00:01] [####] 131072/131072 @ 107 KiB/s\n",
    "  Programming ✓ [00:00:02] [####] 131072/131072 @ 36 KiB/s\n",
    "    Verifying ✓ [00:00:00] [####] 131072/131072 @ 512 KiB/s\n",
    "      Finished in 3.45s\n",
]


@pytest.fixture
def dev() -> UsbDevice:
    return UsbDevice("1-4.7.1", 0x2886, 0x0066, "SER001")


def _patch_probe(monkeypatch, resets: list, *, rc: int = 0):
    """Stub Popen (download) to emit probe lines and subprocess.run (reset)."""
    def fake_popen(cmd, **kw):
        proc = _FakeProc(list(_PROBE_LINES))
        proc._rc = rc
        return proc

    def fake_run(cmd, **kw):
        resets.append(cmd)

    monkeypatch.setattr(flasher.subprocess, "Popen", fake_popen)
    monkeypatch.setattr(flasher.subprocess, "run", fake_run)
    # _read_lines reads char-by-char from a real stream; feed it our lines via a
    # tiny shim that joins them so the existing splitter still works.
    monkeypatch.setattr(flasher, "_read_lines", lambda stream: list(stream))


def test_two_images_scale_into_bands(monkeypatch, dev):
    """Image 0 progress lands in [0,50), image 1 in [50,100]; labels are prefixed."""
    resets: list = []
    _patch_probe(monkeypatch, resets)
    updates: list[tuple[float, str]] = []

    images = [str(config.BOOTLOADER_HEX), str(config.APP_HEX)]
    ok, msg = flasher.flash_device(dev, images, lambda p, m: updates.append((p, m)))

    assert ok and msg == "ok"
    bl = [(p, m) for p, m in updates if m.startswith("bootloader:")]
    app = [(p, m) for p, m in updates if m.startswith("app:")]
    assert bl and app
    assert all(0.0 <= p < 50.0 for p, _ in bl)
    assert all(50.0 <= p <= 100.0 for p, _ in app)
    assert updates[-1] == (100.0, "done")


def test_reset_runs_once_after_last_image(monkeypatch, dev):
    resets: list = []
    _patch_probe(monkeypatch, resets)
    images = [str(config.BOOTLOADER_HEX), str(config.APP_HEX)]
    flasher.flash_device(dev, images, lambda p, m: None)
    assert len(resets) == 1


def test_failure_aborts_and_skips_reset(monkeypatch, dev):
    resets: list = []
    _patch_probe(monkeypatch, resets, rc=1)
    images = [str(config.BOOTLOADER_HEX), str(config.APP_HEX)]
    ok, msg = flasher.flash_device(dev, images, lambda p, m: None)
    assert not ok
    assert msg.startswith("bootloader:")  # failed on the first image
    assert resets == []                    # no reset when a flash failed


def test_single_string_still_supported(monkeypatch, dev):
    resets: list = []
    _patch_probe(monkeypatch, resets)
    ok, _ = flasher.flash_device(dev, str(config.APP_HEX), lambda p, m: None)
    assert ok
    assert len(resets) == 1
