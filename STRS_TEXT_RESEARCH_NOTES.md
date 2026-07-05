# STRS (GW2 text/string table) reverse-engineering notes — checkpoint

Goal: decode a `Gw2.dat` **string table** MFT entry (magic `"strs"`) down to
the actual localized text per entry, for `compressed_2902.bin`,
`compressed_2926.bin`, `compressed_798326.bin`.

Binary: `Gw2-64-disable-aslr.exe`, imagebase `0x140000000`.

## Pipeline (confirmed)

```
Gw2.dat MFT entry -> CmpDecompress Method 0            [solved, see cmp_decompress_method0.py]
  -> "strs" container bytes                              (confirmed: all 3 samples start with
                                                            magic "strs" after CmpDecompress)
  -> sequence of string records                           [container framing: SOLVED, byte-exact]
  -> per-record text decode                                [PARTIALLY solved -- see below]
```

Decompress + parse with the tools already in this repo:
```sh
python cmp_decompress_method0.py compressed_2926.bin out_2926.bin
python strs_decode.py out_2926.bin
```

## Container format (byte-exact, verified against all 3 samples)

```
offset 0-3 : "strs"           magic == TEXT_STRINGS_SIGNATURE == dword 0x73727473
offset 4.. : records, back-to-back, no count field, no padding between them
```

Each record ("`StringHeader`" -- the literal name, recovered from an
assert-string stringification: `"header->bytes >= sizeof(StringHeader)"` /
`"header->rangeBits <= 16"`, both found in `Engine\Text\TextSync.cpp`):

```
u16 bytes       total record length INCLUDING this 6-byte header
                (the NEXT record starts at record_start + bytes)
u16 baseChar    "base" character offset used by the bit-packed decoder
u8  rangeBits   bits-per-symbol for the bit-packed decoder (assert: <= 16)
u8  _pad        unused/reserved (not read by anything found so far)
...payload, exactly (bytes - 6) bytes...
```

Verified by walking every record in all three sample files and checking the
running offset lands exactly on the file end (mod a 2-byte trailer, see
below):

| file | records | bytes consumed | file size |
|---|---|---|---|
| `out_2902.bin` | 1024 | 25897 | 25899 |
| `out_2926.bin` | 1024 | 67605 | 67607 |
| `out_798326.bin` | 1024 | 26963 | 26965 |

The constant 2-byte shortfall in all three is a trailing `u16` (looks like
a 0x0000 terminator/count field after the last record) -- not yet
identified, harmless to ignore for decoding purposes.

## Functions located (Text engine, all in the `sub_1410C*` address range)

Source file paths below are recovered verbatim from embedded C-assert
strings (stringified expressions + `__FILE__`), not from any symbol/PDB --
this binary has no application-level symbols, only CRT ones.

| address | file (from asserts) | role |
|---|---|---|
| `sub_1410CCA80` | `Engine\Text\TextCache.cpp` | parses a whole `strs` blob into a per-language record cache (validates magic, walks records, calls the store-in-hash helper below per record) |
| `sub_1410CCF30` | (same) | hash-insert: caches the **raw, still-undecoded** record bytes (header + payload) keyed by (language, id) |
| `sub_1410CC930` | `Engine\Text\textint.h` | periodic tick: evicts cached raw records after 60s (`0xEA60` ms) unused, re-parses pending loads |
| `sub_1410CE830` | `Engine\Text\TextSync.cpp` | network string-pack sync path; re-validates magic + `StringHeader` invariants per record before caching |
| `sub_1410CFC50` | `Engine\Text\TextDecode.cpp` | **the actual per-record text decoder** (see below) |
| `sub_1410CFF60` | `Engine\Text\TextDecode.cpp` | bracket/tag cross-reference resolver; the only caller of `sub_1410CFC50` anywhere in the binary |
| `sub_140D9F4E0` | `Arena\Services\Crypt\CptRc4.cpp` | RC4 key-scheduling (KSA) -- builds a 256-byte S-box from a key |
| `sub_140D9F630` | (same file, unnamed in asserts) | mixes an 8-byte seed into a 20-byte key via a round function (SHA-1/MD4-style: `rol` by 5/30, similar magic constants) -- produces the key fed to the RC4 KSA above |

