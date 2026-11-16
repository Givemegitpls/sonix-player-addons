#!/usr/bin/env python3
"""A compatibility release: every current zip in one release, tagged `latest`.

Old store builds (0.0.x) ask GitHub for the single newest release and treat its
`.zip` assets as the catalogue. Per-addon tags leave them with one addon -- or
none, once the `store` release exists -- so this job copies the current archive
of every addon into one `latest` release, recreated last so it is the newest.

Nothing is rebuilt or repacked: each archive is downloaded from the release that
publishes it and uploaded byte for byte, so its size and sha256 are exactly what
they were. A client that is already current compares equal and is offered
nothing -- copying rather than zipping again is what makes that true, and it
costs no build time at all, only the transfer.

    tools/rolling_release.py --repo owner/name --dry-run
    tools/rolling_release.py --repo owner/name --publish

`--publish` deletes the previous `latest` release (with its tag) and creates it
again, so its `created_at` is the newest one in the repository. The catalogue
(`index.json`) and the new store's API fallback both skip this tag: it is a copy
of the real releases, not a release of its own.

Disable it with the repository variable ROLLING_RELEASE=off.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

STORE_TAG = "store"
ROLLING_TAG = "latest"


def gh(*args: str, check: bool = True) -> str:
    result = subprocess.run(["gh", *args], check=False, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def current_catalogue(repo: str, workdir: Path) -> list[dict[str, object]]:
    gh(
        "release",
        "download",
        STORE_TAG,
        "--repo",
        repo,
        "--pattern",
        "index.json",
        "--dir",
        str(workdir),
        "--clobber",
    )
    path = workdir / "index.json"
    if not path.is_file():
        raise RuntimeError(f"no index.json in the {STORE_TAG} release")
    doc = json.loads(path.read_text(encoding="utf-8"))
    addons = doc.get("addons")
    if not isinstance(addons, list) or not addons:
        raise RuntimeError("the catalogue names no addons")
    return [item for item in addons if isinstance(item, dict)]


def fetch_copy(repo: str, entry: dict[str, object], workdir: Path) -> Path:
    tag = entry.get("tag")
    asset = entry.get("asset")
    if not isinstance(tag, str) or not isinstance(asset, str):
        raise TypeError(f"the catalogue entry is not tag+asset: {entry!r}")
    folder = workdir / str(entry.get("id", asset))
    folder.mkdir(parents=True, exist_ok=True)
    gh(
        "release",
        "download",
        tag,
        "--repo",
        repo,
        "--pattern",
        asset,
        "--dir",
        str(folder),
        "--clobber",
    )
    path = folder / asset
    if not path.is_file():
        raise RuntimeError(f"{tag}/{asset} did not download")
    size = entry.get("size")
    sha = entry.get("sha256")
    if isinstance(size, int) and path.stat().st_size != size:
        raise RuntimeError(
            f"{tag}/{asset}: size {path.stat().st_size}, the catalogue says {size}"
        )
    if isinstance(sha, str) and sha and sha256_file(path) != sha:
        raise RuntimeError(f"{tag}/{asset}: sha256 is not the published one")
    return path


def publish(repo: str, files: list[Path]) -> None:
    gh(
        "release",
        "delete",
        ROLLING_TAG,
        "--repo",
        repo,
        "--yes",
        "--cleanup-tag",
        check=False,
    )
    gh(
        "release",
        "create",
        ROLLING_TAG,
        "--repo",
        repo,
        "--title",
        "Latest",
        "--notes",
        "Every addon's current archive in one release, for store builds that "
        "only read the newest release. The files are copies: their size and "
        "sha256 are the ones of the releases they came from.",
        *[str(path) for path in files],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="fetch and check, but do not touch the release",
    )
    parser.add_argument(
        "--publish", action="store_true", help="recreate the `latest` release"
    )
    args = parser.parse_args()

    if "/" not in args.repo:
        print("rolling_release: --repo owner/name is required", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        entries = current_catalogue(args.repo, workdir)
        files = [fetch_copy(args.repo, entry, workdir) for entry in entries]
        total = sum(path.stat().st_size for path in files)
        print(
            f"rolling_release: {len(files)} archive(s), {total} bytes, verified against the catalogue"
        )
        if args.dry_run or not args.publish:
            return 0
        publish(args.repo, files)
        print(f"rolling_release: {ROLLING_TAG} release recreated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
