#!/usr/bin/env python3
"""Prints the ATT (GATT) packets of a btsnoop HCI log, one per line. Usage: snoop.py FILE [first-line]
Used to see what a computer asks the Kindle's HID service and what the Kindle answers.
Fragmented ACL packets are shown from their first fragment only."""
import struct
import sys

OPS = {1: "ERROR", 2: "mtu req", 3: "mtu rsp", 4: "find info req", 5: "find info rsp", 6: "find by type req",
       7: "find by type rsp", 8: "read by type req", 9: "read by type rsp", 0x0A: "read req", 0x0B: "read rsp",
       0x0C: "read blob req", 0x0D: "read blob rsp", 0x10: "read by group req", 0x11: "read by group rsp",
       0x12: "write req", 0x13: "write rsp", 0x52: "write cmd", 0x1B: "notify", 0x1D: "indicate", 0x1E: "confirm"}

data = open(sys.argv[1], "rb").read()
assert data[:8] == b"btsnoop\0", "not a btsnoop file"
pos, n = 16, 0
while pos + 24 <= len(data):
    _orig, incl, flags, _drops, _ts = struct.unpack(">IIIIq", data[pos:pos + 24])
    pkt = data[pos + 24:pos + 24 + incl]
    pos += 24 + incl
    if len(pkt) < 10 or pkt[0] != 2:        # ACL data only
        continue
    handle, _alen, _llen, cid = struct.unpack("<HHHH", pkt[1:9])
    if (handle >> 12) & 3 == 1 or cid != 4:  # continuation fragment, or not ATT
        continue
    att = pkt[9:]
    n += 1
    who = "PC    ->" if flags & 1 else "Kindle->"
    op = OPS.get(att[0], "op %02x" % att[0])
    rest = att[1:]
    if att[0] == 1 and len(rest) >= 4:
        text = "for op %02x handle %d code %02x" % (rest[0], rest[1] | rest[2] << 8, rest[3])
    elif att[0] in (0x0A, 0x0C, 0x12, 0x52, 0x1B, 0x1D) and len(rest) >= 2:
        text = "handle %d %s" % (rest[0] | rest[1] << 8, rest[2:26].hex())
    elif att[0] in (4, 6, 8, 0x10) and len(rest) >= 4:
        text = "handles %d..%d %s" % (rest[0] | rest[1] << 8, rest[2] | rest[3] << 8, rest[4:].hex())
    else:
        text = rest[:26].hex()
    print("%4d %s %-18s %s" % (n, who, op, text))