## `sub_1410CFC50` -- the text decoder (fully decompiled, two paths)

Signature (as recovered): `sub_1410CFC50(ctx, header /* StringHeader* */, decodeId, keySeed /* a5 */)`.

Early-out checks (byte-exact against disasm at `0x1410cfc67`-`0x1410cfc8e`):
```
if header == NULL or header->bytes < 6:              return "" (error)
if header->baseChar != 0 and keySeed == 0:            return "" (error)
```

Then it fills a working buffer from the payload -- either a straight
`memcpy` (`keySeed == 0`) or, when `keySeed != 0`, by constructing an RC4
cipher (`sub_140D9F630` to derive a 20-byte key from the 8-byte `keySeed`,
then `sub_140D9F4E0` to KSA-schedule it) and running the payload through
it. **Only after that** does it interpret the (possibly RC4-transformed)
buffer as text, taking one of two paths:

### Path 1 -- raw UTF-16 (`header->baseChar == 0 && header->rangeBits == 16 && payloadBytes even`)

Straight `memcpy`/word-copy of the payload as UTF-16LE, no further
transform. **CONFIRMED CORRECT** -- `strs_decode.py` reproduces this
exactly and it decodes real, grammatical text:

```
'Las mascotas se mueven más rápido.'
'Reduce la recarga de habilidades de espada, mandoble y lanza.'
'Otorga celeridad a ti y a tu aliado cuando lo reanimas. Se ha aumentado la velocidad a la que reanimas.'
'一根荆棘被携带至遥远边境，与世界连结。'
'提兹拉克肩章'
```

(Spanish skill tooltips from `out_2926.bin`, Chinese item/achievement names from `out_798326.bin`)

### Path 2 -- bit-packed ("compact") text (`header->baseChar != 0`, or `rangeBits != 16`)

Disassembled precisely at `0x1410cfe05`-`0x1410cfecb`. Each output
character: refill a bit accumulator from payload bytes (LSB-first, 8 bits
at a time, byte-pointer only advances, never rewinds), then:

```
symbol = accumulator & ((1 << rangeBits) - 1)
accumulator >>= rangeBits
if symbol == 0:        char = 0                                    (NUL / end of string)
elif symbol < 0x20:     char = SPECIAL_TABLE[symbol - 1]            (fixed 31-entry table, see below)
else:                   char = symbol + header->baseChar - 32
```

`SPECIAL_TABLE` is a literal, non-language-specific 31-entry UTF-16 array
embedded in the .exe at `0x1420F3100` (only ever read by this one
function -- confirmed via `xrefs_to`):

```
"0123456strnum()[]<>%#/:-'\" ,.!\n"
```

