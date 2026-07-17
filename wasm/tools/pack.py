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
    """Returns {vfs_path: abs_path} for the aircraft's transitive closure."""
    top = root / "aircraft" / model / f"{model}.xml"
    if not top.is_file():
        sys.exit(f"aircraft xml not found: {top}")
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
            sys.exit(f"XML parse error in {f}: {e}")
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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, type=Path)
    ap.add_argument("--id", required=True, help="API aircraft id (e.g. c172)")
    ap.add_argument("--model", required=True, help="JSBSim model (e.g. c172p)")
    ap.add_argument("--name", required=True, help="display name")
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    files = walk(args.root, args.model)
    print(f"{args.id}: {len(files)} files")

    index = []
    blob = bytearray()
    manifest_files = []
    for vfs in sorted(files):
        data = files[vfs].read_bytes()
        index.append((vfs.encode(), len(blob), len(data)))
        manifest_files.append(
            {"path": vfs, "bytes": len(data),
             "sha256": hashlib.sha256(data).hexdigest()})
        blob += data

    head = bytearray(b"JSBP")
    head += struct.pack("<II", 1, len(index))
    for path, off, ln in index:
        head += struct.pack("<H", len(path)) + path + struct.pack("<II", off, ln)

    args.out.mkdir(parents=True, exist_ok=True)
    pack_path = args.out / f"{args.id}.jsbpack"
    pack_path.write_bytes(bytes(head) + bytes(blob))
    print(f"wrote {pack_path} ({pack_path.stat().st_size} bytes)")

    try:
        commit = subprocess.run(
            ["git", "-C", str(args.root), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        commit = "unknown"

    manifest = {
        "id": args.id, "name": args.name, "jsbsim_model": args.model,
        "pack_version": 1, "abi_version": 1, "source_commit": commit,
        "files": manifest_files,
    }
    (args.out / f"{args.id}.manifest.json").write_text(
        json.dumps(manifest, indent=2))

    catalog_path = args.out / "catalog.json"
    catalog = []
    if catalog_path.is_file():
        catalog = json.loads(catalog_path.read_text())
    catalog = [e for e in catalog if e.get("id") != args.id]
    catalog.append({
        "id": args.id, "name": args.name, "jsbsim_model": args.model,
        "pack": f"{args.id}.jsbpack",
        "sha256": hashlib.sha256(pack_path.read_bytes()).hexdigest(),
    })
    catalog.sort(key=lambda e: e["id"])
    catalog_path.write_text(json.dumps(catalog, indent=2))
    print(f"catalog updated: {catalog_path}")


if __name__ == "__main__":
    main()
