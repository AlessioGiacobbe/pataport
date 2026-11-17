# SPDX-License-Identifier: GPL-3.0-or-later
"""Pataport's changes to a game's code, made before it is translated (codegen --pataport-patch).

patapon3-60fps: "60 FPS Patch v2" for Patapon 3 (all regions), published by Madwig on the PPSSPP
forum (https://forums.ppsspp.org/showthread.php?pid=132462) as a CWCheat code. It turns six
instructions into nops: the frame limiter's loop, which waits until two vblanks have passed since
the last frame, and branches in the game's timing code so that it follows the shorter frame. In
Pataport on an M4 a battle then runs at 60 fps, with the mission timer and the drum combo
advancing exactly as at 30 fps; its author notes unit movement is "a bit off".

patapon3-pcmenu: the OPTION screen's Chat Settings entry and page become Pataport's PC Settings
screen. The game's pages are compiled code (its screen constructor builds six fixed page
objects), so the native Chat Settings page is the donor: its ◄value► cycle row becomes the
quality selector, its chat-colour rows are retired (the cursor's path is clamped to the quality
and Reset rows), and the retired chat-mode value -- a word in the game's System Data record --
becomes the render-scale setting the runtime bridges to the host (PCMENU_CHAIN). Every reader of
that word outside the OPTION screen is detached here, each rewritten to load the stock default
(1) deterministically, so the repurposed value keeps its row, its save persistence and nothing
else. The relabeled strings ride tools/pataport_data_patches.py (the disc's archive) and
PCMENU_TEXT below (the game's own decrypted RAM copy). Without the reader detachments, changing
the quality would silently rearrange the game's chat-log layout.

Each word names the kind of instruction it must replace (a jal also names the callee it must
call; a "word" entry names the exact instruction and its replacement, so the rewrite is
verifiable to the bit). If any differs (another build of the game), the patch is skipped whole
and the game translates unchanged.
"""
import struct

NOP = 0x00000000

PATCHES = {
    "patapon3-60fps": (
        (0x0883E754, "bne"),     # frame limiter: repeat the vblank wait (now: wait once)
        (0x0883D5CC, "bnel"),    # timing: frame-step branches
        (0x0883D5D0, "addiu"),   #   its delay slot
        (0x0883D5DC, "beq"),
        (0x0883D66C, "beql"),
        (0x0883D670, "addu"),    #   its delay slot
    ),
    "patapon3-pcmenu": (
        # The main module's readers of the System Data record's chat-mode word
        # (record + 0x8a8), each rewritten to load the stock default (1):
        # f_08a19e80's chat-message pipeline hands it to f_089f95cc as the
        # message format's second argument; f_088ba9d8's save-info formatter
        # reads it twice while choosing which summary string to build.
        (0x08A1A744, "word", 0x8E0508A8, 0x24050001),  # lw $a1,0x8a8($s0) -> li $a1,1
        (0x088BAAA8, "word", 0x8E8308A8, 0x24030001),  # lw $v1,0x8a8($s4) -> li $v1,1
        (0x088BB2D8, "word", 0x8E8208A8, 0x24020001),  # lw $v0,0x8a8($s4) -> li $v0,1
    ),
}

