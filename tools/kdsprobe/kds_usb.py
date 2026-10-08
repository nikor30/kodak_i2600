"""Minimal native access to the Kodak i2x00 over libusb-1.0 (ctypes, no pyusb).

Protocol facts come from docs/protocol/. Only requests listed there as safe are
sent: READ_SAFE for IN requests, WRITE_SAFE for OUT requests. Everything else is
refused (CLAUDE.md hard rule: no unclassified opcodes to the real device).
"""
import ctypes
import ctypes.util
import struct

VID, PID = 0x040A, 0x601D
EP_INT_A, EP_INT_B = 0x81, 0x88

# bRequest values (docs/protocol/commands.md)
GET_STATUS = 0x00
GET_FW_VERSIONS = 0x02
METERS = 0x33
EOL_CONFIGURATION = 0x34
SERIAL_NUMBER = 0x36
LCD_POPULATE = 0x62

READ_SAFE = {GET_STATUS, GET_FW_VERSIONS, METERS, EOL_CONFIGURATION, SERIAL_NUMBER}
WRITE_SAFE = {LCD_POPULATE}

LCD_W, LCD_H = 128, 48          # LCDPopulate bitmap: 6 pages of 128 column bytes, bit 0 = top row
LCD_MSG_DISCONNECTED = (4, 1)   # (type, id) of the one message the vendor driver uploads on every open

LIBUSB_ERROR_TIMEOUT = -7


class UsbError(OSError):
    pass


class Device:
    def __init__(self, vid=VID, pid=PID):
        self.lib = ctypes.CDLL(ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0")
        self.lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        self.lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        self.lib.libusb_control_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint16,
            ctypes.c_char_p, ctypes.c_uint16, ctypes.c_uint]
        self.lib.libusb_interrupt_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_char_p, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
        for f in ("libusb_claim_interface", "libusb_release_interface"):
            getattr(self.lib, f).argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_close.argtypes = [ctypes.c_void_p]
        self.lib.libusb_error_name.restype = ctypes.c_char_p
        self.ctx = ctypes.c_void_p()
        self._check(self.lib.libusb_init(ctypes.byref(self.ctx)), "init")
        self.lib.libusb_exit.argtypes = [ctypes.c_void_p]
        self.h = self.lib.libusb_open_device_with_vid_pid(self.ctx, vid, pid)
        if not self.h:
            raise UsbError(f"scanner {vid:04x}:{pid:04x} not found (or no permission)")
        self.claimed = False

    def _check(self, rc, what):
        if rc < 0:
            raise UsbError(f"{what}: {self.lib.libusb_error_name(rc).decode()}")
        return rc

    def close(self):
        if self.h:
            if self.claimed:
                self.lib.libusb_release_interface(self.h, 0)
            self.lib.libusb_close(self.h)
            self.h = None
            self.lib.libusb_exit(self.ctx)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def get(self, request, length, value=0, index=0, timeout=2000):
        """Vendor IN request (bmRequestType 0xc0)."""
        if request not in READ_SAFE:
            raise PermissionError(f"request 0x{request:02x} is not classified as a safe read")
        buf = ctypes.create_string_buffer(length)
        n = self._check(self.lib.libusb_control_transfer(
            self.h, 0xC0, request, value, index, buf, length, timeout), f"get 0x{request:02x}")
        return buf.raw[:n]

    def set(self, request, value=0, index=0, data=b"", timeout=2000):
        """Vendor OUT request (bmRequestType 0x40)."""
        if request not in WRITE_SAFE:
            raise PermissionError(f"request 0x{request:02x} is not classified as a safe write")
        self._check(self.lib.libusb_control_transfer(
            self.h, 0x40, request, value, index, bytes(data), len(data), timeout), f"set 0x{request:02x}")

    def lcd_populate(self, bitmap, msg=LCD_MSG_DISCONNECTED):
        """Upload one LCD message bitmap (768 bytes, see lcd_bitmap()).

        Only the message seen in captures is allowed; other (type, id) pairs
        (e.g. the function labels, docs/protocol/commands.md section 7) are not classified yet.
        """
        if len(bitmap) != LCD_W * LCD_H // 8 or msg != LCD_MSG_DISCONNECTED:
            raise ValueError("only the captured message (type 4, id 1) with a 768-byte bitmap is allowed")
        msg_type, msg_id = msg
        self.set(LCD_POPULATE, (msg_id << 8) | msg_type, ((LCD_H // 8) << 8) | LCD_W, bitmap)

    def claim(self):
        if not self.claimed:
            self._check(self.lib.libusb_claim_interface(self.h, 0), "claim interface 0")
            self.claimed = True

    def read_interrupt(self, ep, timeout=250):
        """One 8-byte event from an interrupt endpoint, or None on timeout."""
        self.claim()
        buf = ctypes.create_string_buffer(8)
        got = ctypes.c_int(0)
        rc = self.lib.libusb_interrupt_transfer(self.h, ep, buf, 8, ctypes.byref(got), timeout)
        if rc == LIBUSB_ERROR_TIMEOUT:
            return None
        self._check(rc, f"interrupt ep 0x{ep:02x}")
        return buf.raw[:got.value]


STATUS_FIELDS = ("fw_id", "version", "board_type", "unknown6", "tick", "power", "buffer",
                 "transport", "tray", "udds", "lamp", "interlock", "button", "print_head",
                 "printer", "front_camera", "rear_camera", "carriage", "lamp_intensity", "error")


def parse_status(b):
    """32-byte GetStatus reply -> dict (layout: docs/protocol/commands.md)."""
    vals = struct.unpack_from("<B4sBHI5BH9B", b, 0)
    d = dict(zip(STATUS_FIELDS, vals))
    d["version"] = d["version"].hex()
    return d


def lcd_bitmap(rows):
    """rows: LCD_H sequences of LCD_W truthy/falsy pixels -> 768-byte LCDPopulate payload."""
    out = bytearray(LCD_W * LCD_H // 8)
    for y, row in enumerate(rows):
        for x, px in enumerate(row):
            if px:
                out[(y // 8) * LCD_W + x] |= 1 << (y % 8)
    return bytes(out)


def lcd_rows(bitmap):
    """Inverse of lcd_bitmap(): 768 bytes -> list of LCD_H strings of '#'/' '."""
    return ["".join("#" if bitmap[(y // 8) * LCD_W + x] >> (y % 8) & 1 else " " for x in range(LCD_W))
            for y in range(LCD_H)]
