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
import tempfile
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from pataport_data_patches import (
    BndError,
    SPEC,
    bnd_member,
    build_artifacts,
    compute_container_patch,
    compute_spec_patch,
    msg_patch,
    read_disc_file,
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
        msg = _msg(["ON", "・Old label", "short"])
        patched = msg_patch(msg, {1: "・New label"})
        self.assertEqual(len(patched), len(msg))
        strings = _msg_strings(patched)
        self.assertEqual(strings[0], "ON")
        self.assertEqual(strings[1], "・New label")
        self.assertEqual(strings[2], "short")

    def test_rejects_oversize_and_out_of_range(self):
        msg = _msg(["ON", "・Old label"])
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
        strings = ["ON", "OFF", "・Old label", "Choose whether to use"]
        blob, us_gzip = _build_chain(strings)
        path = "/".join(CHAIN)
        offset, stock_range, patched_range = compute_container_patch(
            blob, path, {2: "・New label"})
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
        self.assertEqual(_msg_strings(member)[2], "・New label")
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
        blob, _ = _build_chain(_spec_stock_strings())
        offset, _, patched_range = compute_spec_patch("UCES01421", blob)
        self.assertGreater(offset, 0x28)
        # The renamed texts live inside the recompressed member; verify by
        # inflating the patched range and walking back down to the member.
        us = _inflate(patched_range)
        scriptlist, _ = bnd_member(us, "scriptlist.bnd")
        member, _ = bnd_member(scriptlist, "titlemsg.msg")
        texts = _msg_strings(member)
        self.assertEqual(texts[0x08], "PC Settings")
        self.assertEqual(texts[0x3C], "・Quality")
        self.assertEqual(texts[0x3E], "1080p")
        self.assertEqual(texts[0x3F], "2K")
        self.assertEqual(texts[0x40], "4K")
        self.assertEqual(texts[0x41], "")
        self.assertEqual(texts[0x42], "")
        self.assertEqual(texts[0], "ON")


class SpecTest(unittest.TestCase):
    def test_replacements_fit_their_stock_slots(self):
        # The real stock slots (measured from the disc's titlemsg.msg, in
        # characters; the game's backslash-n break counts as two). The spec's
        # texts must fit every slot they relabel.
        limits = {
            0x08: 13, 0x09: 35, 0x3B: 15, 0x3C: 17, 0x3D: 51, 0x3E: 7,
            0x3F: 7, 0x40: 12, 0x41: 20, 0x42: 6, 0x44: 9, 0x46: 10,
            0x49: 39, 0x4B: 41,
        }
        for msg_id, text in SPEC["UCES01421"]["replacements"].items():
            self.assertLessEqual(len(text.encode("utf-16-le")), limits[msg_id] * 2)


def _iso(container):
    """A minimal ISO 9660 image: root -> PSP_GAME -> USRDIR -> DATA_CMN.BND."""
    sector = 2048

    def rec(name, extent, size, is_dir=False):
        r = bytearray()
        r.append(0)                                   # length, patched below
        r += b"\x00" * 31                             # ext attr, extents, size, date, flags...
        struct.pack_into("<I", r, 2, extent)
        struct.pack_into(">I", r, 6, extent)
        struct.pack_into("<I", r, 10, size)
        struct.pack_into(">I", r, 14, size)
        r[25] = 2 if is_dir else 0                    # flags
        name_bytes = name.encode() + (b"\x00" if is_dir else b";1")
        r.append(len(name_bytes))                     # index 32: the identifier length
        r += name_bytes
        if len(r) % 2:
            r.append(0)
        r[0] = len(r)
        return bytes(r)

    def dir_sector(*records):
        data = b"".join(records) + b"\x00" * 34
        return data + b"\x00" * ((-len(data)) % sector)

    root_ext, game_ext, usr_ext, file_ext = 17, 18, 19, 20
    root_dir = dir_sector(rec("PSP_GAME", game_ext, sector, is_dir=True))
    game_dir = dir_sector(rec("USRDIR", usr_ext, sector, is_dir=True))
    usr_dir = dir_sector(rec("DATA_CMN.BND", file_ext, len(container)))

    pvd = bytearray(sector)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    root = rec("", root_ext, len(root_dir), is_dir=True)
    pvd[156:156 + len(root)] = root

    total = file_ext + (len(container) + sector - 1) // sector + 1
    out = bytearray(total * sector)
    out[16 * sector:17 * sector] = pvd
    for ext, data in ((root_ext, root_dir), (game_ext, game_dir), (usr_ext, usr_dir)):
        out[ext * sector:ext * sector + len(data)] = data
    out[file_ext * sector:file_ext * sector + len(container)] = container
    return bytes(out)


class DiscReadTest(unittest.TestCase):
    def test_reads_the_container_from_a_disc_image(self):
        blob, _ = _build_chain(["ON"])
        iso = _iso(blob)
        self.assertEqual(read_disc_file(_iso_str(blob), "PSP_GAME/USRDIR/DATA_CMN.BND"), blob)

    def test_missing_file_is_refused(self):
        blob, _ = _build_chain(["ON"])
        with self.assertRaises(BndError):
            read_disc_file(_iso_str(blob), "PSP_GAME/USRDIR/OTHER.BND")


class BuildArtifactsTest(unittest.TestCase):
    def test_writes_payload_and_descriptor(self):
        import json
        import tempfile
        blob, _ = _build_chain(_spec_stock_strings())
        with tempfile.TemporaryDirectory() as tmp:
            descriptor = build_artifacts("UCES01421", _iso_str(blob), pathlib.Path(tmp) / "pdp")
            self.assertIsNotNone(descriptor)
            saved = json.loads(pathlib.Path(descriptor).read_text(encoding="utf-8"))
            self.assertEqual(saved["disc_path"], "data_cmn.bnd")
            self.assertEqual(saved["size"], saved["offset"] and
                             len((pathlib.Path(descriptor).parent / "payload.bin").read_bytes()))
            payload = (pathlib.Path(descriptor).parent / "payload.bin").read_bytes()
            us = _inflate(payload)
            scriptlist, _ = bnd_member(us, "scriptlist.bnd")
            member, _ = bnd_member(scriptlist, "titlemsg.msg")
            self.assertEqual(_msg_strings(member)[0x08], "PC Settings")
            self.assertEqual(_msg_strings(member)[0x3F], "2K")

    def test_foreign_disc_skips_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(
                build_artifacts("UCES01421", _iso_str(b"not a bnd"), pathlib.Path(tmp) / "pdp"))


def _iso_str(container):
    with tempfile.NamedTemporaryFile(suffix=".iso", delete=False) as f:
        f.write(_iso(container))
        return f.name


def _spec_stock_strings():
    """A .msg sized and shaped like the real titlemsg.msg: every id the spec
    relabels carries its stock text (the game's backslash-n break is the two
    characters backslash and n), the rest are fillers."""
    stock = {
        0x00: "ON",
        0x01: "OFF",
        0x08: "Chat Settings",
        0x09: "Change chat mode,\\nfont color, etc.",
        0x3B: "<Chat Settings>",
        0x3C: "・Quest chat mode.",
        0x3D: "Select chat mode\\n(4 Chat, 9 Chat, or Hideout Chat)",
        0x3E: "4 types",
        0x3F: "9 types",
        0x40: "Hideout Type",
        0x41: "・Chat color settings",
        0x42: "System",
        0x44: "Your chat",
        0x46: "Other chat",
        0x49: "Restore all chat\\nsettings to defaults.",
        0x4B: "Resetting all chat\\nsettings. Is this OK?",
    }
    return [stock.get(i, f"filler {i}") for i in range(0x4C)]


class CodegenEmissionTest(unittest.TestCase):
    def test_emits_registration_and_ranges(self):
        import importlib
        codegen = importlib.import_module("codegen")
        codegen.PATAPORT_DATA_PATCHES.clear()
        codegen.PATAPORT_DATA_PATCHES.append(
            ("data_cmn.bnd", "ab" * 32, b"\x01\x02\x03", 0x8B6080))
        lines = codegen._pataport_data_lines()
        text = "\n".join(lines)
        self.assertIn('sr_enhancement_file_patch_register("data_cmn.bnd", nk_pataport_sha_0, '
                      'nk_pataport_ranges_0, 1u)', text)
        self.assertIn("{0x8b6080u, nk_pataport_range_data_0, 3u}", text)
        self.assertIn("0xab, 0xab", text)      # the stock digest bytes
        codegen.PATAPORT_DATA_PATCHES.clear()


if __name__ == "__main__":
    unittest.main()