# The overlay half of patapon3-pcmenu: the same detachment in the game's MWo3
# code overlays, which codegen translates from their own images
# (translate_runtime_module applies these to a module's bytes before analysis,
# and writes them back, so the translated code and the bytes the runtime loads
# at the overlay's fixed address agree). Entries are (vaddr, expected word,
# replacement word), verified to the bit like the "word" entries above.
#
# OL_Azito (the hideout): the chat-log layout readers -- eight coroutine resume
# sites that pick a chat-entry stride from the mode word, and two chat-window
# builders that copy it into their object (each followed by its own store, so
# rewriting the load is enough):
#   nk_lib_a1f0eaa6072a97a8_3.c 0x08afa38c/0x08afa448/0x08afab78/0x08afad94/
#   0x08afbb04/0x08afc0b8/0x08afc0f4/0x08afc174, _10.c 0x08b85e90 (+store
#   0x08b85e9c) and 0x08b87bd4 (+store 0x08b87be0).
OVERLAY_PATCHES = {
    "patapon3-pcmenu": {
        "OL_Title.bin": (
            # The PC Settings page's cursor wrap: UP always lands on row 0
            # (quality) and DOWN always lands on row 4 (Reset), so the three
            # retired chat-colour rows between them are never selected.
            (0x08ACF078, 0x2442FFFF, 0x24020000),  # addiu $v0,$v0,-1 -> li $v0,0
            (0x08ACF0AC, 0x24420001, 0x24020004),  # addiu $v0,$v0,1  -> li $v0,4
        ),
        "OL_Azito.bin": (
            (0x08AFA38C, 0x8C4208A8, 0x24020001),  # lw $v0,0x8a8($v0) -> li $v0,1
            (0x08AFA448, 0x8C4208A8, 0x24020001),
            (0x08AFAB78, 0x8C4508A8, 0x24050001),  # lw $a1,0x8a8($v0) -> li $a1,1
            (0x08AFAD94, 0x8C6308A8, 0x24030001),  # lw $v1,0x8a8($v1) -> li $v1,1
            (0x08AFBB04, 0x8C4308A8, 0x24030001),  # lw $v1,0x8a8($v0) -> li $v1,1
            (0x08AFC0B8, 0x8C4208A8, 0x24020001),
            (0x08AFC0F4, 0x8C4208A8, 0x24020001),
            (0x08AFC174, 0x8C4208A8, 0x24020001),
            (0x08B85E90, 0x8E2208A8, 0x24020001),  # lw $v0,0x8a8($s1) -> li $v0,1
            (0x08B87BD4, 0x8E0208A8, 0x24020001),  # lw $v0,0x8a8($s0) -> li $v0,1
        ),
    },
}

# The PC Settings screen's text, patched where the game actually reads it: the
# game's localize text is loaded from the encrypted DATAMS.BND and decrypted
# into guest RAM, so the rewrite happens in RAM after that load (verified
# against these exact stock bytes before each write; the disc archive's own
# copy of the same strings rides tools/pataport_data_patches.py). The stock
# bytes are facts about the patch; the replacements are this project's own
# text. Each replacement must not be longer than the stock bytes it replaces
# (the write is in place and NUL-terminated).
#
# A short stock string ("System", "4 types") can also appear as a whole string
# elsewhere in the guest's other decrypted text, so those entries anchor
# themselves to the bytes that follow them in this archive -- the game's
# strings sit back to back, and the rewrite writes only the replacement and
# its terminator at the anchor's start, leaving the tail (the neighbour) for
# its own entry. The entries run in one pass in this order, so an anchor's
# tail is still stock when its entry scans; after the pass the anchored
# entries simply wait (and fire again whenever the game re-decrypts).
def _u16(text):
    return text.encode("utf-16-le") + b"\x00\x00"

PCMENU_TEXT = (
    # The OPTION root menu's row (anchored to its description, which is still
    # stock when this entry runs: the description's own entry follows it).
    (_u16("Chat Settings") + _u16("Change chat mode,\\nfont color, etc."),
     _u16("PC Settings")),
    (_u16("Change chat mode,\\nfont color, etc."),
     _u16("Change PC settings\\nfor this port.")),
    # The PC Settings page: title, the quality row, its description.
    (_u16("<Chat Settings>"), _u16("<PC Settings>")),
    (_u16("・Quest chat mode."), _u16("・Quality")),
    (_u16("Select chat mode\\n(4 Chat, 9 Chat, or Hideout Chat)"),
     _u16("Sharpens the render.\\n1080p, 2K, or 4K.")),
    # The quality row's three value labels (each anchored to the next string).
    (_u16("4 types") + _u16("9 types"), _u16("1080p")),
    (_u16("9 types") + _u16("Hideout Type"), _u16("2K")),
    (_u16("Hideout Type") + _u16("・Chat color settings"), _u16("4K")),
    # The retired chat-colour rows and their group header, blanked: the cursor
    # never reaches them (the overlay patch above), and their labels read as
    # nothing.
    (_u16("・Chat color settings") + _u16("System"), _u16("")),
    (_u16("System") + _u16("Change system\\nmessage color."), _u16("")),
    (_u16("Your chat") + _u16("Change chat msg and\\ncharacter name color."),
     _u16("")),
    (_u16("Other chat") + _u16("Change color of other\\nplayers' messages."),
     _u16("")),
    # The Reset row's description and its confirm dialog's body (the title,
    # "Confirm reset", is the game's own and stays).
    (_u16("Restore all chat\\nsettings to defaults."),
     _u16("Restore all PC\\nsettings to defaults.")),
    (_u16("Resetting all chat\\nsettings. Is this OK?"),
     _u16("Resetting all PC\\nsettings. Is this OK?")),
)

