#!/usr/bin/env python3
"""The store's catalogue: one index.json built out of the repository's releases.

    tools/publish_index.py --repo owner/name --out build/index.json

Every release that carries a per-addon metadata fragment (`<id>.json`, written
by tools/release_zip.py) contributes one entry; a release that only carries
`<id>.zip` (the old all-in-one tags) is read out of the zip instead, once.

The newest version of each addon wins: the version is taken from the tag
`<id>-v<semver>`, else from the fragment, else from the zip's manifest, and a
tie goes to the newer release. Drafts, prereleases and the compatibility
`latest` release are skipped.

This script only writes the file: the catalogue is published as an asset of the
compatibility release, which is what the addon requests directly:

    https://github.com/<owner>/<name>/releases/download/latest/index.json

That is `tools/rolling_release.py`'s job -- it copies the current archives and
this file into one release tagged on a fresh commit, so the file and the
archives can never disagree about which version is current. No API call on the
device, no zip downloads, and no rate limit: one request for the whole
catalogue.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

SCHEMA = 1
STORE_TAG = "store"
ROLLING_TAG = "latest"
VERSION_RE = re.compile(r"^[A-Za-z0-9_.-]+-v(\d+(?:\.\d+)*)$")


def gh(*args: str, check: bool = True) -> str:
    result = subprocess.run(["gh", *args], check=False, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def gh_api(path: str) -> list[dict[str, object]]:
    """Every page of a list endpoint, however this gh spells --slurp."""
    try:
        pages = json.loads(gh("api", "--paginate", "--slurp", path))
        return [item for page in pages for item in page]
    except (RuntimeError, json.JSONDecodeError, TypeError):
        pass

    text = gh("api", "--paginate", path)
    decoder = json.JSONDecoder()
    items: list[dict[str, object]] = []
    index = 0
    while index < len(text):
        while index < len(text) and text[index] in " \t\r\n":
            index += 1
        if index >= len(text):
            break
        value, index = decoder.raw_decode(text, index)
        if isinstance(value, list):
            items.extend(item for item in value if isinstance(item, dict))
        elif isinstance(value, dict):
            items.append(value)
    return items


def version_tuple(text: object) -> tuple[int, ...]:
    if not isinstance(text, str):
        return (0,)
    parts = []
    for part in text.split("."):
        match = re.match(r"\d+", part)
        parts.append(int(match.group(0)) if match else 0)
    return tuple(parts) or (0,)


def asset_url(repo: str, tag: str, asset: str) -> str:
    return f"https://github.com/{repo}/releases/download/{tag}/{asset}"


def download(repo: str, tag: str, name: str, into: Path) -> Path | None:
    gh(
        "release",
        "download",
        tag,
        "--repo",
        repo,
        "--pattern",
        name,
        "--dir",
        str(into),
        "--clobber",
        check=False,
    )
    path = into / name
    return path if path.is_file() else None


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def fragment_fields(fragment: dict[str, object]) -> dict[str, object]:
    fields: dict[str, object] = {}
    for key in ("title", "titles", "section", "version"):
        if key in fragment:
            fields[key] = fragment[key]
    return fields


def entry_from_fragment(
    repo: str, tag: str, version: str, fragment: dict[str, object]
) -> dict[str, object] | None:
    addon_id = fragment.get("id")
    asset = fragment.get("asset")
    size = fragment.get("size")
    sha = fragment.get("sha256")
    if (
        not isinstance(addon_id, str)
        or not isinstance(asset, str)
        or not isinstance(size, int)
        or not isinstance(sha, str)
    ):
        return None
    entry: dict[str, object] = {"id": addon_id, "version": version}
    entry.update(fragment_fields(fragment))
    entry.update(
        {
            "tag": tag,
            "asset": asset,
            "size": size,
            "sha256": sha,
            "so_size": fragment.get("so_size", 0),
            "so_sha256": fragment.get("so_sha256", ""),
            "url": asset_url(repo, tag, asset),
        }
    )
    return entry


def entry_from_zip(
    repo: str, tag: str, zip_name: str, version: str, folder: Path
) -> dict[str, object] | None:
    path = download(repo, tag, zip_name, folder)
    if path is None:
        return None
    addon_id = zip_name[:-4]
    fields: dict[str, object] = {"id": addon_id, "version": version}
    so_sha, so_size = "", 0
    try:
        with zipfile.ZipFile(path) as archive:
            if "addon.json" in archive.namelist():
                manifest = json.loads(archive.read("addon.json"))
                if isinstance(manifest, dict):
                    fields.update(fragment_fields(manifest))
            if "addon.so" in archive.namelist():
                library = archive.read("addon.so")
                so_sha, so_size = hashlib.sha256(library).hexdigest(), len(library)
    except (zipfile.BadZipFile, json.JSONDecodeError, OSError):
        return None
    sha, size = sha256_file(path)
    fields.update(
        {
            "tag": tag,
            "asset": zip_name,
            "size": size,
            "sha256": sha,
            "so_size": so_size,
            "so_sha256": so_sha,
            "url": asset_url(repo, tag, zip_name),
        }
    )
    return fields


def build_index(repo: str, workdir: Path) -> dict[str, object]:
    releases = gh_api(f"repos/{repo}/releases?per_page=100")
    best: dict[str, tuple[tuple[int, ...], str, dict[str, object]]] = {}

    for release in releases:
        tag = release.get("tag_name")
        # `store` is this catalogue and `latest` is a copy of the real releases
        # for old clients: neither is a release of its own.
        if not isinstance(tag, str) or tag in (STORE_TAG, ROLLING_TAG):
            continue
        if release.get("draft") or release.get("prerelease"):
            continue
        assets = release.get("assets")
        if not isinstance(assets, list):
            continue
        names = [
            a.get("name")
            for a in assets
            if isinstance(a, dict) and isinstance(a.get("name"), str)
        ]
        created = (
            release.get("created_at")
            if isinstance(release.get("created_at"), str)
            else ""
        )

        for name in names:
            if not name.endswith(".json") or name == "index.json":
                continue
            addon_id = name[:-5]
            fragment_path = download(repo, tag, name, workdir)
            if fragment_path is None:
                continue
            try:
                fragment = json.loads(fragment_path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                continue
            if not isinstance(fragment, dict):
                continue
            match = VERSION_RE.match(tag)
            version = (
                match.group(1) if match and tag.startswith(f"{addon_id}-v") else None
            )
            if not version:
                version = (
                    fragment.get("version")
                    if isinstance(fragment.get("version"), str)
                    else "0"
                )
            entry = entry_from_fragment(repo, tag, str(version), fragment)
            if entry is None:
                continue
            key = (version_tuple(version), created)
            if addon_id not in best or key > (best[addon_id][0], best[addon_id][1]):
                best[addon_id] = (key[0], key[1], entry)

        # The old shape: one tag, every addon's zip, no fragment.
        zip_names = [n for n in names if n.endswith(".zip")]
        if any(n.endswith(".json") for n in names):
            zip_names = []
        for zip_name in zip_names:
            addon_id = zip_name[:-4]
            if addon_id in best:
                continue
            entry = entry_from_zip(repo, tag, zip_name, "0", workdir)
            if entry is None:
                continue
            version = str(entry.get("version") or "0")
            entry["version"] = version
            best[addon_id] = (version_tuple(version), created, entry)

    addons = [best[key][2] for key in sorted(best)]
    return {"schema": SCHEMA, "repo": repo, "addons": addons}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--out", default="build/index.json")
    args = parser.parse_args()

    if "/" not in args.repo:
        print("publish_index: --repo owner/name is required", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        index = build_index(args.repo, Path(tmp))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"publish_index: {len(index['addons'])} addon(s) in {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
