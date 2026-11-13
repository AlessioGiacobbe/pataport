# Changing a game: how a change is discovered, made, and kept safe

This document is the working playbook for Pataport's changes to a game's own
code and data — the kind of change where the result must live inside the
guest, built from the game's own widgets and files, never as an external
overlay. It is written from the first worked example (Patapon 3's PC settings
row inside the game's own OPTION screen), generalized so the next change
starts from the recipe instead of rediscovering it.

The standing rules are Sections 3–6 of [AGENTS.md](../AGENTS.md): no game data
in the repository, fail-closed verification against the stock bytes,
all-or-nothing patches, and a test that fails before the change. Everything
below operates inside those rules.

## The four channels

Every change to a game is one (or more) of four channels. Choose per piece of
the feature, not per feature.

1. **Code patch** — rewrite instructions of the main executable (or a module)
   *before* analysis and translation. For words that become NOPs, with the
   expected instruction kind (and, for a call, its callee) verified first.
   Tool: `tools/pataport_patches.py`, selected by `--pataport-patch=NAME` and
   applied by codegen before analysis.
2. **Data patch** — rewritten bytes of a disc file, served at run time only
   after the whole stock file hashes to a digest recorded at build time.
   Tool: `tools/pataport_data_patches.py` computes the patch from the user's
   disc at build time; codegen emits a descriptor registration; the runtime
   serves sparse byte ranges over the stock bytes (the enhancement data-file
   contract, `docs/ENHANCEMENT_CONTRACT.md` section 9.6).
3. **Bridge** — a guest value the host reads (and may write back) at a fixed
   guest address, polled per frame. The guest side is the game's own storage
   (never invented host-only state); the host side is a small runtime unit.
   Example: `src/rt/pc_settings.c`.
4. **Guest-RAM rewrite** — the same strings rewritten *in guest memory* after
   the game itself decrypts them. Needed when the bytes a data patch reaches
   are not the bytes the game renders: a game may load its own encrypted
   archive (Patapon 3's `DATAMS.BND`) and decrypt it into RAM, so no disc-file
   patch can touch what ends up on screen. The generated code registers the
   stock and replacement bytes (`pataport_patches.py` `PCMENU_TEXT`); a
   runtime scanner finds the stock bytes whole — a `memchr` for the first
   byte, then a full compare on a UTF-16 boundary — and replaces them in
   place, NUL-terminated, because the replacement is shorter. The scan repeats
   on a vblank window forever: the game may re-decrypt the archive on a later
   pass and restore the stock bytes. Fail-closed rules: a replacement longer
   than its stock string refuses the whole registration; a match at an odd
   byte offset is skipped (UTF-16 alignment); nothing but the matched range is
   written; every rewrite logs itself. Example: the text half of
   `src/rt/pc_settings.c`.

A feature usually needs several channels: relabel text (data, and the
guest-RAM rewrite when the game decrypts its own archive), detach or retarget
old behavior (code), carry the value to the host (bridge).

## Locating the thing you want to change

Work from the game that is already running in Pataport, not from scratch. The
best sources, in the order they usually pay off:

- **The build's generated C.** A built package carries the generated
  translation of every function (`experimental-<id>_recomp_N.c`, overlays as
  `nk_lib_<hash>_N.c`) plus the function registry
  (`experimental-<id>_recomp.c`, one `{address, size, name}` row per
  function). It is instruction-level but mechanically reliable for control
  flow, and text-searchable. This is usually faster than a decompiler.
- **The game's text archives.** Menus are text-driven: find the on-screen
  string in a `.msg` file (UTF-16LE, `{count, unk, offsets[]}`), then walk
  outwards through the archive chain it sits in. BND v5 containers
  (`BND\0` version 5) hold members raw or gzip-compressed, with a folder tree
  whose stored folder names carry trailing slashes. Archive chains nest
  deeply (Patapon 3's Option text sits five levels down with one gzip layer).
  A member's "CRC" block field 0 is a name hash, not a content hash — content
  can be rewritten without touching entry tables when sizes are kept exact.
- **Code, from the text.** Localize ids are `(group << 16) | index` and appear
  as immediates in the code that builds the screen. Searching the generated C
  for the id (e.g. `0x40011`) finds the builder; the builder's neighbours give
  the row handlers, the value storage and the commit path.
- **Overlays.** Metrowerks MWo3 overlays (`Overlay/*.bin`) load at one shared
  address and swap at run time; a 0x40-byte header gives the load address and
  section sizes. Import the raw file at its load address in Ghidra, or rely on
  the generated C, which has the overlay's functions already.
- **Ghidra, for what the generated C cannot answer.** Headless via
  `tools/ghidra_headless.py` (Ghidra 12: probe scripts are Java, class name =
  file name; compile errors surface as ClassNotFoundException — grep the output
  for `error:`). Import the decrypted EBOOT and any overlays at their load
  addresses. Use decompile/refs probes for cross-references the text search
  misses, and fan out subagents with **project copies** (project locks are
  per-directory).

## Proving the change is safe before designing it

The step that matters most, and the one that is easy to skip: **find every
consumer of the thing you repurpose.** A byte that looks like dead UI may gate
install behavior, network flows, or save layout elsewhere. Trace the value
from its storage through every read (including reads that reach it through a
cached copy or a function argument, which text search misses). Then decide:

- repurpose in place when the consumers are detached (a verified NOP of the
  one call that hands the value to the old behavior);
- or pick a different anchor.

Write the consumer list down (issue or pull request): it is the evidence that
the patch is safe, and the first thing a reviewer needs.

## Designing the patch set

- **Same-size beats relocation.** If every rewritten container member keeps
  its exact stock size, no parent's entry tables change and a whole data patch
  reduces to one contiguous byte range. Gzip members can be recompressed to an
  exact size by padding through the gzip header's FEXTRA field (RFC 1952);
  deflate-level padding cannot reach every remainder, and padding must never
  follow the final deflate block, which the member's CRC32 and ISIZE
  immediately follow. When the game's own packer beat zlib -9 (it happens by
  about one percent), use zopfli.
- **String slots are NUL-terminated.** A replacement may be shorter than its
  stock slot: write the text, the NUL, and pad after the terminator. Note the
  game's own line-break convention (`\n` as a literal two-character sequence
  in Patapon 3's .msg files).
- **Code patches verify, then change.** Every patched word is checked against
  its expected instruction kind (and callee, for a call) at apply time; any
  mismatch skips the whole patch and the game translates unchanged.
- **Data patches verify at run time.** The build records the stock file's
  SHA-256; the runtime hashes the file it is about to serve and stops the run
  on any mismatch. Patch ranges must stay within the stock bytes.
- **Bridges reuse the game's own storage.** The guest address chain is
  discovered once (from the Option screen's own reads) and guarded: a pointer
  outside plausible guest RAM means "not yet", and the poll waits. The host
  side registers its applier at init rather than hard-linking another layer,
  so tests can run without the GPU backend.

## Building it in

- The build computes data patches **from the user's disc, at build time**. The
  repository carries the spec (which strings, which archive, which disc id);
  never the bytes. A disc the spec was not made for skips the whole slice with
  the game building unchanged.
- New codegen options ride `CODEGEN_USER_ARGS`, which the package cache key
  carries: adding one invalidates the cache, and the next build re-runs
  codegen. Expect that cost once per patch-set change.
- Every channel gets a failing-before test: synthetic containers for the data
  tool (build a miniature of the real archive chain in the test), the module
  selftest for runtime seams (`tests/native/hle_thread_selftest.c`), and the
  patches selftest for code patches.

## The worked example

Patapon 3's PC settings row, end to end (each step's tool lives in the
repository) — the first slice, now superseded by the second:

1. The Option screen's text found in `titlemsg.msg`, five container levels
   inside `DATA_CMN.BND` (the chain walked with the data-patch tool's reader).
2. The row identified as the Game System page's install-data toggle: its
   localize id (0x40011) found as a builder immediate; the value byte, the
   toggle handler and the commit path traced in the generated C.
3. Every consumer of the committed byte traced (one real consumer: the
   install-data preference handed to the VFS; everything else is the row's own
   UI and save round-trip).
4. Three pieces: `patapon3-pcsettings` code patch (one verified NOP detaches
   the consumer); the text patch (ids 17 and 18 relabeled, same-size, one
   264 KB contiguous range of the container); the bridge (`pc_settings.c`,
   reading the byte per vblank and driving the render scale through an
   applier the GPU backend registers at init).
5. Tests at every layer, and the runtime refuses to serve any patched byte
   unless the stock file hashes to the build-time digest.

## The second worked example: a whole screen (patapon3-pcmenu)

The first slice squatted on one row of the Game System page. The real goal was
a PC Settings screen of its own, and the OPTION screen's pages are compiled
code — its constructor builds six fixed page objects with no data-driven page
table, and the screen object's allocation is exactly the size those six pages
fill. So a literal new page would need injected guest code; the screen has to
be a repurposed native page. The study that decided it:

1. **The screen's structure, from the generated C.** The root 6-row list is
   page1 (rows labelled by text ids 0x40002..0x4000c, activated through a
   jump table); each page is a constructor + a state machine over a small cfg
   block; the Chat Settings page already carries the ◄value► cycle primitive
   with a 3-entry label table and a Reset row with its own confirm dialog.
2. **The donor page's value, end to end.** The chat-mode word's persistence
   was already fully mapped (load, apply, snapshot, save-on-change, all in
   the OPTION screen's own code), which is exactly what a repurposed setting
   needs: the quality value rides the game's own save flow untouched.
3. **Every consumer traced and detached.** The retired chat-mode readers are
   twelve display-only sites (the hideout chat-log layout selectors and the
   chat-message pipeline); each is rewritten to load the stock default
   deterministically — three in the main module, ten in the OL_Azito overlay
   (two of them being the stores their loads feed). This is the first use of
   the code-patch channel on overlays: `translate_runtime_module` applies the
   overlay words before a module's identity is taken and writes the patched
   image back, so the translated code and the bytes the runtime loads at the
   overlay's fixed address agree. The cursor's path is clamped to the quality
   and Reset rows, so the retired colour rows are never selected.
4. **The pieces.** `patapon3-pcmenu`: the reader detachments (exact-word
   patches, verified to the bit) and the cursor clamp; the data patch (fourteen
   strings in `titlemsg.msg` — the root row, the page title, the quality row
   with ◄1080p / 2K / 4K► labels, the blanked colour rows, the Reset texts);
   `PCMENU_TEXT`, the same strings anchored in guest RAM (a short stock string
   like "System" is anchored to the bytes that follow it in the archive, so a
   lookalike elsewhere in RAM can never be rewritten); and the bridge, which
   maps the record word 0/1/2 onto the render scale 4x/6x/8x and refuses any
   value outside the cycle.
5. What made it safe: the exact-word patches refused to apply until the
   overlay offset mapping was corrected (the first offset guess failed the
   verification, as designed); every relabel is shorter than its stock slot;
   and a value the game's own cycle cannot produce changes nothing.

## The third worked example: an easter egg's one honest trigger

Pataport plays a short clip of its own (embedded from `assets/easter_egg/`,
maintainer-authored audio, decoded by the vendored `stb_vorbis` through a
one-translation-unit bridge) once per run, when the game's boot reaches its
title overlay. The clip is presentation only — it never touches guest memory
— but *when* to play it had to be an observable, not a guess, and the boot's
pre-title screens are the hardest kind of thing to observe:

1. **What the boot actually does.** A boot run with `SR_IOLOG=1` and
   `SR_ROUTE_LEARN=1` showed the whole pre-title phase opening exactly three
   files (`DATA_CMN.BND` and the two DATAMS archives, before any screen), so
   a disc-file-open trigger cannot exist; the screens carry no message
   dialog; and the screens' pixel signatures change on their own schedule
   without any runtime event attached.
2. **What the runtime observes on its own.** The phase ends when the title
   code overlay binds: `sr_module_code_sync_overlays` reports every bind by
   name, and that report is a fact of the runtime, not of the game. The egg
   registers the overlay's name (`pataport_patches.py` `EGG_TITLE_OVERLAY`,
   an overlay file name — a fact about the game's files, like the data
   patch's disc paths — never a code address) and fires once when the
   runtime reports it bound.
3. **What the bounded search for a screen-state word found.** The game reads
   the controller through one central poller (the EBOOT's only
   `sceCtrlReadBufferPositive` import call site), whose result feeds a
   per-frame callback registry whose nodes are reached by computed address —
   Ghidra establishes no static references into them, and the boot screens'
   state lives behind that dispatch. No single "this screen is showing" word
   surfaced within the bounded step, so the egg ships with the overlay-bind
   trigger and the word hunt stays open; a follow-up starts from the
   installed Ghidra 12.1 toolchain and the callback-registry lead.
4. **What made it safe:** the clip is decoded fail-closed at registration
   (a clip that does not decode logs once and the egg stays silent); the
   player registration is a seam the SDL-free tests replace with a recorder;
   the trigger fires once per run no matter how often the game re-binds the
   same overlay; `SR_EASTER_EGG=0` silences it whole; and its stream obeys
   the same environment guards and the M-key mute as the game's own audio.
   Nothing in the authentic package changes: the generated code gains two
   registration calls inside the applied pcmenu gate, and nothing else.
