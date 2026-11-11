# SPDX-License-Identifier: GPL-3.0-or-later
"""Pataport's changes to a game's data files, computed from the user's own disc at build time.

patapon3-pcmenu: the OPTION screen's Chat Settings entry and page become
Pataport's PC Settings screen (the pages themselves are compiled code, so the
native Chat Settings page is the donor; the code side lives in
tools/pataport_patches.py and the runtime bridge in src/rt/pc_settings.c).
This slice relabels the strings in the disc's text archive: the root row
(id 8), the page title, the quality row and its ◄1080p / 2K / 4K► value labels,
the Reset row's description and confirm dialog, and blanks the three retired
chat-colour rows. The game's own RAM copy of the same strings is rewritten
separately by the bridge (its DATAMS.BND only exists decrypted in guest RAM).
The strings live inside DATA_CMN.BND, five container levels deep, with exactly
one gzip layer:

    DATA_CMN.BND -> loadinggroup/titledata.bnd (raw member)
      -> localize.lbnd (raw member) -> <lang>.bndz (gzip member; 17 languages)
        -> scriptlist.bnd (raw member) -> titlemsg.msg (raw member)

Every level is rewritten at its EXACT stock size, so no parent's entry tables
change and the whole patch reduces to one contiguous byte range of DATA_CMN.BND
(the us.bndz member). Sizes stay exact because the recompressed stream is padded
through the gzip header's optional FEXTRA field (RFC 1952): deflate-level
padding cannot reach every remainder -- stored blocks are five bytes and their
content is output data, while empty fixed-huffman blocks' aligned byte cost is
never 1 mod 5 -- and padding must never follow the final deflate block, which
the member's CRC32 and ISIZE immediately follow. zopfli is used because the
game's packer beat zlib -9 on the stock member by about one percent.

The repo carries the spec only: which strings, in which archive. The bytes are
computed here, from the user's disc, at build time; nothing game-derived is
committed. All-or-nothing: any size overrun or failed round-trip aborts, and
the build ships no partial patch. The runtime verifies the stock file's SHA-256
before serving any patched byte (docs/ENHANCEMENT_CONTRACT.md section 9.6).
"""
import struct
import zlib


def _u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


class BndError(ValueError):
    pass


def _is_gzip(data):
    return data[:3] == b"\x1f\x8b\x08"


def _inflate_gzip(raw):
    d = zlib.decompressobj(31)
    return d.decompress(raw) + d.flush()
    # Trailing bytes inside a member's stored size are container padding; the
    # inflater stops at the stream end, exactly as the game's loader does.


def _entries(container):
    """The entry records of a BND v5 container, in table order. Each yields
    (name, is_folder, data_offset, data_size); folder names keep their trailing
    slash, as stored. Returns None for a container that is not BND v5."""
    if container[:4] != b"BND\x00" or _u32(container, 0x04) != 5:
        raise BndError("not a BND v5 container")
    info = _u32(container, 0x10)
    count = _u32(container, 0x24)
    # Leading all-zero CRC blocks pad the block area; the entry count includes them.
    zero = 0
    while container[0x28 + zero * 0x10:0x38 + zero * 0x10] == b"\x00" * 0x10:
        zero += 1
    count -= zero

    out = []
    off = 0
    for _ in range(count):
        record = container[info + off:info + off + 7]
        crc_ptr = _u32(record, 3)
        name_off = info + off + 7
        name_end = container.index(b"\x00", name_off)
        name = container[name_off:name_end].decode("utf-8", "replace")
        off += 7 + (name_end - name_off) + 1
        if crc_ptr == 0 or crc_ptr + 0x10 > len(container):
            continue
        block = container[crc_ptr:crc_ptr + 0x10]
        attr_ptr = _u32(block, 0x04)
        data_ptr = _u32(block, 0x08)
        size = _u32(block, 0x0C)
        level = struct.unpack_from("<b", container, attr_ptr)[0]
        out.append((name, level >= 0, data_ptr, size))
    return out


