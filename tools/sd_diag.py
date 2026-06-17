#!/usr/bin/env python3
"""One-off SD diagnostic: geometry, raw write/read, and mkfs, on a held
connection after a hard reset. Helps localize why VfsFat.mkfs reported EBUSY."""
import sys
from sd_format_push import hard_reset_and_wait, raw_exec

WIDTH = sys.argv[1] if len(sys.argv) > 1 else "4"
FREQ = sys.argv[2] if len(sys.argv) > 2 else "20000000"

ser = hard_reset_and_wait("/dev/ttyACM0")
try:
    data = "(39,40,41,42)" if WIDTH == "4" else "(39,)"
    print(raw_exec(ser,
        "import machine, vfs, os\n"
        "sd = machine.SDCard(slot=1, width=%s, sck=43, cmd=44, data=%s, freq=%s)\n"
        "print('blocks', sd.ioctl(4, 0), 'blksize', sd.ioctl(5, 0))\n"
        "b = bytearray(512)\n"
        "for i in range(512): b[i] = i & 0xff\n"
        "try:\n"
        "    sd.writeblocks(0, b); r = bytearray(512); sd.readblocks(0, r)\n"
        "    print('write/read:', 'MATCH' if r == b else 'MISMATCH')\n"
        "except Exception as e:\n"
        "    print('writeblocks FAIL:', repr(e))\n"
        "try:\n"
        "    vfs.VfsFat.mkfs(sd); print('mkfs OK')\n"
        "except Exception as e:\n"
        "    print('mkfs FAIL:', repr(e))\n"
        % (WIDTH, data, FREQ), timeout=30))
finally:
    ser.close()
