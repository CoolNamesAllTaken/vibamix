"""Unit tests for the probe-rs flash retry wrapper (no probe-rs / USB).

``_flash_once`` is monkeypatched so these run without hardware; we only exercise
the retry/backoff classification in ``flash_device`` and ``_is_retriable``.
"""

from __future__ import annotations

import pytest

from badgectl import flash

# A dummy image list so flash_device's "no images" guard never trips (the real
# guard lives in _flash_once, which we replace here anyway).
_IMAGES = [flash.Image("dummy.bin", "bin", "app", 0xE000)]


def _noop(pct: float, phase: str) -> None:
    pass


@pytest.fixture(autouse=True)
def _no_sleep(monkeypatch: pytest.MonkeyPatch) -> None:
    """Don't actually back off between retries."""
    monkeypatch.setattr(flash.time, "sleep", lambda *_: None)


def _stub_once(monkeypatch: pytest.MonkeyPatch, results):
    """Replace _flash_once with one that returns ``results`` in order.

    Returns a single-element list holding the call count.
    """
    seq = iter(results)
    calls = [0]

    def fake(selector, images, on_progress, factory_id, erase, attempt, max_attempts):
        calls[0] += 1
        return next(seq)

    monkeypatch.setattr(flash, "_flash_once", fake)
    return calls


def test_retries_transient_then_succeeds(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(False, "app: probe-rs exited 1"), (True, "ok")])
    ok, msg = flash.flash_device("sel", _IMAGES, _noop, max_attempts=3)
    assert (ok, msg) == (True, "ok")
    assert calls[0] == 2


def test_gives_up_after_max_attempts(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(False, "app: probe-rs exited 1")] * 3)
    ok, msg = flash.flash_device("sel", _IMAGES, _noop, max_attempts=3)
    assert ok is False
    assert calls[0] == 3
    assert "3/3 attempts" in msg


def test_timeout_is_retried(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(False, "app: timeout"), (True, "ok")])
    ok, _ = flash.flash_device("sel", _IMAGES, _noop, max_attempts=2)
    assert ok is True
    assert calls[0] == 2


def test_probe_rs_not_found_not_retried(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(False, "probe-rs not found — install via ...")])
    ok, _ = flash.flash_device("sel", _IMAGES, _noop, max_attempts=3)
    assert ok is False
    assert calls[0] == 1


def test_factory_id_out_of_range_not_retried(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(False, "factory id 99999 out of range")])
    ok, _ = flash.flash_device("sel", _IMAGES, _noop, max_attempts=3)
    assert ok is False
    assert calls[0] == 1


def test_success_first_try(monkeypatch: pytest.MonkeyPatch) -> None:
    calls = _stub_once(monkeypatch, [(True, "ok")])
    ok, msg = flash.flash_device("sel", _IMAGES, _noop, max_attempts=3)
    assert (ok, msg) == (True, "ok")
    assert calls[0] == 1


@pytest.mark.parametrize(
    "msg, retriable",
    [
        ("app: probe-rs exited 1", True),
        ("erase: probe-rs exited 134", True),
        ("factory id: probe-rs exited 1", True),
        ("app: timeout", True),
        ("erase: timeout", True),
        ("probe-rs not found — install via 'cargo install probe-rs-tools'", False),
        ("factory id 99999 out of range", False),
        ("no firmware images to flash", False),
    ],
)
def test_is_retriable(msg: str, retriable: bool) -> None:
    assert flash._is_retriable(msg) is retriable