def _tree(container):
    """The folder tree of a BND v5 container: {'name': ('dir', subtree) | ('file',
    data_ptr, size)}. A folder's stored name may carry trailing slashes; its slash
    count places it in the tree."""
    root = {}
    folders = [root]
    for name, is_folder, data_ptr, size in _entries(container):
        if is_folder:
            depth = name.rstrip("/").count("/")
            while len(folders) > depth + 1:
                folders.pop()
            node = {}
            folders[-1][name.rstrip("/").split("/")[-1]] = ("dir", node)
            folders.append(node)
        else:
            folders[-1][name] = ("file", data_ptr, size)
    return root


def bnd_member(container, path):
    """Read one member of a BND v5 container by its slash path. Returns
    (member_bytes, data_offset). The leaf must be a file member; descent into
    member containers is the caller's job (one container at a time)."""
    node = _tree(container)
    parts = [p for p in path.split("/") if p]
    for part in parts[:-1]:
        found = node.get(part)
        if found is None or found[0] != "dir":
            raise BndError(f"member {path}: '{part}' is not a folder")
        node = found[1]
    found = node.get(parts[-1])
    if found is None or found[0] != "file":
        raise BndError(f"member {path} not found")
    data_ptr, size = found[1], found[2]
    if data_ptr + size > len(container):
        raise BndError(f"member {path} reaches past its container")
    return container[data_ptr:data_ptr + size], data_ptr


def msg_patch(msg, replacements):
    """Rewrite UTF-16LE strings in a .msg file in place, space-padding each to its
    stock slot (strings are NUL-terminated, so shorter is fine). `replacements`
    maps message id -> new text. Returns the patched bytes; the size never changes."""
    count = _u32(msg, 0)
    out = bytearray(msg)
    for msg_id, text in replacements.items():
        if msg_id >= count:
            raise BndError(f"msg id {msg_id} out of range ({count} strings)")
        start = _u32(msg, 8 + 4 * msg_id)
        end = msg.index(b"\x00\x00", start)
        if end % 2:
            end += 1
        slot = end + 2 - start - 2          # excluding the terminating NUL pair
        encoded = text.encode("utf-16-le")
        if len(encoded) > slot:
            raise BndError(f"msg id {msg_id}: replacement of {len(encoded)} bytes "
                           f"exceeds its stock slot of {slot} bytes")
        out[start:end + 2] = encoded + b"\x00\x00" + b" " * (slot - len(encoded))
    return bytes(out)


def rebuild_gzip_member(raw, body, zopfli=None):
    """Recompress `body` to EXACTLY the stock member's size, padding through the
    header's FEXTRA field. `raw` is the stock member (copied header bytes keep
    its XFL and OS fields). `zopfli` is a module with zlib.compress(bytes,
    numiterations=) that squeezes below zlib -9; without it the stock member
    must still fit zlib's own output."""
    if raw[:3] != b"\x1f\x8b\x08":
        raise BndError("stock member is not a gzip stream")
    if zopfli is not None:
        deflate = zopfli.compress(body, numiterations=100)[2:-4]
    else:
        deflate = zlib.compress(body, 9)[2:-4]
    payload = len(raw) - 18               # header + FEXTRA/XLEN + crc32 + isize
    extra = payload - len(deflate) - 2    # minus the XLEN field itself
    if extra < 0:
        raise BndError(f"recompressed member exceeds the stock size by {-extra} bytes; "
                       "pass the 'zopfli' package, whose output is smaller")
    # Stock header with FEXTRA set: magic, CM, FLG, mtime, XFL, OS, XLEN, extra.
    header = (raw[:3] + b"\x04" + raw[4:10]
              + struct.pack("<H", extra) + b"\x00" * extra + deflate)
    member = header + struct.pack("<II", zlib.crc32(body) & 0xFFFFFFFF, len(body) & 0xFFFFFFFF)
    if len(member) != len(raw):
        raise BndError(f"rebuilt member is {len(member)} bytes, stock is {len(raw)}")
    if _inflate_gzip(member) != body:
        raise BndError("rebuilt member does not inflate 1:1")
    return member


