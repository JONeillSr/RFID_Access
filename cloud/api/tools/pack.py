"""Build a deployment zip the Linux Functions host can actually read.

WHY THIS EXISTS INSTEAD OF Compress-Archive
PowerShell's Compress-Archive produces a zip that is valid on Windows and subtly
broken for this purpose, in two ways:

  1. It records almost no DIRECTORY entries -- of the tree here it emitted 65,
     and not the one containing the functions.
  2. It writes external_attr = 0, i.e. Unix mode 000. Every file arrives with no
     read permission.

Flex Consumption MOUNTS this zip rather than extracting it, so both defects
survive to runtime. The failure is maddening: the files are demonstrably present
at the right paths, `az` reports "Deployment was successful", the host starts
cleanly -- and the Node worker reports

    Worker was unable to load entry point "dist/api/src/functions/*.js":
    Found zero files matching the supplied pattern

because a glob has to enumerate directories and stat files, and it can do
neither. The host then registers zero functions and every route 404s, including
/api/v1/sync, which takes the whole door fleet offline.

So: write entries with create_system=3 (Unix), real modes, and an explicit entry
for every directory.

Usage (from cloud/api):  python tools/pack.py [-o deploy.zip]
"""
from __future__ import annotations

import argparse
import os
import sys
import zipfile

# Exactly what the host needs at the zip root. host.json and package.json must be
# at the ROOT, not inside a folder -- the host looks for them there and nowhere else.
INCLUDE = ["host.json", "package.json", "dist", "node_modules"]

FILE_MODE = 0o644
DIR_MODE = 0o755


def entry(name: str, mode: int, is_dir: bool) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name + ("/" if is_dir else ""))
    info.create_system = 3                     # Unix, so external_attr is honoured
    info.external_attr = (mode << 16) | (0x10 if is_dir else 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.date_time = (1980, 1, 1, 0, 0, 0)     # deterministic
    return info


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="deploy.zip")
    args = ap.parse_args()

    root = os.getcwd()
    missing = [p for p in INCLUDE if not os.path.exists(os.path.join(root, p))]
    if missing:
        print(f"missing {', '.join(missing)} -- run `npm run build` first", file=sys.stderr)
        return 1

    seen_dirs: set[str] = set()
    files = dirs = 0

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        def add_dirs(rel: str) -> None:
            """Emit an entry for every ancestor, so readdir works at each level."""
            nonlocal dirs
            parts = rel.replace("\\", "/").split("/")
            for i in range(1, len(parts) + 1):
                d = "/".join(parts[:i])
                if d and d not in seen_dirs:
                    seen_dirs.add(d)
                    z.writestr(entry(d, DIR_MODE, True), b"")
                    dirs += 1

        for item in INCLUDE:
            path = os.path.join(root, item)
            if os.path.isfile(path):
                with open(path, "rb") as fh:
                    z.writestr(entry(item, FILE_MODE, False), fh.read())
                files += 1
                continue

            for dirpath, dirnames, filenames in os.walk(path):
                dirnames.sort()
                rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")
                add_dirs(rel_dir)
                for fn in sorted(filenames):
                    rel = f"{rel_dir}/{fn}"
                    try:
                        with open(os.path.join(dirpath, fn), "rb") as fh:
                            z.writestr(entry(rel, FILE_MODE, False), fh.read())
                        files += 1
                    except OSError as e:
                        print(f"skip {rel}: {e}", file=sys.stderr)

    size = os.path.getsize(args.out)
    print(f"{args.out}: {files} files, {dirs} directories, {size/1e6:.1f} MB")

    # Refuse to hand over a package that cannot work, rather than letting the
    # deployment "succeed" and take the fleet down.
    with zipfile.ZipFile(args.out) as z:
        names = set(z.namelist())
        for required in ("host.json", "package.json"):
            if required not in names:
                print(f"FAIL: {required} is not at the zip root", file=sys.stderr)
                return 1
        fns = [n for n in names if n.startswith("dist/api/src/functions/") and n.endswith(".js")]
        if not fns:
            print("FAIL: no function modules in dist/api/src/functions/", file=sys.stderr)
            return 1
        if "dist/api/src/functions/" not in names:
            print("FAIL: no directory entry for dist/api/src/functions/", file=sys.stderr)
            return 1
        bad = [i.filename for i in z.infolist()
               if not i.is_dir() and ((i.external_attr >> 16) & 0o400) == 0]
        if bad:
            print(f"FAIL: {len(bad)} file(s) not readable, e.g. {bad[0]}", file=sys.stderr)
            return 1
        print(f"verified: {len(fns)} function module(s), all entries readable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