# Where the PC Settings bridge finds its value: the config-manager pointer's
# address, the System Data record pointer's offset in it, the quality value's
# offset in the record (a 32-bit word the game's own ◄value► cycle keeps at 0,
# 1 or 2), and the main module's load base (the guest-RAM plausibility floor).
# The generated code hands these to sr_pcsettings_register_chain when the patch
# applies; they live here because they are facts about the patch.
PCMENU_CHAIN = (0x08AABD94, 0x48, 0x8A8, 0x08804000)

# The easter egg's trigger (src/rt/easter_egg.c): the title overlay whose
# binding ends the boot's pre-title phase. The game draws its logo and warning
# screens from the main module before any code overlay binds, with no disc-file
# open (a boot run with SR_IOLOG=1 opens only DATA_CMN.BND and the DATAMS
# archives) and no message dialog marking them; the runtime's own observable
# for the phase's end is this overlay binding. The generated code registers
# the name here and the sound unit beside it, and the clip plays once when the
# overlay binds. The name is an overlay file name, not a code address: like the
# disc paths in tools/pataport_data_patches.py, it is a fact about the game's
# files, carried by the generated code and guarded at run time (a game whose
# overlays bind under other names simply never fires).
EGG_TITLE_OVERLAY = "OL_Title.bin"


def kind(word):
    """The mnemonic of the few instruction kinds the patches name."""
    op = word >> 26
    if op == 0:
        return "addu" if word & 0x3F == 0x21 else None
    if op == 3:
        return "jal"
    return {4: "beq", 5: "bne", 9: "addiu", 0x14: "beql", 0x15: "bnel"}.get(op)


def _jal_target(addr, word):
    """The callee of a jal at addr (its 26-bit index, shifted, in the fetch page)."""
    return ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def _write_word(elf, addr, word):
    """Write one word where Elf.read_at_vaddr reads it: the relocated image of a PRX, or the
    PT_LOAD bytes of a fixed-address executable (Patapon 3's EBOOT)."""
    data = struct.pack("<I", word)
    if elf.reloc is not None:
        o = addr - elf.reloc.lo
        elf.reloc.mem[o:o + 4] = data
        return
    for seg in elf.segments:
        if seg["type"] == 1 and seg["vaddr"] <= addr < seg["vaddr"] + seg["filesz"]:
            if not isinstance(elf.data, bytearray):
                elf.data = bytearray(elf.data)
            o = seg["off"] + addr - seg["vaddr"]
            elf.data[o:o + 4] = data
            return
    raise ValueError(f"0x{addr:08x} is not in a loaded segment")


def apply(elf, name, log):
    """Apply patch `name` to a loaded Elf (analyze.Elf) in place, before any analysis. Returns the
    number of words changed: all of them, or 0 when the code is not the build the patch was made
    for. A "word" entry also accepts its own replacement (an already-applied patch), so a build
    that re-reads patched bytes stays applied."""
    words = PATCHES[name]
    found = []
    for entry in words:
        addr, want, *extra = entry
        raw = elf.read_at_vaddr(addr, 4)
        word = struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else None
        if want == "word":
            expected, replacement = extra
            if word != expected and word != replacement:
                log(f"pataport: {name} not applied: 0x{addr:08x} is neither the expected "
                    f"0x{expected:08x} nor the applied 0x{replacement:08x}; "
                    "this build of the game translates unchanged")
                return 0
            found.append((addr, replacement))
            continue
        if word is None or kind(word) != want:
            log(f"pataport: {name} not applied: 0x{addr:08x} is not the expected {want}; "
                "this build of the game translates unchanged")
            return 0
        if extra and _jal_target(addr, word) != extra[0]:
            log(f"pataport: {name} not applied: 0x{addr:08x} does not call 0x{extra[0]:08x}; "
                "this build of the game translates unchanged")
            return 0
        found.append((addr, NOP))
    for addr, new in found:
        _write_word(elf, addr, new)
    log(f"pataport: {name} applied ({len(found)} instructions)")
    return len(found)


