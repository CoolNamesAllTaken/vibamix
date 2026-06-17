"""Badge fleet constants — mirror of the firmware.

Keep in sync with:
  - firmware/vibamix_zephyr/src/mesh_keys.h    (mesh keys / indices / group)
  - firmware/vibamix_zephyr/src/config_gatt.c  (GATT service + characteristic UUIDs)
  - firmware/vibamix_zephyr/src/mesh_model.c    (vendor opcodes, company id)
"""

# --- Bluetooth Mesh credentials (baked, shared across the fleet) ---
NET_KEY = b"VIBAMIXNETKEY001"   # 16 bytes
APP_KEY = b"VIBAMIXAPPKEY001"
DEV_KEY = b"VIBAMIXDEVKEY001"
assert len(NET_KEY) == len(APP_KEY) == len(DEV_KEY) == 16

NET_IDX = 0
APP_IDX = 0
IV_INDEX = 0
GROUP_ADDR = 0xC000           # "all badges" subscription group

# Company ID for the vendor model (CONFIG_BT_COMPANY_ID default = Nordic).
COMPANY_ID = 0x0059

# --- Vendor model opcodes (the 0xNN in BT_MESH_MODEL_OP_3(0xNN, CID)) ---
OP_SET_NAME = 0x01
OP_SET_FUN_FACT = 0x02
OP_SET_LED_COLOR = 0x03
OP_IMG_START = 0x04
OP_IMG_DATA = 0x05
OP_IMG_END = 0x06
OP_HEARTBEAT = 0x07
OP_SCREEN_HDR = 0x08
OP_SCREEN_BODY = 0x09
OP_DISPLAY = 0x0A


def vendor_opcode(op: int) -> bytes:
    """3-octet vendor opcode on the wire: 0xC0|op, then Company ID little-endian."""
    return bytes([0xC0 | (op & 0x3F), COMPANY_ID & 0xFF, (COMPANY_ID >> 8) & 0xFF])


# --- Custom config GATT service (f0de00xx-4b1c-4e2a-9a11-a1b2c3d4e5f6) ---
def _u(short: int) -> str:
    return f"f0de{short:04x}-4b1c-4e2a-9a11-a1b2c3d4e5f6"


UUID_CFG_SVC = _u(0x0001)
UUID_CHR_IMAGE = _u(0x0002)      # render-only 1bpp image
UUID_CHR_NAME = _u(0x0003)
UUID_CHR_SCREEN = _u(0x0004)     # store text screen
UUID_CHR_IMGSLOT = _u(0x0005)    # store image slot (BW or 2-bit gray)
UUID_CHR_DISPLAY = _u(0x0006)    # show stored screen
# (0x0007 attendee, 0x0008 per-frame LED — firmware-side; not used by badgectl yet)
UUID_CHR_OTA = _u(0x0009)        # firmware OTA update (trailered direct-XIP image)
UUID_CHR_OTA_STATUS = _u(0x000A)  # read: active_slot, inactive_slot, le32 version

# OTA slots (direct-XIP A/B). Mirror BL_SLOT_* in firmware/common/bl_state.h.
OTA_SLOT_A = 0
OTA_SLOT_B = 1

# --- Mesh GATT Proxy service (SIG-assigned) ---
UUID_PROXY_SVC = "00001828-0000-1000-8000-00805f9b34fb"
UUID_PROXY_DATA_IN = "00002add-0000-1000-8000-00805f9b34fb"   # write
UUID_PROXY_DATA_OUT = "00002ade-0000-1000-8000-00805f9b34fb"  # notify

# Chunk-framing op bytes shared by image / image-slot / screen characteristics.
FRAME_START = 0x01
FRAME_DATA = 0x02
FRAME_END = 0x03

# Image formats / sizes (panel 176x264).
FMT_BW = 1
FMT_GRAY2 = 2
IMG_BW_BYTES = 5808       # 176*264/8
IMG_GRAY2_BYTES = 11616   # 176*264*2/8

# Display kinds.
DISP_KIND_TEXT = 0
DISP_KIND_IMAGE = 1

DEVICE_NAME_PREFIX = "vibamix-"
