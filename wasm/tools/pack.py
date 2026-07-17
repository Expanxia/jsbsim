#!/usr/bin/env python3
"""kiro-wasm aircraft data packer.

Walks an aircraft's transitive XML references (engine/thruster/system files,
resolved the way JSBSim's CheckPathName does) and emits:
  <out>/<id>.jsbpack     one binary: header + index + concatenated files
  <out>/<id>.manifest.json  provenance: files, sha256, source commit
and updates <out>/catalog.json.

Pack format (little-endian):
  magic  "JSBP" | u32 version=1 | u32 count
  count * ( u16 path_len | path utf-8 | u32 offset | u32 len )   # offset
  file blob                                                      # from blob0

Usage:
  python pack.py --root <jsbsim_repo> --id c172 --model c172p \
                 --name "Cessna C-172P" --out ../dist
"""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Candidate directories per referencing tag, mirroring FGModel/FGPropulsion/
# FGFCS FindFullPathName order (aircraft dir first, then shared roots).
TAG_DIRS = {
    "engine": ["aircraft/{m}", "aircraft/{m}/Engines", "engine"],
    "thruster": ["aircraft/{m}", "aircraft/{m}/Engines", "engine"],
    "propeller": ["aircraft/{m}", "aircraft/{m}/Engines", "engine"],
    "system": ["aircraft/{m}", "aircraft/{m}/Systems", "systems"],
    "autopilot": ["aircraft/{m}", "aircraft/{m}/Systems", "systems"],
    "flight_control": ["aircraft/{m}", "aircraft/{m}/Systems", "systems"],
}
DEFAULT_DIRS = ["aircraft/{m}"]


def resolve(root: Path, model: str, tag: str, fname: str) -> Path | None:
    if not fname.endswith(".xml"):
        fname += ".xml"
    for d in TAG_DIRS.get(tag, DEFAULT_DIRS):
        cand = root / d.format(m=model) / fname
        if cand.is_file():
            return cand
    return None


def walk(root: Path, model: str) -> dict[str, Path]:
    """Returns {vfs_path: abs_path} for the aircraft's transitive closure.
    Raises RuntimeError on a missing top XML or a parse error (so batch
    conversion can skip the offending aircraft)."""
    top = root / "aircraft" / model / f"{model}.xml"
    if not top.is_file():
        raise RuntimeError(f"aircraft xml not found: {top}")
    seen: dict[str, Path] = {}
    queue = [top]
    while queue:
        f = queue.pop()
        vfs = f.relative_to(root).as_posix()
        if vfs in seen:
            continue
        seen[vfs] = f
        try:
            tree = ET.parse(f)
        except ET.ParseError as e:
            raise RuntimeError(f"XML parse error in {f}: {e}")
        for el in tree.iter():
            fname = el.get("file")
            if not fname:
                continue
            dep = resolve(root, model, el.tag, fname)
            if dep is None:
                print(f"  warning: unresolved {el.tag} file={fname} in {vfs}")
                continue
            queue.append(dep)
    return seen


def display_name(root: Path, model: str) -> str:
    """Human name from the aircraft XML's <fdm_config name=...>, else the id."""
    try:
        top = root / "aircraft" / model / f"{model}.xml"
        name = ET.parse(top).getroot().get("name")
        return name.strip() if name else model
    except Exception:
        return model


def pack_one(root: Path, ac_id: str, model: str, name: str, out: Path,
             commit: str) -> dict:
    """Build one `aircraft/<id>.jsbpack` and return its catalog entry.

    Pack container (gzip'd at max level; payload is XML, ~7x; one stream so
    the shared XML compresses across files) — the manifest JSON is embedded
    (no separate .manifest.json; catalog.json is the discovery index):
      "JSBP" | u32 _reserved | u32 manifest_len | manifest_json
            | u32 file_count | file_count*( u16 path_len | path | u32 len )
            | concatenated file bytes
    The second word is a fixed reserved constant (1), NOT a format version —
    it is never bumped pre-release; a stale pack is regenerated, not
    negotiated. Kept only so the on-disk packs need no rewrite.
    """
    files = walk(root, model)

    ordered = sorted(files)
    index = bytearray()
    blob = bytearray()
    manifest_files = []
    for vfs in ordered:
        data = files[vfs].read_bytes()
        path = vfs.encode()
        index += struct.pack("<H", len(path)) + path + struct.pack("<I", len(data))
        manifest_files.append(
            {"path": vfs, "bytes": len(data),
             "sha256": hashlib.sha256(data).hexdigest()})
        blob += data

    manifest = {
        "id": ac_id, "name": name, "jsbsim_model": model,
        "abi_version": 1, "source_commit": commit, "files": manifest_files,
    }
    manifest_bytes = json.dumps(manifest, separators=(",", ":")).encode()

    container = bytearray(b"JSBP")
    container += struct.pack("<II", 1, len(manifest_bytes))
    container += manifest_bytes
    container += struct.pack("<I", len(ordered))
    container += index
    container += blob

    packed = gzip.compress(bytes(container), compresslevel=9)

    aircraft_dir = out / "aircraft"
    aircraft_dir.mkdir(parents=True, exist_ok=True)
    pack_rel = f"aircraft/{ac_id}.jsbpack"
    pack_path = out / pack_rel
    pack_path.write_bytes(packed)
    print(f"  {ac_id}: {len(files)} files, {len(container)} -> {len(packed)} B gz")

    return {
        "id": ac_id, "name": name, "jsbsim_model": model, "pack": pack_rel,
        "sha256": hashlib.sha256(packed).hexdigest(),
    }


def write_catalog(out: Path, entries: list[dict]) -> None:
    """Merge `entries` into out/catalog.json (replace by id, sort by id)."""
    catalog_path = out / "catalog.json"
    catalog = []
    if catalog_path.is_file():
        catalog = json.loads(catalog_path.read_text())
    ids = {e["id"] for e in entries}
    catalog = [e for e in catalog if e.get("id") not in ids] + entries
    catalog.sort(key=lambda e: e["id"])
    catalog_path.write_text(json.dumps(catalog, indent=2))
    print(f"catalog: {len(catalog)} aircraft -> {catalog_path}")


def git_commit(root: Path) -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return "unknown"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--all", action="store_true",
                    help="pack every aircraft under <root>/aircraft/")
    ap.add_argument("--id", help="API aircraft id (single mode; e.g. c172)")
    ap.add_argument("--model", help="JSBSim model (single mode; e.g. c172p)")
    ap.add_argument("--name", help="display name (single mode)")
    args = ap.parse_args()

    commit = git_commit(args.root)

    if args.all:
        ac_root = args.root / "aircraft"
        models = sorted(
            d.name for d in ac_root.iterdir()
            if d.is_dir() and (d / f"{d.name}.xml").is_file())
        print(f"packing {len(models)} aircraft from {ac_root}")
        entries, failed = [], []
        for model in models:
            try:
                entries.append(pack_one(
                    args.root, model, model, display_name(args.root, model),
                    args.out, commit))
            except Exception as e:
                failed.append((model, str(e)))
                print(f"  SKIP {model}: {e}")
        write_catalog(args.out, entries)
        print(f"done: {len(entries)} packed, {len(failed)} skipped")
        for model, err in failed:
            print(f"  skipped {model}: {err}")
        return

    if not (args.id and args.model and args.name):
        ap.error("single mode requires --id, --model, --name (or use --all)")
    entry = pack_one(args.root, args.id, args.model, args.name, args.out, commit)
    write_catalog(args.out, [entry])


if __name__ == "__main__":
    main()
