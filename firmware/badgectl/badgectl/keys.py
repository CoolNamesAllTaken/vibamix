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
# Mesh is the BROADCAST/EPHEMERAL surface: it overrides live state on every badge
# and never stores anything. Per-badge persistent config is GATT-only (below).
# (0x01/0x02/0x0A/0x0B/0x0C — set name/fun-fact/display-stored/attendee/frame-LED —
# were removed; that config moved to GATT.)
OP_SET_LED = 0x03         # anim, r, g, b   (live LED override, not stored)
OP_IMG_START = 0x04       # le16 size, w, h (render-only image, not stored)
OP_IMG_DATA = 0x05        # le16 off, bytes
OP_IMG_END = 0x06         # le32 crc
OP_HEARTBEAT = 0x07       # optional UTF-8 event name (<=8 bytes, keeps it unsegmented)
OP_SHOW_TEXT_HDR = 0x08   # title           (draw text frame, not stored)
OP_SHOW_TEXT_BODY = 0x09  # seq, last, body

# LED animation codes (mirror enum LedPattern in firmware src/LEDStrip.h).
ANIM_OFF = 0
ANIM_SOLID = 1
ANIM_RAINBOW = 2
ANIM_WHEEL = 3
ANIM_BREATHE = 4
ANIM_COMET = 5
ANIM_SPARKLE = 6
ANIM_NAMES = {
    ANIM_OFF: "Off",
    ANIM_SOLID: "Solid",
    ANIM_RAINBOW: "Rainbow",
    ANIM_WHEEL: "Wheel",
    ANIM_BREATHE: "Breathe",
    ANIM_COMET: "Comet",
    ANIM_SPARKLE: "Sparkle",
}


def vendor_opcode(op: int) -> bytes:
    """3-octet vendor opcode on the wire: 0xC0|op, then Company ID little-endian."""
    return bytes([0xC0 | (op & 0x3F), COMPANY_ID & 0xFF, (COMPANY_ID >> 8) & 0xFF])


# --- Custom config GATT service (f0de00xx-4b1c-4e2a-9a11-a1b2c3d4e5f6) ---
def _u(short: int) -> str:
    return f"f0de{short:04x}-4b1c-4e2a-9a11-a1b2c3d4e5f6"


UUID_CFG_SVC = _u(0x0001)
UUID_CHR_IMAGE = _u(0x0002)      # render-only 1bpp image (quick render, not stored)
# One read/write characteristic per frame type. Each carries the whole frame and
# folds in a "display now" op. Write is tagged by a leading op byte (GOP_*); read
# is preceded by GOP_SELECT (which serializes the frame on the badge).
UUID_IDENTITY = _u(0x0003)       # identity frame: name + ID + LED + image
UUID_TEXT_FRAME = _u(0x0004)     # text frame: title + body + LED (indexed)
UUID_IMAGE_FRAME = _u(0x0005)    # image frame: image + LED (indexed)
UUID_CHR_OTA = _u(0x0009)        # firmware OTA update (trailered direct-XIP image)
UUID_CHR_OTA_STATUS = _u(0x000A)  # read: active_slot, inactive_slot, le32 version
UUID_CHR_KEEPALIVE = _u(0x000B)  # write (app->badge) + notify (badge->app), 1 Hz liveness
UUID_MESH_TX = _u(0x000C)        # write a vendor-model access payload -> gateway re-broadcasts to mesh

# Frame-characteristic op bytes (first byte of a write; mirror config_gatt.c).
GOP_START = 0x01    # begin chunked payload (image pixels / text body)
GOP_DATA = 0x02
GOP_END = 0x03
GOP_META = 0x10     # identity: name + ID + LED in one small write
GOP_DISPLAY = 0x20  # show this frame on the panel now
GOP_SELECT = 0x21   # serialize this frame so the next read returns it
GOP_READ_AT = 0x22  # set the read window base (le16 offset) for the next read

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

# Display/frame kinds (mirror APP_DISP_KIND_* in firmware src/app_config.h).
DISP_KIND_TEXT = 0
DISP_KIND_IMAGE = 1
DISP_KIND_IDENTITY = 2

# Image slots (mirror firmware src/badge_store.h): 0..3 user image frames; 4 is the
# dedicated identity-frame image (drawn below the name/ID banner; must be gray2).
SLOT_IDENTITY = 4

DEVICE_NAME_PREFIX = "vibamix-"
