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
| `gw2_sniff` | Decompress an entry and report only its type + size. |
| `gw2_extract` | Decompress an entry and write the raw file bytes to disk. |
| `gw2_decode_texture` | ATEX-family → RGBA **PNG** (Method0 + inflate + BCn/3DCX). |
| `gw2_parse_packfile` | Packfile → structured field tree via the JSON template. |

Selection is by `index` (MFT index) or `file_id` (resolved via the file-id
table). Detected types: `packfile`, `texture`, `dds`, `png`, `jpeg`, `binary`.

## CLI (usable standalone)

`gw2dat_cli.exe` is handy on its own; each command prints one JSON object:

```powershell
gw2dat_cli info    --dat <Gw2.dat>
gw2dat_cli list    --dat <Gw2.dat> --offset 0 --limit 100
gw2dat_cli lookup  --dat <Gw2.dat> --file-id 123456
gw2dat_cli sniff   --dat <Gw2.dat> --index 100000
gw2dat_cli extract --dat <Gw2.dat> --index 100000 --out file.bin
gw2dat_cli texture --dat <Gw2.dat> --index 100000 --out tex.png [--mip 0]
gw2dat_cli parse   --dat <Gw2.dat> --index 5000 --template templates\gw2_packfile.json --max-depth 6
gw2dat_cli parse   --data file.bin --template templates\gw2_packfile.json
```

## Notes / limits

- Only decompression **Method0** (Huffman+LZ77) is implemented — the delta/patch
  Method1 is not, matching the source RE work.
- The texture path decodes the **ATEX family** (ATEX/ATTX/ATEC/ATEP/ATEU/ATET).
  Standalone `DDS `/PNG/JPEG entries are extracted as-is by `gw2_extract` (the
  GUI's WIC/D3D preview path is intentionally not part of the headless build).
- Rebuild `gw2dat_cli.exe` if you change anything under `native/`.
