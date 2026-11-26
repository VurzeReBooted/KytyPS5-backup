"""Build a tiny plaintext FIH/CNT/PFS/NAPS image and exercise Kyty's archive API."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def dirent(inode, kind, name):
    name = name.encode()
    return struct.pack("<IIII", inode, kind, len(name), 16 + len(name)) + name


def fixture():
    block = 0x10000
    outer = block
    image = outer + 4 * block
    tail = image + 8 * block
    cnt = tail + block
    pkg = bytearray(cnt + 0x200)
    magic = 20130315

    def put(at, fmt, *values):
        struct.pack_into(fmt, pkg, at, *values)

    pkg[:4] = b"\x7fFIH"
    put(0x10, "<QQQ", outer, cnt - outer, outer)
    put(0x58, "<Q", cnt)
    put(outer, "<QQ", 2, magic)
    put(outer + 0x20, "<I", block)
    put(outer + 0x30, "<Q", 3)
    put(outer + 0xd8, "<Q", 1)

    def outer_file(inode, blocks, size):
        rec = outer + block + inode * 0xa8
        put(rec + 8, "<Q", size)
        for i, address in enumerate(blocks):
            put(rec + 0x64 + i * 4, "<I", address)

    names = dirent(1, 2, "pfs_image.dat") + dirent(2, 2, "naps_pkg_layout.dat")
    outer_file(0, [2], len(names))
    outer_file(1, range(4, 13), 9 * block)
    pkg[outer + 2 * block:outer + 2 * block + len(names)] = names

    layout = bytearray(struct.pack("<QQ", 2 | (3 << 32), 9 | (2 << 24)) + bytes(9 * 8))
    for address, kind in [(0, 0), (8 * block, 0), (9 * block, 0x40)]:
        layout.extend(address.to_bytes(5, "little") + bytes([kind]))
    layout.extend(bytes(10))  # one u2c entry, compact layout
    layout.extend(bytes(-len(layout) % 8))
    for address in [0, 0, 0, block]:
        layout.extend(address.to_bytes(9, "little"))
    outer_file(2, [3], len(layout))
    pkg[outer + 3 * block:outer + 3 * block + len(layout)] = layout

    # Metadata lives in the last block. The asset crosses a 256 KiB read boundary.
    put(tail, "<QQ", 2, magic)
    put(tail + 0x1c, "<H", 0x10)
    put(tail + 0x20, "<I", 4096)
    put(tail + 0x30, "<QQ", 5, 9 * block // 4096)
    def inner(inode, mode, logical, data):
        rec = tail + 4096 + inode * 0xa8
        put(rec, "<H", mode)
        put(rec + 8, "<Q", len(data))
        put(rec + 0x60, "<Q", logical)
        pkg[image + logical:image + logical + len(data)] = data
    inner(0, 0x4000, 8 * block + 8192, dirent(1, 3, "uroot"))
    inner(1, 0x4000, 8 * block + 12288, dirent(2, 2, "eboot.bin") + dirent(3, 3, "data"))
    # A clear PS5 SELF containing a single loadable segment.
    ident = b"\x7fELF\x02\x01\x01\x09\x02".ljust(16, b"\0")
    elf = struct.pack("<16sHHIQQQIHHHHHH", ident, 0xfe10, 62, 1, 0x4000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0x1000, 0x4000, 0, 4, 4, 0x1000)
    self_ident = bytes.fromhex("54 14 f5 ee 10 01 01 12 01 01 00 10")
    header = struct.pack("<12sHHQHHI", self_ident, 0, 0, 188, 1, 0x32, 0)
    segment = struct.pack("<QQQQ", 0x800, 184, 4, 4)
    inner(2, 0x8000, 0, header + segment + elf + phdr + b"TEST")
    inner(3, 0x4000, 8 * block + 16384, dirent(4, 2, "asset"))
    inner(4, 0x8000, 4 * block - 6, b"beforeAFTER")
    pkg[cnt:cnt + 4] = b"\x7fCNT"
    put(cnt + 16, ">I", 1)
    put(cnt + 24, ">I", 128)
    put(cnt + 32, ">QQ", 160, 2)
    put(cnt + 128, ">I", 0x2000)
    put(cnt + 144, ">II", 160, 2)
    pkg[cnt + 160:cnt + 162] = b"{}"
    return pkg


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        pkg = Path(tmp) / "stream test.PKG"
        contents = fixture()
        pkg.write_bytes(contents)
        subprocess.run([sys.argv[1], "--pkg-mount", str(pkg)], cwd=tmp, check=True, timeout=30)
        assert pkg.read_bytes() == contents and list(Path(tmp).iterdir()) == [pkg]
