"""Entry point: start the Qt app with a qasync event loop (so bleak coroutines
run on the Qt thread — no separate worker thread needed)."""

from __future__ import annotations

import asyncio
import sys

import qasync
from PyQt6.QtWidgets import QApplication

from .gui import MainWindow


def main() -> None:
    app = QApplication(sys.argv)
    loop = qasync.QEventLoop(app)
    asyncio.set_event_loop(loop)

    window = MainWindow()
    window.show()

    with loop:
        loop.run_forever()


if __name__ == "__main__":
    main()