def _walk_chain(container, parts):
    """Resolve a member path through nested containers. Returns one entry per
    container level: (parent, start, raw_member, is_gzip), leaf last. Gzip
    members are followed into their inflated container."""
    chain = []
    current = container
    remaining = list(parts)
    while remaining:
        node = _tree(current)
        consumed = 0
        for part in remaining:
            found = node.get(part)
            if found is None:
                raise BndError(f"member {'/'.join(parts)}: '{part}' not found")
            kind, value = found[0], found[1:]
            consumed += 1
            if kind == "dir":
                node = value[0]       # the subtree dict
                continue
            data_ptr, size = value
            if data_ptr + size > len(current):
                raise BndError(f"member {'/'.join(parts)} reaches past a container")
            raw = current[data_ptr:data_ptr + size]
            chain.append((current, data_ptr, raw, _is_gzip(raw)))
            remaining = remaining[consumed:]
            if remaining:
                current = _inflate_gzip(raw) if _is_gzip(raw) else raw
            break
        else:
            raise BndError(f"member {'/'.join(parts)} ends at a folder")
    if len(chain) < 2:
        raise BndError("member path must descend at least one container level")
    return chain


def compute_container_patch(container, member_path, replacements, zopfli=None):
    """Compute the byte-range patch for one member of a BND container, walking
    every nesting level and keeping all sizes exact. Returns
    (data_offset, stock_bytes, patched_bytes) for the ONE contiguous changed
    range of the outer container. Every intermediate container is rebuilt
    in place: raw members splice directly, gzip members are recompressed to
    their exact stock size."""
    chain = _walk_chain(container, [p for p in member_path.split("/") if p])

    # Patch the leaf, then work upward: each chain entry splices its member's
    # patched bytes into the parent container; a gzip member is recompressed
    # first, to its exact stock size.
    current = msg_patch(chain[-1][2], replacements)
    for parent, start, stock_member, gzip_member in reversed(chain):
        if gzip_member:
            current = rebuild_gzip_member(stock_member, current, zopfli)
        if len(current) != len(stock_member):
            raise BndError(f"member at 0x{start:x} changed size")
        spliced = bytearray(parent)
        spliced[start:start + len(stock_member)] = current
        current = bytes(spliced)

    # The patch must be exactly the deepest gzip member's range in the outer
    # container: everything outside it is byte-identical by construction, and
    # inside it only the recompressed stream differs.
    gzip_level = next((l for l in chain if l[3]), None)
    if gzip_level is None:
        raise BndError("member path carries no gzip level; a plain splice is enough")
    abs_start = 0
    for parent, start, stock_member, gzip_member in chain:
        abs_start += start
        if gzip_member:
            break
    abs_end = abs_start + len(gzip_level[2])
    patched_range = current[abs_start:abs_end]
    if container[abs_start:abs_end] == patched_range:
        raise BndError("patch produced no changed bytes")
    if container[:abs_start] != current[:abs_start] or container[abs_end:] != current[abs_end:]:
        raise BndError("bytes outside the patched member range changed")
    return abs_start, container[abs_start:abs_end], patched_range


# The patapon3-pcmenu spec: which strings, in which archive of which disc.
# The game's line break in these strings is the two characters backslash and n.
SPEC = {
    "UCES01421": {
        "container": "PSP_GAME/USRDIR/DATA_CMN.BND",
        "member": "loadinggroup/titledata.bnd/localize.lbnd/us.bndz/scriptlist.bnd/titlemsg.msg",
        "replacements": {
            0x08: "PC Settings",                                # the root row
            0x09: "Change PC settings\\nfor this port.",        #   its description
            0x3B: "<PC Settings>",                              # the page title
            0x3C: "・Quality",                                  # the quality row
            0x3D: "Sharpens the render.\\n1080p, 2K, or 4K.",
            0x3E: "1080p",                                      # the three value labels
            0x3F: "2K",
            0x40: "4K",
            0x41: "",                                           # the retired chat-colour
            0x42: "",                                           #   header and rows
            0x44: "",
            0x46: "",
            0x49: "Restore all PC\\nsettings to defaults.",     # the Reset row's description
            0x4B: "Resetting all PC\\nsettings. Is this OK?",   #   and confirm dialog
        },
    },
}


