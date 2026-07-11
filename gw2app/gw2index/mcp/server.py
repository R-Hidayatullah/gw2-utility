"""gw2index MCP -- query a prebuilt GW2 .dat index (from the gw2index tool).

The index is a SQLite database produced by `gw2index.exe`. These tools answer
"what is fileId X / what type / what chunks / what's mis-flagged" instantly from
the index, with NO dat re-scan, extract, or decompress.

Point it at a database with the GW2_INDEX_DB env var or the `db` argument on any
tool (default: ../local_index.db next to this server).
"""
from __future__ import annotations

import os
import sqlite3
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("gw2index")

_DEFAULT_DB = os.environ.get(
    "GW2_INDEX_DB", str(Path(__file__).resolve().parent.parent / "local_index.db")
)


def _conn(db: Optional[str]) -> sqlite3.Connection:
    path = db or _DEFAULT_DB
    if not Path(path).exists():
        raise FileNotFoundError(
            f"index db not found: {path}. Build it with gw2index.exe or set GW2_INDEX_DB / pass db=."
        )
    c = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    c.row_factory = sqlite3.Row
    return c


def _entry(c: sqlite3.Connection, base_id: int) -> dict[str, Any]:
    row = c.execute("SELECT * FROM entries WHERE base_id=?", (base_id,)).fetchone()
    if not row:
        return {}
    e = dict(row)
    e["file_ids"] = [r["file_id"] for r in c.execute("SELECT file_id FROM file_ids WHERE base_id=? ORDER BY file_id", (base_id,))]
    e["chunks"] = [dict(r) for r in c.execute(
        "SELECT seq,fourcc,version,struct_variant,chunk_size FROM chunks WHERE base_id=? ORDER BY seq", (base_id,))]
    return e


@mcp.tool()
def idx_stats(db: Optional[str] = None) -> dict[str, Any]:
    """Overview of an index: dat metadata, entry/fileId counts, type & container
    histograms, and the compression-truth breakdown (how many entries are flagged
    compressed vs actually compressed -- the mis-flagged ones show up here)."""
    with _conn(db) as c:
        meta = {r["key"]: r["value"] for r in c.execute("SELECT key,value FROM meta")}
        n = c.execute("SELECT COUNT(*) n FROM entries").fetchone()["n"]
        nf = c.execute("SELECT COUNT(*) n FROM file_ids").fetchone()["n"]
        types = {r["type"]: r["n"] for r in c.execute("SELECT type,COUNT(*) n FROM entries GROUP BY type ORDER BY n DESC")}
        conts = {r["container"]: r["n"] for r in c.execute(
            "SELECT container,COUNT(*) n FROM entries WHERE container!='' GROUP BY container ORDER BY n DESC")}
        comp = [dict(r) for r in c.execute(
            "SELECT comp_flag,actually_compressed,COUNT(*) n FROM entries GROUP BY comp_flag,actually_compressed")]
        false_comp = c.execute("SELECT COUNT(*) n FROM entries WHERE comp_flag!=0 AND actually_compressed=0").fetchone()["n"]
        errors = c.execute("SELECT COUNT(*) n FROM entries WHERE error!=''").fetchone()["n"]
    return {"ok": True, "meta": meta, "entries": n, "file_ids": nf, "types": types,
            "containers": conts, "compression": comp, "flagged_compressed_but_not": false_comp,
            "entries_with_errors": errors, "db": db or _DEFAULT_DB}


@mcp.tool()
def idx_lookup(file_id: Optional[int] = None, base_id: Optional[int] = None,
               db: Optional[str] = None) -> dict[str, Any]:
    """Full indexed record for one entry -- by fileId or baseId. Returns the MFT
    header, verified compression truth, sizes (on-disk / stripped / decompressed),
    type/container, all fileIds sharing the baseId, and its chunk list."""
    with _conn(db) as c:
        if file_id is not None:
            row = c.execute("SELECT base_id FROM file_ids WHERE file_id=?", (file_id,)).fetchone()
            if not row:
                return {"ok": False, "error": f"fileId {file_id} not in index"}
            base_id = row["base_id"]
        if base_id is None:
            return {"ok": False, "error": "provide file_id or base_id"}
        e = _entry(c, base_id)
        if not e:
            return {"ok": False, "error": f"baseId {base_id} not in index"}
        return {"ok": True, "entry": e}


