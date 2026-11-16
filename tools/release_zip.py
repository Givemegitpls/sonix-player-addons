#!/usr/bin/env python3
"""One zip per addon, and one metadata fragment beside it.

    tools/release_zip.py --build build/Addons --out build/release
    tools/release_zip.py --build build/Addons --out build/release --ids "dino qobuz"

The zip holds the addon folder's contents at its root (`addon.json`,
`addon.so`, `icon.png`, `assets/`), exactly what `make pack` wrote, so it is
unzipped into `<card>/Addons/<id>/`.

The fragment -- `<id>.json` -- is what builds the store's catalogue without
downloading the zips again: the sizes and sha256 of both the archive and the
library inside it, and the manifest fields the list needs to draw the row. It
is uploaded into the same release as the zip.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

SCHEMA = 1


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def zip_dir(src: Path, dst: Path) -> None:
    """The folder's files at the root of the archive, in a stable order."""
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(p for p in src.rglob("*") if p.is_file()):
            archive.write(path, path.relative_to(src).as_posix())


def manifest_fields(path: Path) -> dict[str, object]:
    if not path.is_file():
        return {}
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}
    if not isinstance(doc, dict):
        return {}
    fields: dict[str, object] = {}
    for key in ("title", "section", "version"):
        value = doc.get(key)
        if isinstance(value, str) and value:
            fields[key] = value
    titles = doc.get("titles")
    if isinstance(titles, dict):
        fields["titles"] = {
            k: v for k, v in titles.items() if isinstance(k, str) and isinstance(v, str)
        }
    return fields


def make_one(folder: Path, out: Path) -> Path:
    addon_id = folder.name
    archive = out / f"{addon_id}.zip"
    zip_dir(folder, archive)
    zip_sha, zip_size = sha256_file(archive)
    so_sha, so_size = (
        sha256_file(folder / "addon.so") if (folder / "addon.so").is_file() else ("", 0)
    )

    fragment: dict[str, object] = {
        "schema": SCHEMA,
        "id": addon_id,
        "asset": archive.name,
    }
    fragment.update(manifest_fields(folder / "addon.json"))
    fragment["size"] = zip_size
    fragment["sha256"] = zip_sha
    fragment["so_size"] = so_size
    fragment["so_sha256"] = so_sha

    target = out / f"{addon_id}.json"
    target.write_text(
        json.dumps(fragment, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return archive


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build", default="build/Addons", help="what `make pack` wrote"
    )
    parser.add_argument(
        "--out", default="build/release", help="zips and fragments go here"
    )
    parser.add_argument(
        "--ids", default="", help="space separated; empty means every folder in --build"
    )
    parser.add_argument(
        "--expect-version",
        default="",
        help="the version the tag names; every selected addon.json must carry it",
    )
    args = parser.parse_args()

    build = Path(args.build)
    if not build.is_dir():
        print(
            f"release_zip: {build} is not there -- build and pack first",
            file=sys.stderr,
        )
        return 1
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    wanted = args.ids.split()
    folders = sorted(
        p for p in build.iterdir() if p.is_dir() and (p / "addon.so").is_file()
    )
    if wanted:
        folders = [p for p in folders if p.name in set(wanted)]
    if not folders:
        print("release_zip: nothing to package", file=sys.stderr)
        return 1

    # The tag is the release's version and addon.json is what a fresh install
    # shows: they have to agree, or the catalogue names one number and the addon
    # another. Checked before anything is written, so a forgotten bump does not
    # produce a release -- and a release never half-happens.
    if args.expect_version:
        wrong = []
        for folder in folders:
            version = manifest_fields(folder / "addon.json").get("version")
            if version != args.expect_version:
                wrong.append(
                    f"{folder.name}: addon.json says {version!r}, the tag says "
                    f"{args.expect_version!r}"
                )
        if wrong:
            for line in wrong:
                print(f"release_zip: {line}", file=sys.stderr)
            print(
                'release_zip: bump "version" in the addon\'s addon.json, or tag '
                "the version it carries",
                file=sys.stderr,
            )
            return 1

    for folder in folders:
        archive = make_one(folder, out)
        size = archive.stat().st_size
        print(f"release_zip: {archive} ({size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
