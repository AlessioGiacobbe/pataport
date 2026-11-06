# SPDX-License-Identifier: GPL-3.0-or-later
"""The data-patch tool keeps every container size exact and reduces to one range.

The fixtures build a miniature of the real chain (container -> raw member ->
container -> gzip member -> container -> raw member -> .msg file) with the same
BND v5 shapes the tool parses, at test scale.
"""
import gzip
import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from pataport_data_patches import (
    BndError,
    SPEC,
    bnd_member,
    compute_container_patch,
    compute_spec_patch,
    msg_patch,
    rebuild_gzip_member,
)


def _u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def _bnd(entries):
    """A minimal BND v5 container. `entries` are ("file", name, data) members,
    ("dir", name) folders (name may carry the stored trailing slash), or plain
    (name, data) shorthand for root-level files. A file lands in the folder
    currently open at its depth."""
    norm = [e if e[0] in ("file", "dir") else ("file", e[0], e[1]) for e in entries]
    entries = norm
    n = len(entries)
    blocks_off = 0x28
    records_off = blocks_off + 0x10 * n
    # Each 7-byte record is immediately followed by its name (as the format stores it).
    records_len = sum(7 + len(name.encode()) + 1 for _k, name, *_ in entries)
    attrs_off = records_off + records_len
    data_off = (attrs_off + n + 0x7F) & ~0x7F

    header = bytearray(0x28)
    header[0:4] = b"BND\x00"
    struct.pack_into("<I", header, 0x04, 5)
    struct.pack_into("<I", header, 0x10, records_off)
    struct.pack_into("<I", header, 0x14, data_off)
    struct.pack_into("<I", header, 0x24, n)

    out = bytearray(header)
    blocks = bytearray()
    records = bytearray()
    for i, (kind, name, *_rest) in enumerate(entries):
        blocks += struct.pack("<IIII", 0xDEAD0000 + i, attrs_off + i, 0, 0)  # data filled below
        records += b"\x00\x00\x00" + struct.pack("<I", blocks_off + 0x10 * i) \
            + name.encode() + b"\x00"
    out += blocks + records
    out += bytes((name.rstrip("/").count("/") if kind == "dir" else -1) & 0xFF
                 for kind, name, *_ in entries)   # the level byte each attr_ptr points at
    out += b"\x00" * (data_off - len(out))
    data_ptr = data_off
    for i, (kind, name, *rest) in enumerate(entries):
        struct.pack_into("<I", out, blocks_off + 0x10 * i + 0x04, attrs_off + i)
        if kind == "file":
            data = rest[0]
            struct.pack_into("<I", out, blocks_off + 0x10 * i + 0x08, data_ptr)
            struct.pack_into("<I", out, blocks_off + 0x10 * i + 0x0C, len(data))
            out += data
            data_ptr += len(data)
    return bytes(out)


def _msg(strings):
    """A .msg file: {count, 4, offsets[]} of UTF-16LE NUL-terminated strings."""
    offsets = []
    body = bytearray()
    for text in strings:
        offsets.append(8 + 4 * len(strings) + len(body))
        body += text.encode("utf-16-le") + b"\x00\x00"
    return struct.pack("<II", len(strings), 4) + b"".join(struct.pack("<I", o) for o in offsets) + body


def _msg_strings(msg):
    count = _u32(msg, 0)
    out = []
    for i in range(count):
        start = _u32(msg, 8 + 4 * i)
        end = msg.index(b"\x00\x00", start)
        if end % 2:
            end += 1
        out.append(msg[start:end].decode("utf-16-le"))
    return out


def _gzip_member(body, slack):
    """A stock-shaped gzip member of exact size: zlib output padded with FEXTRA."""
    deflate = zlib.compress(body, 9)[2:-4]
    extra = slack
    header = b"\x1f\x8b\x08\x04" + b"\x00" * 6 + struct.pack("<H", extra) + b"\x00" * extra
    member = header + deflate + struct.pack("<II", zlib.crc32(body) & 0xFFFFFFFF, len(body))
    assert len(gzip.decompress(member)) == len(body)
    return member


def _inflate(raw):
    d = zlib.decompressobj(31)
    return d.decompress(raw) + d.flush()


def _read_msg(container):
    """Walk the whole chain in a (patched or stock) container down to the msg."""
    titledata, _ = bnd_member(container, "loadinggroup/titledata.bnd")
    localize, _ = bnd_member(titledata, "localize.lbnd")
    us_raw, _ = bnd_member(localize, "us.bndz")
    us = _inflate(us_raw)
    scriptlist, _ = bnd_member(us, "scriptlist.bnd")
    member, _ = bnd_member(scriptlist, "titlemsg.msg")
    return member


CHAIN = ["loadinggroup", "titledata.bnd", "localize.lbnd", "us.bndz",
         "scriptlist.bnd", "titlemsg.msg"]


def _build_chain(msg_strings):
    msg = _msg(msg_strings)
    scriptlist = _bnd([("file", "titlemsg.msg", msg)])
    us = _bnd([("file", "scriptlist.bnd", scriptlist)])
    us_gzip = _gzip_member(us, slack=64)
    localize = _bnd([("file", "us.bndz", us_gzip)])
    titledata = _bnd([("file", "localize.lbnd", localize)])
    return (_bnd([("dir", "loadinggroup/"), ("file", "titledata.bnd", titledata)]),
            us_gzip)


