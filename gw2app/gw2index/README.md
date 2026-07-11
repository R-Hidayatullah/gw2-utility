# gw2index — fast, resumable GW2 .dat indexer + query MCP

Scans a GW2 `.dat` once and builds a queryable **SQLite** index, so you never have to
re-scan / extract / decompress to answer "what is fileId X / what type / what chunks /
what's mis-flagged". A companion **MCP server** exposes the index.

## What it records (per MFT entry, no raw data stored)
- **baseId** (MFT index, 1-based) + **all fileIds** that resolve to it.
- MFT header: **offset, size (on-disk), compression flag, CRC, MFT-declared uncompressed size**.
- **Verified compression truth** — GW2 has entries flagged compressed that are *not*;
  the indexer tries the Method0 decompress and records `actually_compressed` (0/1) and
  `error='flagged-compressed-but-not'` for the mis-flagged ones.
- **Sizes**: on-disk → after CRC32-strip → after decompress (`size`, `size_stripped`, `size_final`).
- Sniffed **type** (packfile/texture/strs/asnd/…) and packfile **container** fourcc.
- For packfiles: **chunks** = fourcc + version + the **template struct variant** that
  applies (`fileTypes[container][chunk][version]` → global `chunks[chunk][version]`),
  which resolves the duplicate-chunk ambiguity. Empty variant = not in the template.

## Build
```bash
bash gw2index/build.sh        # compiles sqlite3.o once, then gw2index.exe
```

## Index a dat (multithreaded, resumable)
```bash
# test target: account Local.dat
gw2index_released.exe --dat "%APPDATA%\Guild Wars 2\Local.dat" --out local_index.db --template ../gw2mcp/templates/gw2_packfile.json
# the big one (bump threads):
gw2index_released.exe --dat "…\Guild Wars 2\Gw2.dat" --out gw2_index.db --template ../gw2mcp/templates/gw2_packfile.json --threads 16
```
- **Resumable**: re-running skips entries whose fingerprint (`offset|size|crc|flag`) is
  unchanged; only patched/new entries are re-processed. `--full` forces a full re-index.
- Verified: Local.dat (322 entries) full = ~1.2 s; unchanged re-run = ~0.2 s.

## Query MCP
Registered in `.mcp.json` as **gw2index** (env `GW2_INDEX_DB` → the .db). Restart Claude
Code to load it. Tools:
- `idx_stats` — dat meta, counts, type/container histograms, compression-truth breakdown.
- `idx_lookup(file_id | base_id)` — full record + fileIds + chunks.
- `idx_query(type, container, compressed, false_compressed, has_error, …)` — filtered list.
- `idx_find_chunk(fourcc, container?, version?)` — every entry containing a chunk.
- `idx_chunk_variants` — the (container, chunk, version → struct variant) map.

Run standalone / self-test:
```bash
GW2_INDEX_DB=…/local_index.db python gw2index/mcp/server.py --selftest
```

## Schema
`entries(base_id PK, offset,size,comp_flag,crc,mft_usize,fingerprint,actually_compressed,
size_stripped,size_final,magic,type,container,error)`,
`file_ids(file_id PK, base_id)`, `chunks(base_id,seq,fourcc,version,struct_variant,chunk_size)`,
`meta(key,value)`.