@mcp.tool()
def idx_query(type: Optional[str] = None, container: Optional[str] = None,
              compressed: Optional[bool] = None, false_compressed: bool = False,
              has_error: bool = False, limit: int = 50, offset: int = 0,
              db: Optional[str] = None) -> dict[str, Any]:
    """Filtered list of entries. Any of: `type` (packfile/texture/strs/...),
    `container` fourcc (MODL/ASND/cntc/...), `compressed` (actually_compressed),
    `false_compressed` (flagged compressed but not), `has_error`. Paginated."""
    where, args = [], []
    if type is not None: where.append("type=?"); args.append(type)
    if container is not None: where.append("container=?"); args.append(container)
    if compressed is not None: where.append("actually_compressed=?"); args.append(1 if compressed else 0)
    if false_compressed: where.append("comp_flag!=0 AND actually_compressed=0")
    if has_error: where.append("error!=''")
    w = ("WHERE " + " AND ".join(where)) if where else ""
    with _conn(db) as c:
        total = c.execute(f"SELECT COUNT(*) n FROM entries {w}", args).fetchone()["n"]
        rows = c.execute(
            f"SELECT base_id,size,size_stripped,size_final,mft_usize,comp_flag,actually_compressed,"
            f"type,container,magic,error FROM entries {w} ORDER BY base_id LIMIT ? OFFSET ?",
            (*args, limit, offset)).fetchall()
    return {"ok": True, "total": total, "count": len(rows), "offset": offset,
            "entries": [dict(r) for r in rows]}


@mcp.tool()
def idx_find_chunk(fourcc: str, container: Optional[str] = None, version: Optional[int] = None,
                   limit: int = 50, offset: int = 0, db: Optional[str] = None) -> dict[str, Any]:
    """Find every entry whose packfile contains a chunk with this `fourcc`
    (optionally filtered by container and/or chunk version). Returns baseIds +
    the resolved struct variant. Great for "which files hold a GEOM / txtm / ...".
    """
    where, args = ["ch.fourcc=?"], [fourcc]
    if version is not None: where.append("ch.version=?"); args.append(version)
    if container is not None: where.append("e.container=?"); args.append(container)
    w = "WHERE " + " AND ".join(where)
    with _conn(db) as c:
        total = c.execute(
            f"SELECT COUNT(*) n FROM chunks ch JOIN entries e ON e.base_id=ch.base_id {w}", args).fetchone()["n"]
        rows = c.execute(
            f"SELECT ch.base_id,e.container,ch.version,ch.struct_variant,ch.chunk_size "
            f"FROM chunks ch JOIN entries e ON e.base_id=ch.base_id {w} "
            f"ORDER BY ch.base_id LIMIT ? OFFSET ?", (*args, limit, offset)).fetchall()
    return {"ok": True, "total": total, "count": len(rows), "hits": [dict(r) for r in rows]}


@mcp.tool()
def idx_chunk_variants(db: Optional[str] = None) -> dict[str, Any]:
    """List every distinct (container, chunk fourcc, version -> struct variant)
    combination seen across the index. This is the map of which chunk versions
    exist and which template struct applies to each -- resolves the duplicate-chunk
    ambiguity. `struct_variant` empty = chunk not in the template."""
    with _conn(db) as c:
        rows = c.execute(
            "SELECT e.container,ch.fourcc,ch.version,ch.struct_variant,COUNT(*) n "
            "FROM chunks ch JOIN entries e ON e.base_id=ch.base_id "
            "GROUP BY e.container,ch.fourcc,ch.version,ch.struct_variant "
            "ORDER BY e.container,ch.fourcc,ch.version").fetchall()
    return {"ok": True, "count": len(rows), "variants": [dict(r) for r in rows]}


if __name__ == "__main__":
    import sys
    if "--selftest" in sys.argv:
        print("default db:", _DEFAULT_DB)
        print(idx_stats())
    else:
        mcp.run()