class BndMemberTest(unittest.TestCase):
    def test_resolves_member_within_one_container(self):
        blob, _ = _build_chain(["a", "b"])
        member, off = bnd_member(blob, "loadinggroup/titledata.bnd")
        self.assertEqual(member[:4], b"BND\x00")
        self.assertGreater(off, 0x28)
        # Descent into nested containers is the caller's job (one container at a
        # time); a path crossing into a member's own contents is refused.
        with self.assertRaises(BndError):
            bnd_member(blob, "loadinggroup/titledata.bnd/localize.lbnd")

    def test_rejects_missing_and_folder_paths(self):
        blob, _ = _build_chain(["a"])
        with self.assertRaises(BndError):
            bnd_member(blob, "nope.bnd")
        with self.assertRaises(BndError):
            bnd_member(blob, "titledata.bnd/titlemsg.msg")


class MsgPatchTest(unittest.TestCase):
    def test_pads_to_stock_slot_and_keeps_size(self):
        msg = _msg(["ON", "・Installation data", "short"])
        patched = msg_patch(msg, {1: "・Quality"})
        self.assertEqual(len(patched), len(msg))
        strings = _msg_strings(patched)
        self.assertEqual(strings[0], "ON")
        self.assertEqual(strings[1], "・Quality")
        self.assertEqual(strings[2], "short")

    def test_rejects_oversize_and_out_of_range(self):
        msg = _msg(["ON", "・Installation data"])
        with self.assertRaises(BndError):
            msg_patch(msg, {1: "・" * 30})
        with self.assertRaises(BndError):
            msg_patch(msg, {5: "x"})


class RebuildGzipMemberTest(unittest.TestCase):
    def test_rebuilds_to_exact_size_and_inflates_1_to_1(self):
        body = _bnd([("x.bin", b"payload " * 40)])
        raw = _gzip_member(body, slack=64)
        rebuilt = rebuild_gzip_member(raw, body)
        self.assertEqual(len(rebuilt), len(raw))
        self.assertEqual(rebuilt[3] & 0x04, 0x04)      # FEXTRA set
        self.assertEqual(gzip.decompress(rebuilt), body)

    def test_rejects_oversize(self):
        body = _bnd([("x.bin", b"payload " * 40)])
        raw = _gzip_member(body, slack=0)
        # Incompressible bytes: even zlib -9 cannot fit them into the stock size.
        bigger = bytes(range(256)) * 8
        with self.assertRaises(BndError):
            rebuild_gzip_member(raw, bigger)


class ComputeContainerPatchTest(unittest.TestCase):
    def test_one_range_renames_only_the_target_string(self):
        strings = ["ON", "OFF", "・Installation data", "Choose whether to use"]
        blob, us_gzip = _build_chain(strings)
        path = "/".join(CHAIN)
        offset, stock_range, patched_range = compute_container_patch(
            blob, path, {2: "・Quality"})
        self.assertEqual(len(stock_range), len(patched_range))
        self.assertEqual(len(stock_range), len(us_gzip))

        patched = bytearray(blob)
        patched[offset:offset + len(stock_range)] = patched_range
        # Everything outside the one range is byte-identical.
        self.assertEqual(bytes(patched[:offset]), blob[:offset])
        self.assertEqual(bytes(patched[offset + len(stock_range):]),
                         blob[offset + len(stock_range):])
        # The whole chain re-reads from the patched container, texts applied.
        member = _read_msg(bytes(patched))
        self.assertEqual(_msg_strings(member)[2], "・Quality")
        self.assertEqual(_msg_strings(member)[0], "ON")
        self.assertEqual(_msg_strings(member)[3], "Choose whether to use")

    def test_no_change_is_refused(self):
        blob, _ = _build_chain(["ON"])
        with self.assertRaises(BndError):
            # A replacement identical to the stock text must not ship as a patch.
            compute_container_patch(blob, "/".join(CHAIN), {0: "ON"})

    def test_missing_member_is_refused(self):
        blob, _ = _build_chain(["ON"])
        with self.assertRaises(BndError):
            compute_container_patch(blob, "/".join(CHAIN[:-1]) + "/nope.msg", {0: "x"})

    def test_spec_helper_uses_the_named_disc(self):
        strings = ["ON", "OFF"] + [f"row {i}" for i in range(2, 17)] \
            + ["・Installation data", "Choose whether to use\\ninstallation data."]
        blob, _ = _build_chain(strings)
        offset, _, patched_range = compute_spec_patch("UCES01421", blob)
        self.assertGreater(offset, 0x28)
        # The renamed texts live inside the recompressed member; verify by
        # inflating the patched range and walking back down to the member.
        us = _inflate(patched_range)
        scriptlist, _ = bnd_member(us, "scriptlist.bnd")
        member, _ = bnd_member(scriptlist, "titlemsg.msg")
        texts = _msg_strings(member)
        self.assertEqual(texts[17], "・Quality")
        self.assertEqual(texts[18], "Sharpens the 3D view.\\nUp to four times.")
        self.assertEqual(texts[0], "ON")


class SpecTest(unittest.TestCase):
    def test_replacements_fit_their_stock_slots(self):
        # The real stock slots: 17 chars for id 16's header neighbour, 19 for the
        # row label, 41 for the description. The spec's texts must fit all three.
        limits = {17: 19, 18: 41}
        for msg_id, text in SPEC["UCES01421"]["replacements"].items():
            self.assertLessEqual(len(text.encode("utf-16-le")), limits[msg_id] * 2)


if __name__ == "__main__":
    unittest.main()
