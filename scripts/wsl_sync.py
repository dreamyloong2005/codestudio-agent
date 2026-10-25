"""Copy source into an owned Linux-native build directory without Windows caches.

Run inside WSL: python3 scripts/wsl_sync.py /var/tmp/codestudio-agent-linux
The source remains authoritative; this only copies changed source files and
never deletes destination files or copies .xmake/build/state directories.
"""
import argparse
import os
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
destination = args.destination.resolve()
if destination == source or destination in source.parents or source in destination.parents:
    parser.error("The native mirror must be outside the source directory")
marker = destination / ".codestudio-build-mirror"
if destination.exists() and (not marker.is_file() or marker.read_text() != str(source)):
    parser.error("Refusing to copy into an existing directory not owned by this source mirror")
destination.mkdir(parents=True, exist_ok=True)
marker.write_text(str(source))
paths = []
for name in ("include", "src", "tests", "scripts", "examples", "docs", "protocol", ".github"):
    paths.extend(p for p in (source / name).rglob("*") if p.is_file()
                 and not any(part in {"__pycache__", ".codestudio", "artifacts", "build", ".xmake"}
                             for part in p.relative_to(source).parts))
paths.extend(source / name for name in ("xmake.lua", "xmake-requires.lock", "README.md"))
changed = 0
for path in paths:
    target = destination / path.relative_to(source)
    if target.is_file() and path.read_bytes() == target.read_bytes():
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".sync-tmp")
    shutil.copyfile(path, temporary)
    os.replace(temporary, target)
    changed += 1
print(f"Linux source mirror: {destination}; {changed} files updated")
