"""Unit tests for the XLSX bulk-programming parser (no Qt / BLE)."""

from __future__ import annotations

from pathlib import Path

import openpyxl
import pytest
from PIL import Image

from badgectl import bulkprog, imageconv, keys


def _png(path: Path) -> None:
    Image.new("L", (264, 176), 128).save(path)


def _write_sheet(path: Path, headers, rows) -> None:
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.append(headers)
    for r in rows:
        ws.append(r)
    wb.save(str(path))


def test_parse_full_row(tmp_path: Path) -> None:
    _png(tmp_path / "id.png")
    _png(tmp_path / "slot1.png")
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(
        xlsx,
        ["name", "attendee_id", "identity_image", "identity_image_fmt",
         "text_label_1", "text_body_1", "text_label_2", "text_body_2",
         "image_1", "image_1_fmt"],
        [["Ada", "T12", "id.png", "bw",
          "About", "First programmer", "Ask me", "Engines",
          "slot1.png", "gray2"]],
    )

    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert warnings == []
    assert len(rows) == 1
    row = rows[0]
    assert row.name == "Ada"
    assert row.attendee_id == "T12"
    assert row.identity_image is not None
    assert row.identity_image[1] == keys.FMT_BW  # per-column override honored

    assert [(t.idx, t.label, t.body) for t in row.text_frames] == [
        (0, "About", "First programmer"),
        (1, "Ask me", "Engines"),
    ]
    assert len(row.image_frames) == 1
    img = row.image_frames[0]
    assert img.slot == 0
    assert img.fmt == keys.FMT_GRAY2


def test_missing_image_raises(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "image_1"], [["Bob", "nope.png"]])
    with pytest.raises(ValueError) as ei:
        bulkprog.parse_workbook(xlsx)
    msg = str(ei.value)
    assert "not found" in msg and "nope.png" in msg


def test_corrupt_image_raises(tmp_path: Path) -> None:
    bad = tmp_path / "bad.png"
    bad.write_bytes(b"this is not a real image")
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "image_1"], [["Cy", "bad.png"]])
    with pytest.raises(ValueError) as ei:
        bulkprog.parse_workbook(xlsx)
    assert "bad.png" in str(ei.value)


def test_missing_identity_image_raises(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "identity_image"], [["Bob", "gone.png"]])
    with pytest.raises(ValueError) as ei:
        bulkprog.parse_workbook(xlsx)
    assert "identity_image" in str(ei.value) and "gone.png" in str(ei.value)


def test_column_order_and_blank_rows(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    # headers reordered; second data row entirely blank -> skipped
    _write_sheet(
        xlsx,
        ["text_body_3", "name", "text_label_3", "bogus_col"],
        [["body3", "Carol", "label3", "x"],
         [None, None, None, None]],
    )
    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert len(rows) == 1                        # blank row skipped
    assert rows[0].name == "Carol"
    assert [(t.idx, t.label, t.body) for t in rows[0].text_frames] == [(2, "label3", "body3")]
    assert any("bogus_col" in w for w in warnings)


def test_default_fmt_is_gray2(tmp_path: Path) -> None:
    _png(tmp_path / "p.png")
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["image_1"], [["p.png"]])
    rows, _ = bulkprog.parse_workbook(xlsx)
    assert rows[0].image_frames[0].fmt == keys.FMT_GRAY2


def test_attendee_alias(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "table_id"], [["Dee", "42"]])
    rows, _ = bulkprog.parse_workbook(xlsx)
    assert rows[0].attendee_id == "42"


def test_imageconv_byte_lengths(tmp_path: Path) -> None:
    p = tmp_path / "p.png"
    _png(p)
    assert len(imageconv.to_gray2(str(p))) == keys.IMG_GRAY2_BYTES
    assert len(imageconv.to_bw(str(p))) == keys.IMG_BW_BYTES


def test_text_led_named_color_and_anim(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(
        xlsx,
        ["name", "text_label_1", "text_body_1", "text_led_1", "text_anim_1"],
        [["Eve", "Role", "Hacker", "orange", "wheel"]],
    )
    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert warnings == []
    tf = rows[0].text_frames[0]
    assert tf.anim == keys.ANIM_WHEEL
    assert (tf.r, tf.g, tf.b) == (255, 165, 0)


def test_image_led_hex_default_solid(tmp_path: Path) -> None:
    _png(tmp_path / "p.png")
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(
        xlsx,
        ["image_1", "image_led_1"],
        [["p.png", "#00ff00"]],   # color but no anim -> Solid
    )
    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert warnings == []
    im = rows[0].image_frames[0]
    assert im.anim == keys.ANIM_SOLID
    assert (im.r, im.g, im.b) == (0, 255, 0)


def test_led_three_digit_hex(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "text_label_1", "text_led_1"],
                 [["Fae", "X", "#0f0"]])
    rows, _ = bulkprog.parse_workbook(xlsx)
    tf = rows[0].text_frames[0]
    assert (tf.r, tf.g, tf.b) == (0, 255, 0)


def test_led_neither_defaults_off(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "text_label_1", "text_body_1"],
                 [["Gus", "X", "Y"]])
    rows, _ = bulkprog.parse_workbook(xlsx)
    tf = rows[0].text_frames[0]
    assert (tf.anim, tf.r, tf.g, tf.b) == (keys.ANIM_OFF, 0, 0, 0)


def test_anim_only_solid_no_color_warns(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "text_label_1", "text_anim_1"],
                 [["Hal", "X", "solid"]])
    rows, warnings = bulkprog.parse_workbook(xlsx)
    tf = rows[0].text_frames[0]
    assert tf.anim == keys.ANIM_SOLID and (tf.r, tf.g, tf.b) == (0, 0, 0)
    assert any("solid animation with no color" in w for w in warnings)


def test_unknown_color_and_anim_warn(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "text_label_1", "text_led_1", "text_anim_1"],
                 [["Ivy", "X", "chartreusey", "disco"]])
    rows, warnings = bulkprog.parse_workbook(xlsx)
    tf = rows[0].text_frames[0]
    # both invalid -> treated as absent -> OFF default
    assert (tf.anim, tf.r, tf.g, tf.b) == (keys.ANIM_OFF, 0, 0, 0)
    assert any("unrecognized color" in w for w in warnings)
    assert any("unknown animation" in w for w in warnings)


def test_identity_led_with_name(tmp_path: Path) -> None:
    xlsx = tmp_path / "sheet.xlsx"
    _write_sheet(xlsx, ["name", "identity_led", "identity_anim"],
                 [["Jo", "red", "solid"]])
    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert warnings == []
    assert rows[0].identity_led == (keys.ANIM_SOLID, 255, 0, 0)


def test_identity_led_without_name_warns(tmp_path: Path) -> None:
    _png(tmp_path / "p.png")
    xlsx = tmp_path / "sheet.xlsx"
    # identity LED on a row with only an image (no name/attendee) -> ignored + warn
    _write_sheet(xlsx, ["image_1", "identity_led"], [["p.png", "red"]])
    rows, warnings = bulkprog.parse_workbook(xlsx)
    assert rows[0].identity_led is None
    assert any("identity LED ignored" in w for w in warnings)


def test_make_template_roundtrips(tmp_path: Path) -> None:
    out = tmp_path / "tmpl.xlsx"
    bulkprog.make_template(out)
    assert out.is_file()
    rows, _ = bulkprog.parse_workbook(out)
    # template's example row references images that don't exist here, but name parses
    assert rows and rows[0].name == "Ada Lovelace"
