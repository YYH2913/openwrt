#!/bin/sh
# Archive a complete XR1710G target output without overwriting earlier builds.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 - "$root" "$@" <<'PY'
import argparse
import datetime
import os
from pathlib import Path
import shutil
import sys

parser = argparse.ArgumentParser(description="Archive all XR1710G target files")
parser.add_argument("root")
parser.add_argument("--timestamp", help="local YYYYMMDD-HHMMSS (default: now)")
parser.add_argument("--dry-run", action="store_true")
args = parser.parse_args()
stamp = args.timestamp or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
try:
    parsed = datetime.datetime.strptime(stamp, "%Y%m%d-%H%M%S")
except ValueError:
    parser.error("timestamp must be a valid local YYYYMMDD-HHMMSS")
if parsed.strftime("%Y%m%d-%H%M%S") != stamp:
    parser.error("timestamp must use exactly YYYYMMDD-HHMMSS")

root = Path(args.root)
source = root / "bin/targets/airoha/an7581"
output = root / "out"
if not source.is_dir():
    sys.exit(f"Missing target output: {source}")
files = []
for directory, dirs, names in os.walk(source, followlinks=False):
    dirs[:] = sorted(d for d in dirs if not (Path(directory) / d).is_symlink())
    for name in sorted(names):
        src = Path(directory) / name
        if src.is_symlink() or not src.is_file():
            continue
        relative = src.relative_to(source)
        dst = output / relative.with_name(f"{relative.stem}-{stamp}{relative.suffix}")
        if os.path.lexists(dst):
            sys.exit(f"Refusing to overwrite existing archive: {dst}")
        files.append((src, dst))
if not files:
    sys.exit(f"No regular target files to archive: {source}")
if args.dry_run:
    for src, dst in files:
        print(f"{src} -> {dst}")
    sys.exit(0)

created = []
try:
    for src, dst in files:
        dst.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation also rejects a destination appearing after preflight.
        with dst.open("xb") as target:
            created.append(dst)
            with src.open("rb") as original:
                shutil.copyfileobj(original, target)
        shutil.copystat(src, dst)
        print(dst)
except BaseException:
    for dst in reversed(created):
        dst.unlink(missing_ok=True)
    raise
print(f"Archived {len(files)} regular files with local timestamp {stamp}")
PY
