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

## Next steps

1. Locate where the per-context override table (`ctx+0x200`-ish, checked
   in `sub_1410CFF60`) gets populated -- if it's seeded from something
   static (e.g. a fixed per-build secret, or derived from the numeric
   string ID itself) rather than from live network state, the key could
   still be recovered statically.
2. Reverse `sub_1410D4500`/`sub_1410D4610` (`TextParser.cpp`) far enough to
   learn the inline reference syntax it scans for in a parent string's
   markup -- this is the more likely real source of `keySeed` for normal
   (non-override) cross-references.
3. Failing both of the above, dynamic analysis (breakpoint on
   `sub_1410CFC50` at `0x1410CFC50` in a live client, log `(decodeId,
   keySeed, header->bytes)` tuples across many strings) would settle it
   quickly and is probably the fastest path to finishing this.
4. Once the key derivation is known, finish `_decode_packed()` in
   `strs_decode.py`: derive the RC4 keystream (`sub_140D9F630` then
   `sub_140D9F4E0`'s KSA + standard RC4 PRGA), XOR it into the payload
   *before* running the existing (already-correct) bit-unpack math.

## Files in this project directory (this session's additions)

- `strs_decode.py` — container parser + decoder. Raw-UTF16 path is
  DONE and byte-exact verified. Bit-packed path is framing-correct but
  not byte-exact (missing RC4 step, see above) -- output for those
  records is explicitly flagged `[packed*]` in the CLI output.
- `out_2902.bin`, `out_2926.bin`, `out_798326.bin` — CmpDecompress
  Method0 output of the user-supplied `compressed_2902.bin` /
  `compressed_2926.bin` / `compressed_798326.bin` samples (produced via
  the existing `cmp_decompress_method0.py`).
