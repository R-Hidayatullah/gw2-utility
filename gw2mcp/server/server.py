"""
gw2mcp -- MCP server for reverse-engineered Guild Wars 2 archive tooling.

This is a *thin* layer: every heavy operation (MFT parsing, ANet Method0
decompression, ATEX->PNG decoding, packfile template parsing) is done by the
native C++ CLI `../native/gw2dat_cli.exe`. This server just validates
arguments, shells out to that binary, and returns its JSON.

Build the native binary once before using:
    powershell -ExecutionPolicy Bypass -File ../native/build.ps1

Run standalone for a smoke test:
    python server.py --selftest
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

# --------------------------------------------------------------------------- #
#  Paths & defaults
# --------------------------------------------------------------------------- #
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "native" / "gw2dat_cli.exe"
DEFAULT_TEMPLATE = ROOT / "templates" / "gw2_packfile.json"

# Default archives. Override per-call with the `dat_path` argument, or globally
# with the GW2_DAT_PATH / GW2_LOCAL_DAT_PATH environment variables.
DEFAULT_GW2_DAT = os.environ.get(
    "GW2_DAT_PATH",
    r"C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat",
)
DEFAULT_LOCAL_DAT = os.environ.get(
    "GW2_LOCAL_DAT_PATH",
    str(Path(os.environ.get("APPDATA", "")) / "Guild Wars 2" / "Local.dat"),
)

mcp = FastMCP("gw2mcp")


# --------------------------------------------------------------------------- #
#  Core: invoke the native CLI and parse its JSON
# --------------------------------------------------------------------------- #
def _run(args: list[str]) -> dict[str, Any]:
    """Run gw2dat_cli.exe with the given args, returning the parsed JSON dict.

    Never raises for a "clean" tool failure: the CLI reports those as
    {"ok": false, "error": ...} which we pass straight through. Only genuinely
    broken invocations (missing binary, non-JSON output) become error dicts.
    """
    if not CLI.exists():
        return {
            "ok": False,
            "error": (
                f"native CLI not found at {CLI}. Build it first: "
                f"powershell -ExecutionPolicy Bypass -File {ROOT / 'native' / 'build.ps1'}"
            ),
        }
    try:
        proc = subprocess.run(
            [str(CLI), *args],
            capture_output=True,
            text=True,
            timeout=300,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "gw2dat_cli timed out (300s)"}

    out = proc.stdout.strip()
    if not out:
        return {
            "ok": False,
            "error": f"gw2dat_cli produced no output (exit {proc.returncode}); stderr: {proc.stderr.strip()}",
        }
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {"ok": False, "error": f"could not parse CLI output as JSON: {out[:500]}"}


def _target_flags(index: Optional[int], file_id: Optional[int]) -> list[str]:
    """Turn an index/file_id selector into CLI flags (exactly one required)."""
    if index is not None:
        return ["--index", str(index)]
    if file_id is not None:
        return ["--file-id", str(file_id)]
    raise ValueError("provide either `index` or `file_id`")


def _dat(dat_path: Optional[str], local: bool) -> str:
    if dat_path:
        return dat_path
    return DEFAULT_LOCAL_DAT if local else DEFAULT_GW2_DAT


# --------------------------------------------------------------------------- #
#  Tools
# --------------------------------------------------------------------------- #
@mcp.tool()
def gw2_info(dat_path: Optional[str] = None, local: bool = False) -> dict[str, Any]:
    """Read a GW2 .dat archive header + MFT summary.

    Returns version, file size, MFT offset/size, and entry/file-id/base-id
    counts. `local=True` targets the account Local.dat instead of Gw2.dat.
    """
    return _run(["info", "--dat", _dat(dat_path, local)])


@mcp.tool()
def gw2_list_entries(
    offset: int = 0,
    limit: int = 100,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """List MFT entries (index, offset, size, compression flag, CRC, ...).

    Paginated via `offset`/`limit`. The archive has ~800k entries, so page
    through it rather than requesting everything at once.
    """
    return _run(
        ["list", "--dat", _dat(dat_path, local), "--offset", str(offset), "--limit", str(limit)]
    )


@mcp.tool()
def gw2_lookup(
    file_id: Optional[int] = None,
    base_id: Optional[int] = None,
    search_file_id: Optional[int] = None,
    search_base_id: Optional[int] = None,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """Map between GW2 file-ids, base-ids, and MFT indices.

    - `file_id`: resolve a fileId to its baseId (== MFT index for extraction).
    - `base_id`: list the fileIds sharing a baseId.
    - `search_file_id` / `search_base_id`: suffix-search ids (ArenaNet style).
    Provide exactly one.
    """
    dat = _dat(dat_path, local)
    if file_id is not None:
        return _run(["lookup", "--dat", dat, "--file-id", str(file_id)])
    if base_id is not None:
        return _run(["lookup", "--dat", dat, "--base-id", str(base_id)])
    if search_file_id is not None:
        return _run(["lookup", "--dat", dat, "--search-file-id", str(search_file_id)])
    if search_base_id is not None:
        return _run(["lookup", "--dat", dat, "--search-base-id", str(search_base_id)])
    return {"ok": False, "error": "provide one of file_id / base_id / search_file_id / search_base_id"}


@mcp.tool()
def gw2_sniff(
    index: Optional[int] = None,
    file_id: Optional[int] = None,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """Extract + decompress an entry and report its detected type/size only.

    Type is one of: packfile, texture, dds, png, jpeg, binary. Cheap way to
    decide whether to call gw2_decode_texture vs gw2_parse_packfile next.
    """
    try:
        target = _target_flags(index, file_id)
    except ValueError as e:
        return {"ok": False, "error": str(e)}
    return _run(["sniff", "--dat", _dat(dat_path, local), *target])


@mcp.tool()
def gw2_extract(
    out_path: str,
    index: Optional[int] = None,
    file_id: Optional[int] = None,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """Extract + decompress one entry, writing the raw file bytes to `out_path`.

    This is the exact "Export Decompressed" output: an ATEX/DDS/PNG/packfile/...
    depending on the entry. Returns bytesWritten and the sniffed type.
    """
    try:
        target = _target_flags(index, file_id)
    except ValueError as e:
        return {"ok": False, "error": str(e)}
    return _run(["extract", "--dat", _dat(dat_path, local), *target, "--out", out_path])


@mcp.tool()
def gw2_decode_texture(
    out_path: str,
    index: Optional[int] = None,
    file_id: Optional[int] = None,
    mip: int = 0,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """Decode an ATEX-family texture entry to an RGBA PNG at `out_path`.

    Handles the full ANet pipeline: Method0 decompress -> ATEX inflate ->
    BCn/3DCX -> RGBA8888 -> PNG. Returns width/height, source format (e.g.
    DXT5, BC7, 3DCX), and mip count. `mip` selects the mip level.
    """
    try:
        target = _target_flags(index, file_id)
    except ValueError as e:
        return {"ok": False, "error": str(e)}
    return _run(
        ["texture", "--dat", _dat(dat_path, local), *target, "--out", out_path, "--mip", str(mip)]
    )


@mcp.tool()
def gw2_parse_packfile(
    index: Optional[int] = None,
    file_id: Optional[int] = None,
    data_path: Optional[str] = None,
    template_path: Optional[str] = None,
    max_depth: int = 6,
    out_path: Optional[str] = None,
    dat_path: Optional[str] = None,
    local: bool = False,
) -> dict[str, Any]:
    """Parse a GW2 packfile (PF) into a structured field tree via the template.

    Source is either an entry in the DAT (`index`/`file_id`) or a raw
    decompressed file on disk (`data_path`). Follows ArenaNet self-relative
    pointers per the registry in templates/gw2_packfile.json. `max_depth` caps
    the returned tree; pass `out_path` to dump the full tree to a file and get
    only a small metadata summary back (recommended for large packfiles).
    """
    tpl = template_path or str(DEFAULT_TEMPLATE)
    args = ["parse", "--template", tpl, "--max-depth", str(max_depth)]
    if data_path:
        args += ["--data", data_path]
    else:
        try:
            target = _target_flags(index, file_id)
        except ValueError as e:
            return {"ok": False, "error": str(e)}
        args += ["--dat", _dat(dat_path, local), *target]
    if out_path:
        args += ["--out", out_path]
    return _run(args)


# --------------------------------------------------------------------------- #
#  Entry point
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    print(f"CLI: {CLI} (exists={CLI.exists()})")
    print(f"template: {DEFAULT_TEMPLATE} (exists={DEFAULT_TEMPLATE.exists()})")
    print(f"Gw2.dat default: {DEFAULT_GW2_DAT}")
    print(f"Local.dat default: {DEFAULT_LOCAL_DAT}")
    print("info ->", json.dumps(gw2_info(), indent=2)[:400])
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        raise SystemExit(_selftest())
    mcp.run()
