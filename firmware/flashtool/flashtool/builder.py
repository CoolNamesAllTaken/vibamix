"""west build wrapper.

Runs ``west build`` inside the NCS toolchain environment without requiring the
user to activate any shell scripts first.  The toolchain environment is
reconstructed from the ``environment.json`` file that ships with every NCS
toolchain bundle.
"""

from __future__ import annotations

import json
import os
import subprocess
from collections.abc import Callable
from pathlib import Path

from . import config


def firmware_hex(build_dir: Path = config.BUILD_DIR) -> Path:
    """Return the expected path of the output hex for a given build directory."""
    return build_dir / "vibamix_zephyr" / "zephyr" / "zephyr.hex"


def build(
    build_dir: Path = config.BUILD_DIR,
    on_output: Callable[[str], None] = print,
) -> bool:
    """Run ``west build`` and stream output through *on_output*.

    Returns ``True`` if the build succeeded (exit code 0).
    """
    cmd = [
        "west", "build",
        "-b", config.BOARD,
        "--build-dir", str(build_dir),
        str(config.WEST_APP_DIR),
        "--",
        f"-DBOARD_ROOT={config.WEST_APP_DIR}",
    ]
    env = _toolchain_env()
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(config.NCS_DIR),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except FileNotFoundError:
        on_output("ERROR: 'west' not found — check NCS_DIR in config.py\n")
        return False

    for line in _read_lines(proc.stdout):
        on_output(line)

    return proc.wait() == 0


def _read_lines(stream):
    """Yield lines split on both \\n and \\r (handles build tool progress output)."""
    buf = ""
    while True:
        ch = stream.read(1)
        if not ch:
            if buf:
                yield buf
            break
        if ch in ("\n", "\r"):
            if buf.strip():
                yield buf
            buf = ""
        else:
            buf += ch


def _toolchain_env() -> dict[str, str]:
    """Return an environment dict with the NCS toolchain prepended to PATH etc.

    Reads relative path entries from ``environment.json`` in the toolchain
    bundle directory and resolves them against the bundle root.  Also injects
    ``ZEPHYR_BASE`` which is required by west but absent from environment.json.
    """
    tc = config.NCS_TOOLCHAIN_DIR
    env = os.environ.copy()

    env_json = tc / "environment.json"
    if env_json.exists():
        data = json.loads(env_json.read_text())
        for var in data.get("env_vars", []):
            key = var["key"]
            vtype = var.get("type", "value")
            if vtype == "relative_paths":
                resolved = ":".join(str(tc / p) for p in var.get("values", []))
                treatment = var.get("existing_value_treatment", "prepend_to")
                if treatment == "prepend_to":
                    existing = env.get(key, "")
                    env[key] = (resolved + ":" + existing) if existing else resolved
                else:
                    env[key] = resolved
            else:
                # plain value (may be a single string or a list)
                val = var.get("value") or var.get("values")
                if isinstance(val, list):
                    env[key] = ":".join(str(tc / v) for v in val)
                elif val is not None:
                    env[key] = str(val)

    # ZEPHYR_BASE is required by west but not present in environment.json.
    env["ZEPHYR_BASE"] = str(config.NCS_DIR / "zephyr")
    return env