(digits 0-6, then `s t r n u m ( ) [ ] < > % # / : - ' " space , . ! \n` --
almost certainly the common markup/punctuation glyphs used in GW2's
in-text tags like `<c=@flavor>`, `%target%`, etc., factored out so every
localized string doesn't need its own bits for them.)

**This part (framing + formula + table) is confirmed exactly right at the
disassembly level.** What is *not* solved: this whole path is only ever
reachable, per the two early-out checks above, when `header->baseChar != 0`
**and** a non-zero `keySeed` is supplied -- and that `keySeed` triggers an
RC4 transform of the payload *before* the bit-unpack math above ever runs.
`strs_decode.py`'s `_decode_packed()` currently skips the RC4 step
entirely (treats the raw payload as if it were already the post-RC4
buffer), which is why it produces structurally well-formed but
semantically wrong output (garbage/control characters) for every
`baseChar != 0` record in the samples -- e.g. `out_2902.bin` is **100%**
`baseChar != 0` records (all 1024), so none of it decodes correctly yet.

## The missing piece: where does `keySeed` come from?

Traced the sole caller, `sub_1410CFF60` (`TextDecode.cpp`):

```c
v37 = 0;
sub_1410D4500(sourceText, sourceTextEnd, decodeId, &v37);   // TextParser.cpp: scans an
                                                              // *already-decoded parent string's*
                                                              // raw markup for an inline
                                                              // reference/key near `decodeId`
keySeed = v37;
if (!v37 && context->overrideTableCount) {
    // fall back to a small hash table (ctx+0x200) keyed by decodeId,
    // populated elsewhere (not yet located) -- if decodeId isn't in it,
    // keySeed stays 0
}
sub_1410CFC50(ctx, ctx2, header, keySeed);
```

So `keySeed` is genuinely **runtime/context state** — either parsed out of
the *calling* string's own markup text (a cross-reference syntax we
haven't identified, e.g. something like an inline `[id,key]` tag) or from
a small per-context override table populated somewhere we haven't traced.
Neither source is present in a standalone `compressed_*.bin` MFT entry —
this can't be resolved from static analysis of one string-table blob
alone.

### Working hypothesis for what `baseChar != 0` records actually *are*

Given `sub_1410CFC50` is the **only** function in the whole binary that
reads the compact-alphabet table, and is **only ever called** from the
bracket/cross-reference resolver (never from the plain "get this string
for display" path), `baseChar != 0` records are probably not ordinary
dialogue/tooltip text at all -- they look like a separate sub-category
used for short, cross-referenced fragments (e.g. reusable tokens plugged
into other strings via `[decodeId]`-style tags), deliberately
RC4-obfuscated per reference so they can't be trivially data-mined out of
context. That would explain why `compressed_2902.bin` (short strings,
`rangeBits` only 6-7, i.e. tiny alphabets) is *entirely* made of this
record type, while `compressed_2926.bin`/`compressed_798326.bin` (normal
skill/item text) are overwhelmingly plain UTF-16 with only a handful of
these mixed in.

## Session 2: full trace of where `keySeed`/`decodeId` actually come from — RESOLVED (structurally), and it's a dead end for single-file static analysis

Traced the whole chain one level further:

- `sub_1410D4610`/`sub_1410D4800`/`sub_1410D4C90` (all `TextParser.cpp`) are
  a **recursive-descent parser over an already-decoded parent string's own
  token stream**. The "string" isn't plain UTF-16 at this layer -- it's a
  compiled markup byte-code: most values are literal UTF-16 chars, but a
  reserved high range (`WORD_BIT_MORE = 0x8000`, `WORD_VALUE_BASE = 0x100`)
  encodes **variable-length integers** (continuation-bit VLQ, base ~32512
  per digit) inline in the token stream, and specific token opcodes
  (`opcode - 269 <= 5`, recursing back into `sub_1410D4610`) are tag/bracket
  markers that carry **two VLQ integers**: a tag/reference id (parsed by
  `sub_1410D4800`) and an associated numeric value (parsed by
  `sub_1410D4C90`). When the id being searched for (`decodeId`) matches the
  tag's id, its value becomes `keySeed`.
  - In plain language: this is the compiled form of GW2's bracket tags
    (the same family as the `<c=@reminder>...</c>` and `%target%`-style
    markup visible verbatim in several of our raw-UTF16 samples). A
    *parent* string's markup can embed something like `[tag id=1234
    value=5678]`, and if some code later asks "decode reference 1234",
    the engine walks the parent string's own token stream looking for a
    tag whose id is 1234, and reuses that tag's *own* companion value
    (5678) as the RC4 `keySeed` for a *separate* string-table record.
- Traced `sub_1410CFF60`'s callers (`xrefs_to`): both are the same drain
  loop, `sub_1410CDEE0`, which pulls entries off a **global pending-request
  queue** (`dword_142852640`) populated by other, unrelated engine code
  (UI/rendering, not the text system) calling something like
  "please resolve reference `decodeId` in context `contextHandle`". This
  queue is filled by code entirely outside `Engine\Text\*`.

**Conclusion**: `decodeId` and `keySeed` are never present in a
string-table MFT entry at all, in any form. They are supplied at runtime
by whichever UI/game-logic code decided it needs to resolve a
cross-reference, and `keySeed`'s actual value is whatever numeric literal
some *other*, unrelated parent string's compiled markup happens to embed
next to a matching tag id. There is no algorithmic derivation to reverse
here — it's a **plaintext cross-reference already sitting in a different
piece of text somewhere else in the game's full string corpus**, not a
secret and not a function of anything inside `compressed_2902.bin` /
`compressed_2926.bin` / `compressed_798326.bin` in isolation.

### What would actually be needed to finish this

Not more IDA tracing -- the algorithm side is now fully understood end to
end (framing, RC4 KSA, VLQ tag parsing). What's missing is *data*:

1. A way to know each `baseChar != 0` record's own numeric string ID (not
   stored in the record itself -- likely assigned by position in a
   separate per-file ID index inside `Gw2.dat`, not reverse-engineered
   here).
2. A **corpus-wide search** across every other decompressed `strs` table
   in the game (not just these 3 samples) for a markup tag whose
   embedded id matches that string ID, to recover its companion
   `keySeed` value.
3. With both in hand, finishing `_decode_packed()` is mechanical: derive
   the RC4 keystream (`sub_140D9F630`'s 20-byte key mix, fed into
   `sub_140D9F4E0`'s KSA, then standard RC4 PRGA) and XOR it into the
   payload *before* the existing (already-correct) bit-unpack math runs.

Given (1) and (2) both require data well beyond the three sample files
provided, this is not resolvable from static analysis of these files
alone -- it would need either the full extracted `Gw2.dat` string-table
corpus to search across, or dynamic instrumentation of a live client
(breakpoint on `sub_1410CFC50` at `0x1410CFC50`, log every
`(decodeId, keySeed, header->bytes)` tuple it's actually called with).

## Session 3: live debugging attempt (IDA debugger attached to the running client)

Goal: capture real `(decodeId, keySeed, StringHeader)` tuples passed into
`sub_1410CFC50` during actual gameplay, to nail down the RC4 pipeline and
verify it byte-exact against the game's own decoded output.

### Method

IDA's Win32 debugger (`ida_dbg` Python API) was used to attach to the
already-running, already-logged-in client (`Gw2-64-disable-aslr.exe`,
ASLR disabled so the static IDB addresses match the live process exactly)
and set a breakpoint at `0x1410CFC50` (the function's entry). A
`DBG_Hooks` callback read the x64 fastcall argument registers
(`RCX/RDX/R8/R9` = a1-a4, `[RSP+0x28]` = the 5th stack argument = `keySeed`)
plus the `StringHeader` (`bytes/baseChar/rangeBits`) pointed to by `R9`,
logged them to `strs_rc4_key_log.jsonl`, armed a one-shot breakpoint at the
return address to also capture the real decoded output (`RAX`, then read
as a UTF-16 string), and auto-resumed the process (`continue_process()`)
so the game kept running with only a brief pause per hit.

**Important lesson learned**: launching the process directly under IDA's
debugger (`start_process`) and leaving the breakpoint armed through the
entire loading sequence caused the client to error out/crash twice.
Attaching to an *already fully loaded, already in-instance* process
(`attach_process` on the running PID) instead was stable and produced
clean data with no crash. If repeating this, always attach late rather
than launching under the debugger.

### Results

- **Raw-UTF16 path: fully reconfirmed live**, multiple times, byte-exact:
  captured payload `2500 6e00 7500 6d00 3100 2500` (baseChar=0,
  rangeBits=16) decoded to `"%num1%"` by this script's logic, and the
  game's own return value (captured via the return-address breakpoint)
  was also exactly `"%num1%"`. Same for `"Achievements"` and
  `"Squad ready!"`. This also validates the *capture methodology itself*
  (reading `RAX` after return really does yield the real decoded
  string) -- so the technique is sound in principle.
- **RC4 path (`baseChar != 0`): real, non-zero `keySeed` values were
  captured from live gameplay**, confirming the path genuinely executes
  during normal play (not just theoretical). Example captured tuples
  (decodeId, header bytes/baseChar/rangeBits, keySeed, payload):
  - `decodeId=44576, bytes=12, baseChar=86, rangeBits=6, keySeed=0xb830,
    payload=c7ffa52ee2c7` -- keySeed reproduced identically across 3
    separate hits of the same decodeId (high confidence this one is a
    real, stable read).
  - `decodeId=43462, bytes=20, baseChar=66, rangeBits=7, keySeed=0x0,
    payload=859c...` -- **keySeed=0 correctly triggered the documented
    early-exit** (`if baseChar!=0 && keySeed==0: return ""`); the
    caller's fallback display was `"[null]"`, which lines up exactly
    with the early-exit path being taken. This is a clean confirmation
    of that branch's behavior.
- **Full RC4 pipeline implemented and tested, but NOT verified
  byte-exact**: with `sub_140D9F8C0` now decompiled (the RC4 object's
  vtable slot-0 method), the encrypt/decrypt step is confirmed to be
  textbook RC4 PRGA + XOR (`i=(i+1)&0xFF; j=(j+S[i])&0xFF; swap(S[i],S[j]);
  out = in ^ S[(S[i]+S[j])&0xFF]`), keyed by a 20-byte value produced by
  cycling the 8 raw bytes of `keySeed` and running them through the
  one-round SHA-1-like mixer in `sub_140D9F630`. Running this full
  pipeline (`rc4_key_test.py`, not committed -- ad hoc verification
  script) against the captured `keySeed=0xb830` /
  `keySeed=0x6b7ab82c` samples produced output that is *plausibly
  text-like for `rangeBits=6` records* (e.g. `'2di1</rp'`,
  `'f!0 bbep'` -- lowercase letters/punctuation, no control characters)
  but this is inconclusive: `rangeBits=6` intrinsically maps most
  symbols into a narrow, mostly-printable character band regardless of
  whether the bytes were decrypted correctly first, so "looks like
  text" isn't proof. The two `rangeBits=7` samples where a live
  ground-truth return value was also captured did **not** match:
  - `decodeId=257809, keySeed=0x9ae7f6bc` decoded (with this pipeline)
    to control-character noise, while the game's own captured return
    value was `"슓¥"` -- characters far outside the
    `symbol+baseChar-32` range this header's fields imply, which is a
    contradiction the pipeline as implemented cannot produce no matter
    the key. Either the key-derivation constants were mistranscribed
    somewhere, or the true `keySeed` for that specific call wasn't
    reliably captured (repeated hits of the *same* decodeId in this
    session showed the captured `keySeed` value change between calls in
    a few cases, e.g. `decodeId=257809` read as `0x9ae7f6bc` once and
    `0x295ecb0` -- suspiciously equal to the constant `ctx2_a2` context
    pointer -- on later hits, which points at an occasional bad stack
    read rather than a genuinely varying key).

**Net conclusion**: dynamic tracing is possible and the mechanism/tooling
works (proven by the raw-UTF16 round-trips), but this session's captures
were not clean enough to close out the RC4 key derivation with certainty.
Given two client crashes were caused by the `start_process`-under-debugger
approach and IDA itself crashed once during a `detach_process` call at the
end of this session, further live-debugging iterations should be weighed
against that instability -- if repeated, prefer very short attach windows
(attach, arm the breakpoint, capture a handful of hits, remove the
breakpoint and detach immediately) over a long-lived 200-hit session.

## Files in this project directory (this session's additions)

- `strs_decode.py` — container parser + decoder. Raw-UTF16 path is
  DONE and byte-exact verified (both statically and now live, against
  real game output). Bit-packed path is framing-correct but not
  byte-exact (missing a verified RC4 key derivation, see Session 3
  above) -- output for those records is explicitly flagged `[packed*]`
  in the CLI output.
- `out_2902.bin`, `out_2926.bin`, `out_798326.bin` — CmpDecompress
  Method0 output of the user-supplied `compressed_2902.bin` /
  `compressed_2926.bin` / `compressed_798326.bin` samples (produced via
  the existing `cmp_decompress_method0.py`).
- `strs_rc4_key_log.jsonl` — raw live-capture log from the Session 3
  debugging run (264 lines: register/header dumps per breakpoint hit,
  plus correlated ground-truth decoded strings where the return-address
  breakpoint also fired).