def compute_spec_patch(disc_id, container, zopfli=None):
    """Compute the patapon3-pcmenu patch for one disc's container bytes.
    Returns (data_offset, stock_bytes, patched_bytes) as one contiguous range.
    `zopfli` is the imported zopfli.zlib module; without it the stock member
    must still fit zlib -9, which the real titledata member does not."""
    spec = SPEC[disc_id]
    return compute_container_patch(container, spec["member"], spec["replacements"], zopfli)


def read_disc_file(iso_path, wanted):
    """Read one file out of an ISO 9660 image by its slash path (a targeted
    directory lookup, not a full walk). Returns its bytes."""
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root = pvd[156:156 + 34]
        extent = struct.unpack_from("<I", root, 2)[0]
        size = struct.unpack_from("<I", root, 10)[0]
        f.seek(extent * 2048)
        directory = f.read(size)
        parts = [p for p in wanted.split("/") if p]
        while True:
            i = 0
            found = None
            while i < len(directory):
                length = directory[i]
                if length == 0:
                    break
                record = directory[i:i + length]
                name = record[33:33 + record[32]].decode("ascii", "replace")
                name = name.rstrip("\x00").split(";")[0].rstrip(".")
                if record[25] & 2:                      # a directory record
                    if name.upper() == parts[0].upper() and len(parts) > 1:
                        extent = struct.unpack_from("<I", record, 2)[0]
                        size = struct.unpack_from("<I", record, 10)[0]
                        f.seek(extent * 2048)
                        directory = f.read(size)
                        parts = parts[1:]
                        found = "dir"
                        break
                elif name.upper() == parts[0].upper() and len(parts) == 1:
                    extent = struct.unpack_from("<I", record, 2)[0]
                    size = struct.unpack_from("<I", record, 10)[0]
                    found = "file"
                    break
                i += length
            if found == "dir":
                continue
            if found == "file":
                f.seek(extent * 2048)
                out = bytearray()
                left = size
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        raise BndError(f"iso short read for {wanted}")
                    out += chunk
                    left -= len(chunk)
                return bytes(out)
            raise BndError(f"{wanted} not found in the disc image")


def build_artifacts(disc_id, iso_path, out_dir, zopfli=None):
    """Compute the patch from the user's disc and write the build inputs codegen
    consumes: `payload.bin` (the rewritten range) and `descriptor.json`
    ({disc_path, sha256, offset, size, payload}). Returns the descriptor path,
    or None when the disc is not the build the spec was made for (the game then
    builds unchanged -- the patch is skipped whole)."""
    import hashlib
    import json
    from pathlib import Path

    spec = SPEC[disc_id]
    try:
        container = read_disc_file(iso_path, spec["container"])
        _walk_chain(container, [p for p in spec["member"].split("/") if p])
    except (BndError, OSError):
        return None          # not the disc the spec was made for: the game builds unchanged
    offset, _stock, patched = compute_spec_patch(disc_id, container, zopfli)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "payload.bin").write_bytes(patched)
    descriptor = {
        "disc_path": "data_cmn.bnd",     # the guest key data_normalize_guest_key folds to
        "sha256": hashlib.sha256(container).hexdigest(),
        "offset": offset,
        "size": len(patched),
        "payload": "payload.bin",
    }
    (out / "descriptor.json").write_text(json.dumps(descriptor), encoding="utf-8")
    return str(out / "descriptor.json")
