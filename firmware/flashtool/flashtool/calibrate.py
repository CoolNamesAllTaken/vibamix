"""Calibration helper — builds the port map for a specific USB hub model.

Run ``python -m flashtool.calibrate`` (or click Discover in the GUI).
Calibrate one hub instance; the resulting *relative* paths work for every
instance of the same hub model regardless of which laptop port it is on.

The config is saved to ``flashtool/usb_hubs/bench_<name>.json`` (gitignored).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from . import usb_topology
from .config import MAX_SLOTS, _CONFIG_DIR


def write_bench_config(relative_map: dict[int, str], dest: Path) -> None:
    """Write relative_map to a bench config file."""
    data = {"relative_map": {str(k): v for k, v in relative_map.items()}}
    dest.write_text(json.dumps(data, indent=2) + "\n")


def run_slot_calibration(
    prompt_fn=input,
    num_slots: int = MAX_SLOTS,
) -> dict[int, str]:
    """Map each physical slot to an absolute USB path by plugging one board at a time.

    Returns ``{slot_index: absolute_usb_path}``.  Call ``abs_to_relative`` to
    strip the hub-root prefix before saving.
    """
    slot_map: dict[int, str] = {}
    before = usb_topology.snapshot_all_port_paths()

    for i in range(num_slots):
        while True:
            prompt_fn(
                f"Slot {i + 1} of {num_slots}: plug a board into physical "
                f"slot {i + 1}, then press Enter / click OK…"
            )
            after = usb_topology.snapshot_all_port_paths()
            probe_paths = {p.port_path for p in usb_topology.enumerate_probes()}
            new_boards = usb_topology.diff_hub_ports(before, after) & probe_paths

            if len(new_boards) == 1:
                path = next(iter(new_boards))
                slot_map[i] = path
                print(f"  slot {i:>2}  ->  {path}")
                before = after
                break
            elif len(new_boards) == 0:
                print("  No new board detected — check the connection and try again.")
            else:
                print(
                    f"  {len(new_boards)} new boards detected at once; "
                    "unplug the extras and try again."
                )

    return slot_map


def abs_to_relative(slot_map: dict[int, str]) -> dict[int, str]:
    """Strip the shared hub-root prefix from all paths in slot_map.

    ``{"0": "1-4.7.1", "1": "1-4.7.2"}`` → ``{"0": "7.1", "1": "7.2"}``
    """
    if not slot_map:
        return {}
    sample = next(iter(slot_map.values()))
    root = usb_topology.hub_root(sample)
    return {i: usb_topology.strip_hub_root(p) for i, p in slot_map.items()
            if p.startswith(root + ".")}


def _prompt_positive_int(prompt: str) -> int:
    while True:
        raw = input(prompt).strip()
        if raw.isdigit() and int(raw) > 0:
            return int(raw)
        print("  Please enter a positive integer.")


def _format_map(relative_map: dict[int, str]) -> str:
    lines = [f"  slot {i:>2}  ->  {path}" for i, path in sorted(relative_map.items())]
    return "\n".join(lines)


def main() -> None:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Calibrate flashtool port map for a USB hub model.")
    parser.add_argument("num_slots", type=int, nargs="?",
                        help="Number of ports per hub")
    args = parser.parse_args()

    if args.num_slots is None:
        args.num_slots = _prompt_positive_int("How many ports does your hub have? ")

    print(f"\nflashTool hub calibration ({args.num_slots} slots)\n")

    slot_map = run_slot_calibration(num_slots=args.num_slots)
    if not slot_map:
        return

    relative_map = abs_to_relative(slot_map)
    print(f"\nRelative map ({len(relative_map)} slots):\n{_format_map(relative_map)}\n")

    name = input("Save as (e.g. my_hub): ").strip()
    if name:
        dest = _CONFIG_DIR / f"bench_{name}.json"
        write_bench_config(relative_map, dest)
        print(f"Written to {dest}")
    else:
        print("Skipped — rerun calibrate when ready.")


if __name__ == "__main__":
    main()