def apply_overlay(data, module_name, applied, log):
    """Apply the overlay half of the applied patch sets to one runtime module's image (an MWo3
    code overlay: a 0x40-byte header whose load address sits at +8; every overlay of this game
    shares one load address, so a set names its module). The image maps linearly at the load
    address -- the file word for vaddr V sits at V - load address, as the generated C's own PCs
    confirm -- so an entry resolves without a header-skip guesswork. Returns (image, words
    written). All-or-nothing per set, like apply(): a word that is neither the expected code nor
    already the replacement zeroes that set's entry in `applied`, so the generated registrations
    stay off."""
    written = 0
    out = data
    for name in list(applied):
        entries = OVERLAY_PATCHES.get(name, {}).get(module_name)
        if not entries:
            continue
        if len(data) < 0x44 or data[:4] != b"MWo3":
            log(f"pataport: {name} not applied: {module_name} is not an MWo3 code overlay; "
                "this build of the game translates unchanged")
            applied[name] = 0
            return data, 0
        base = struct.unpack_from("<I", data, 8)[0]
        found = []
        for addr, expected, replacement in entries:
            off = addr - base
            if off < 0 or off + 4 > len(data):
                log(f"pataport: {name} not applied: 0x{addr:08x} is outside "
                    f"{module_name}; this build of the game translates unchanged")
                applied[name] = 0
                return data, 0
            word = struct.unpack_from("<I", data, off)[0]
            if word != expected and word != replacement:
                log(f"pataport: {name} not applied: {module_name} 0x{addr:08x} is neither the "
                    f"expected 0x{expected:08x} nor the applied 0x{replacement:08x}; "
                    "this build of the game translates unchanged")
                applied[name] = 0
                return data, 0
            found.append((off, replacement))
        if isinstance(out, bytes):
            out = bytearray(out)
        for off, replacement in found:
            struct.pack_into("<I", out, off, replacement)
        written += len(found)
        log(f"pataport: {name} applied to overlay {module_name} ({len(found)} words)")
    return (bytes(out) if written else data), written


