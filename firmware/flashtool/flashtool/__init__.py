"""flashtool — flash a pre-built firmware image onto every device on one USB hub.

See README.md for the design. Modules:
    config        bench setup (artifact dir, chip, port->slot map, limits)
    models        UsbDevice / Slot / Status data types
    usb_topology  enumerate the hub and bind physical ports to fixed slots
    flasher       probe-rs backend: flash a single device
    controller    discovery + concurrent dispatch (no GUI deps)
    gui           FreeSimpleGUI window + event loop
"""

__version__ = "0.0.1"
