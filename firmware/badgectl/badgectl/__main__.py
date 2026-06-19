"""Entry point: start the Qt app with a qasync event loop (so bleak coroutines
run on the Qt thread — no separate worker thread needed)."""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import qasync
from PyQt6.QtGui import QIcon
from PyQt6.QtWidgets import QApplication

from .gui import MainWindow

ICON_PATH = Path(__file__).parent / "assets" / "icon.png"


def main() -> None:
    app = QApplication(sys.argv)
    app.setWindowIcon(QIcon(str(ICON_PATH)))
    loop = qasync.QEventLoop(app)
    asyncio.set_event_loop(loop)

    window = MainWindow()
    window.show()

    with loop:
        loop.run_forever()


if __name__ == "__main__":
    main()