if __name__ == "__main__":
    # Self-check on synthetic images: a matching build is patched, any other is left alone.
    class Reloc:
        lo = 0x0883D000
        mem = bytearray(0x2000)

    class FakeElf:
        reloc = Reloc()

        def read_at_vaddr(self, a, n):
            o = a - self.reloc.lo
            return bytes(self.reloc.mem[o:o + n])

    elf = FakeElf()
    enc = {"bne": 5 << 26, "bnel": 0x15 << 26, "addiu": 9 << 26, "beq": 4 << 26,
           "beql": 0x14 << 26, "addu": 0x21}
    for a, k in PATCHES["patapon3-60fps"]:
        struct.pack_into("<I", elf.reloc.mem, a - Reloc.lo, enc[k] | 0x1234 if k != "addu" else enc[k])
    other = FakeElf()
    other.reloc = type("R", (), {"lo": Reloc.lo, "mem": bytearray(elf.reloc.mem)})()
    struct.pack_into("<I", other.reloc.mem, 0x0883D5DC - Reloc.lo, 9 << 26)   # not a beq
    before = bytes(other.reloc.mem)
    assert apply(other, "patapon3-60fps", lambda m: None) == 0
    assert bytes(other.reloc.mem) == before                                    # untouched
    assert apply(elf, "patapon3-60fps", lambda m: None) == 6
    assert all(elf.read_at_vaddr(a, 4) == b"\0\0\0\0" for a, _ in PATCHES["patapon3-60fps"])
    # The fixed-address form (no relocation): the words live in the PT_LOAD bytes.
    class FixedElf:
        reloc = None
        segments = [{"type": 1, "vaddr": Reloc.lo, "filesz": 0x2000, "off": 0x100}]

        def __init__(self, image):
            self.data = bytes(0x100) + image

        def read_at_vaddr(self, a, n):
            o = 0x100 + a - Reloc.lo
            return bytes(self.data[o:o + n])

    struct.pack_into("<I", other.reloc.mem, 0x0883D5DC - Reloc.lo, 4 << 26)  # a beq again
    fixed = FixedElf(bytes(other.reloc.mem))
    assert apply(fixed, "patapon3-60fps", lambda m: None) == 6
    assert all(fixed.read_at_vaddr(a, 4) == b"\0\0\0\0" for a, _ in PATCHES["patapon3-60fps"])
    # The pcmenu patch: exact-word entries, each rewritten to its replacement.
    Window = type("W", (), {"lo": 0x08804000, "mem": bytearray(0x300000)})
    main_elf = FakeElf()
    main_elf.reloc = Window()
    for addr, _want, expected, replacement in PATCHES["patapon3-pcmenu"]:
        struct.pack_into("<I", main_elf.reloc.mem, addr - Window.lo, expected)
    assert apply(main_elf, "patapon3-pcmenu", lambda m: None) == 3
    for addr, _want, _expected, replacement in PATCHES["patapon3-pcmenu"]:
        assert struct.unpack_from("<I", main_elf.reloc.mem, addr - Window.lo)[0] == replacement
    # Already-applied words re-apply cleanly (a build that re-reads patched bytes).
    again = FakeElf()
    again.reloc = Window()
    for addr, _want, _expected, replacement in PATCHES["patapon3-pcmenu"]:
        struct.pack_into("<I", again.reloc.mem, addr - Window.lo, replacement)
    assert apply(again, "patapon3-pcmenu", lambda m: None) == 3
    # A wrong word refuses the whole set.
    wrong = FakeElf()
    wrong.reloc = Window()
    struct.pack_into("<I", wrong.reloc.mem, 0x08A1A744 - Window.lo, 0x8E0508A8)
    struct.pack_into("<I", wrong.reloc.mem, 0x088BAAA8 - Window.lo, 0x8E8308A9)  # one bit off
    before = bytes(wrong.reloc.mem)
    assert apply(wrong, "patapon3-pcmenu", lambda m: None) == 0
    assert bytes(wrong.reloc.mem) == before
    # The overlay half: an MWo3 image whose load address and text size the
    # header carries; entries resolve through it and verify to the bit.
    def make_overlay(base, text_bytes):
        head = bytearray(0x40)
        head[0:4] = b"MWo3"
        struct.pack_into("<I", head, 8, base)
        struct.pack_into("<I", head, 0xC, text_bytes)
        return bytearray(head) + bytearray(text_bytes)

    title_entries = OVERLAY_PATCHES["patapon3-pcmenu"]["OL_Title.bin"]
    ov_base = 0x08ABB180
    ov = make_overlay(ov_base, 0x30000)
    for addr, expected, _replacement in title_entries:
        struct.pack_into("<I", ov, addr - ov_base, expected)
    state = {"patapon3-pcmenu": 3}
    out, n = apply_overlay(bytes(ov), "OL_Title.bin", state, lambda m: None)
    assert n == 2 and state["patapon3-pcmenu"] == 3
    for addr, _expected, replacement in title_entries:
        assert struct.unpack_from("<I", out, addr - ov_base)[0] == replacement
    # Already-applied overlay words re-apply cleanly.
    out2, n2 = apply_overlay(out, "OL_Title.bin", {"patapon3-pcmenu": 3}, lambda m: None)
    assert n2 == 2
    # A wrong word (or a non-overlay image) refuses and zeroes the set.
    bad = bytearray(out)
    struct.pack_into("<I", bad, title_entries[0][0] - ov_base, 0x2442FFFE)
    state = {"patapon3-pcmenu": 3}
    out3, n3 = apply_overlay(bytes(bad), "OL_Title.bin", state, lambda m: None)
    assert n3 == 0 and state["patapon3-pcmenu"] == 0
    state = {"patapon3-pcmenu": 3}
    _out4, n4 = apply_overlay(b"\0" * 0x1000, "OL_Title.bin", state, lambda m: None)
    assert n4 == 0 and state["patapon3-pcmenu"] == 0
    # A module the set does not name is returned untouched.
    state = {"patapon3-pcmenu": 3}
    _out5, n5 = apply_overlay(bytes(ov), "psmf.prx", state, lambda m: None)
    assert n5 == 0 and state["patapon3-pcmenu"] == 3
    print("pataport_patches selftest: OK")
