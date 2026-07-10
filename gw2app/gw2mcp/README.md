# gw2mcp

An MCP server exposing the reverse-engineered Guild Wars 2 archive tooling
(from `gw2browser` + `minieditor`) as tools Claude can call directly.

The heavy lifting stays in C++ — a single headless CLI, `native/gw2dat_cli.exe`,
does all MFT parsing, ANet **Method0** decompression, **ATEX -> PNG** decoding,
and **packfile** template parsing. A thin Python server (`server/server.py`)
just shells out to it and returns the JSON. This folder is self-contained: the
needed C++ sources/headers are vendored under `native/`.

```
gw2mcp/
├─ native/            # headless C++ CLI — all the real work
│  ├─ main.cpp        # command dispatch + JSON output
│  ├─ src/            # gw2dat.cpp (MFT), BinaryParser.cpp (packfile engine)
│  ├─ include/        # gw2dat.h, cmp_decompress_method0.hpp, gw2_atex.hpp, ...
│  ├─ third_party/    # nlohmann/json, stb_image_write
│  ├─ build.ps1       # one-shot MinGW build  → gw2dat_cli.exe
│  └─ CMakeLists.txt  # alternative CMake build
├─ server/
│  ├─ server.py       # FastMCP server (7 tools)
│  └─ requirements.txt
├─ templates/
│  └─ gw2_packfile.json  # ArenaNet PF struct registry (fileTypes + types)
└─ .mcp.json          # ready-to-use MCP server config
```

## Setup (once)

1. **Build the native CLI** (needs MinGW-w64 g++ with C++20):
   ```powershell
   powershell -ExecutionPolicy Bypass -File native\build.ps1
   ```
   This produces `native/gw2dat_cli.exe`, statically linked (no MinGW DLLs
   needed at runtime).

2. **Install the Python dep**:
   ```powershell
   python -m pip install -r server\requirements.txt
   ```

3. **Register the server** with Claude. Either point Claude at this folder's
   `.mcp.json`, or merge its `gw2mcp` entry into your project `.mcp.json`.
   Adjust the absolute path in `.mcp.json` if you move this folder.

Verify without Claude:
```powershell
python server\server.py --selftest
```

## Archive locations

Defaults (overridable via the `dat_path` tool argument or the
`GW2_DAT_PATH` / `GW2_LOCAL_DAT_PATH` env vars set in `.mcp.json`):

- **Gw2.dat**  — `C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat`
- **Local.dat** — `%APPDATA%\Guild Wars 2\Local.dat` (pass `local=True`)

## Tools

| Tool | What it does |
|------|--------------|
| `gw2_info` | Archive header + MFT summary (version, size, entry counts). |
| `gw2_list_entries` | Paginated MFT entries (offset/size/compression flag/CRC). |
| `gw2_lookup` | Map between fileId ⇄ baseId ⇄ MFT index; suffix search. |
| `gw2_resolve` | Show how one id decodes as **both** baseId and fileId (disambiguate). |
| `gw2_sniff` | Decompress an entry; report type + magic + PF containerType. |
| `gw2_extract` | Decompress an entry and write the raw file bytes to disk. |
| `gw2_decode_texture` | ATEX-family → RGBA **PNG** (Method0 + inflate + BCn/3DCX). |
| `gw2_parse_packfile` | Packfile → structured field tree via the JSON template. |
| `gw2_decode_strs` | 'strs' string-table → individual string records (text). |

Selection is by `index`/`base_id` (the physical MFT index — same thing) or
`file_id` (game logical id, resolved via the file-id table). Detected types:
`packfile`, `texture`, `strs`, `dds`, `riff`, `png`, `jpeg`, `binary`; for a
packfile, `containerType` gives the payload fourcc (MODL/AMAT/ASND/ABNK/...).

### The two id namespaces (important)

GW2 ids live in **two namespaces that collide**:

- **baseId** — the physical MFT entry index. Used directly (`base_id`/`index`).
- **fileId** — the game's logical id. Resolved via a table to a baseId.

The *same number* is frequently valid as both and points to **different files**
(e.g. `165505`: as baseId → an ATEX texture, as fileId → an ASND packfile). If a
curated id doesn't produce the type you expect, call `gw2_resolve(id)` to see
both readings and pick the right selector. Most hand-curated ids are baseIds, so
prefer `base_id=` first.

### STRS decode

`gw2_decode_strs` returns per-record `mode`/`confirmed`/`text`:

- **`raw-utf16` / `confirmed: true`** — byte-exact real game text (verified
  against Spanish/Chinese samples).
- **`packed` / `confirmed: false`** — **structural only**. These records are
  RC4-locked: the keystream comes from runtime cross-reference state that isn't
  present in a standalone entry, so `text` is *not* the real string (framing and
  header fields are still correct). Finishing this needs the RC4 key — see
  `../strs_decode.py`'s docstring. Use `only_confirmed=true` to get just the
  real strings, or `contains="..."` to search them.

Confirmed STRS sample baseIds (from the curated list, verified `strs` magic):
`2925, 2930, 632438, 635540` (plus `2878, 2924, 3558, 156894`).

### Texture resolution pairs

GW2 frequently stores a texture as a pair at consecutive MFT indices: **baseId
B = full resolution**, **baseId B-1 = the half-resolution version** (same
format, exactly half the dimensions). It's a convention, not universal — some
textures are single-resolution and an adjacent baseId can be an unrelated
texture. `gw2_decode_texture` reports `isFullResolution: true` and
`reducedSibling {index,width,height}` when the pairing is detected. A `file_id`
always resolves to the full-size baseId B, so decoding by fileId gives the
original size.

## CLI (usable standalone)

`gw2dat_cli.exe` is handy on its own; each command prints one JSON object:

```powershell
gw2dat_cli info    --dat <Gw2.dat>
gw2dat_cli list    --dat <Gw2.dat> --offset 0 --limit 100
gw2dat_cli lookup  --dat <Gw2.dat> --file-id 123456
gw2dat_cli resolve --dat <Gw2.dat> --id 165505        # both baseId & fileId readings
gw2dat_cli sniff   --dat <Gw2.dat> --index 100000
gw2dat_cli extract --dat <Gw2.dat> --index 100000 --out file.bin
gw2dat_cli texture --dat <Gw2.dat> --index 100000 --out tex.png [--mip 0]
gw2dat_cli parse   --dat <Gw2.dat> --index 5000 --template templates\gw2_packfile.json --max-depth 6
gw2dat_cli parse   --data file.bin --template templates\gw2_packfile.json
```

## Notes / limits

- Only decompression **Method0** (Huffman+LZ77) is implemented — the delta/patch
  Method1 is not, matching the source RE work.
- The texture path decodes the **ATEX family** (ATEX/ATTX/ATEC/ATEP/ATEU/ATET)
  **and the CTEX/CTTX/CTEC/CTEP/CTEU/CTET siblings**. The C-variants have a
  byte-identical container (same fourcc + width/height + mip records) and differ
  only in the 4-byte magic, so the CLI aliases `C*`→`A*` before decoding and
  reports the original magic as `aliasedFrom`. Verified visually: real CTEX item
  icons decode correctly. Standalone `DDS `/PNG/JPEG entries are extracted as-is
  by `gw2_extract` (the GUI's WIC/D3D preview path is not in the headless build).
- Rebuild `gw2dat_cli.exe` if you change anything under `native/`.
