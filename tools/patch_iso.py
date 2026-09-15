#!/usr/bin/env python3
"""Patch a bigger main.dol into a plain (unencrypted) GameCube ISO.

GC discs have no encryption/partition wrapping (unlike Wii), so this is a
purely mechanical shift: everything from the old fst_offset onward moves
forward by the DOL's size increase, and every FILE entry's absolute disc
offset in the FST gets the same delta added. Directory entries carry no
absolute offset (their second field is a tree-traversal index, not a byte
offset) so they're untouched.
"""
import struct
import sys

SRC_ISO = sys.argv[1]
NEW_DOL = sys.argv[2]
OUT_ISO = sys.argv[3]

ALIGN = 32


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


with open(SRC_ISO, "rb") as f:
    disc = bytearray(f.read())

with open(NEW_DOL, "rb") as f:
    new_dol = f.read()

dol_offset = struct.unpack(">I", disc[0x420:0x424])[0]
fst_offset = struct.unpack(">I", disc[0x424:0x428])[0]
fst_size = struct.unpack(">I", disc[0x428:0x42C])[0]
fst_max = struct.unpack(">I", disc[0x42C:0x430])[0]

old_dol_size = fst_offset - dol_offset  # includes original alignment padding
new_dol_size_aligned = align_up(len(new_dol), ALIGN)
delta = new_dol_size_aligned - old_dol_size

print(f"dol_offset=0x{dol_offset:X} fst_offset=0x{fst_offset:X} "
      f"fst_size=0x{fst_size:X} fst_max=0x{fst_max:X}")
print(f"old dol slot size=0x{old_dol_size:X} new dol (aligned)=0x{new_dol_size_aligned:X} "
      f"delta=0x{delta:X} ({delta} bytes)")

if delta == 0:
    print("No shift needed, just overwriting main.dol in place.")
    disc[dol_offset:dol_offset + len(new_dol)] = new_dol
    with open(OUT_ISO, "wb") as f:
        f.write(disc)
    sys.exit(0)

new_fst_offset = fst_offset + delta

# Read + patch the FST in place (still at its OLD position in `disc`,
# before we do the big shift below).
fst = bytearray(disc[fst_offset:fst_offset + fst_size])
num_entries = struct.unpack(">I", fst[8:12])[0]
patched_files = 0
for i in range(num_entries):
    off = i * 12
    flags = fst[off]
    if flags == 0:  # file entry
        file_offset = struct.unpack(">I", fst[off + 4:off + 8])[0]
        new_file_offset = file_offset + delta
        fst[off + 4:off + 8] = struct.pack(">I", new_file_offset)
        patched_files += 1
print(f"Patched {patched_files} file offsets in FST ({num_entries} total entries)")

# Patch boot.bin's fst_offset field.
disc[0x424:0x428] = struct.pack(">I", new_fst_offset)

# Assemble the new disc image:
#   [0 .. dol_offset)                  unchanged header/apploader region
#   [dol_offset .. +new_dol_size)      our new main.dol, zero-padded to align
#   [ .. new_fst_offset)               (already covered by the padding above)
#   [new_fst_offset ..)                the patched FST, then all file data
#                                       (which is just the OLD bytes from
#                                       fst_offset onward, unmoved relative
#                                       to each other -- only their new
#                                       absolute position, and the offsets
#                                       recorded inside the FST, changed)
tail = disc[fst_offset + fst_size:]  # raw file data after the FST itself
head = disc[:dol_offset]

out = bytearray()
out += head
out += new_dol
out += b"\x00" * (new_dol_size_aligned - len(new_dol))
out += fst
out += tail

with open(OUT_ISO, "wb") as f:
    f.write(out)

print(f"Wrote {OUT_ISO}: {len(out)} bytes (was {len(disc)})")
