#!/usr/bin/env python3
"""The one compatibility release: every current archive plus the catalogue.

Store builds from before per-addon tags ask GitHub for the single newest release
and treat its `.zip` assets as the catalogue. The same release is where the
catalogue file itself lives -- `index.json`, which the current store fetches
instead of walking the release list. So one release, tagged `latest`, carries
both.

Ordering is the reason this file exists at all. GitHub dates a release by the
commit its tag points at (`created_at` is the ref's date, not the moment the
release was made), and two releases dated the same second are ordered by tag
name, descending. A bundle sharing a commit with an addon release therefore
loses to it -- `yandex-v0.2.0` sorts above `latest`, and the release tagged
`store` used to sit above it for the same reason, leaving old clients with a
catalogue file and no archives. The tag is therefore put on a fresh commit of
the `releases` branch: no addon release can be newer than a commit made after
all of them.

Nothing is rebuilt or repacked: each archive is downloaded from the release that
publishes it and uploaded byte for byte, so its size and `sha256` are exactly
what they were, and a client that is already current is offered nothing.

    tools/rolling_release.py --repo owner/name --index build/index.json --dry-run
    tools/rolling_release.py --repo owner/name --index build/index.json --publish

`--publish` deletes the previous `latest` release (with its tag), commits
`index.json` on a fresh `releases` commit, tags that commit and creates the
release there. The catalogue (`index.json`) and the store's own API fallback
both skip this tag: it is a copy of the real releases, not a release of its own.

Disable it with the repository variable ROLLING_RELEASE=off.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROLLING_TAG = "latest"
RELEASES_BRANCH = "releases"
INDEX_NAME = "index.json"
NOTES = (
    "Every addon's current archive in one release, and the store's catalogue "
    "(`index.json`) for the builds that read it. The files are copies: their "
    "size and sha256 are the ones of the releases they came from."
)


def run(argv: list[str], cwd: Path) -> str:
    """Run a command that is not `gh`, without echoing an argument list that may
    carry the token."""
    result = subprocess.run(argv, cwd=cwd, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{argv[0]} {argv[1] if len(argv) > 1 else ''}: {result.stderr.strip()}"
        )
    return result.stdout


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


def load_entries(index_path: Path) -> list[dict[str, object]]:
    if not index_path.is_file():
        raise RuntimeError(
            f"no catalogue at {index_path} (tools/publish_index.py writes it)"
        )
    doc = json.loads(index_path.read_text(encoding="utf-8"))
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


def token() -> str:
    value = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
    if not value:
        raise RuntimeError("GH_TOKEN is required to publish")
    return value


def commit_catalogue(repo: str, index_path: Path) -> str:
    """One fresh commit carrying `index.json`, so the release's date is now.

    The branch is pushed with --force: its history is not read by anything, and
    one commit per release is enough to keep the git tree small.
    """
    url = f"https://x-access-token:{token()}@github.com/{repo}.git"
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp)
        shutil.copy(index_path, tree / INDEX_NAME)
        run(["git", "init", "--quiet", "--initial-branch", RELEASES_BRANCH], tree)
        run(["git", "add", INDEX_NAME], tree)
        run(
            [
                "git",
                "-c",
                "user.name=github-actions[bot]",
                "-c",
                "user.email=41898282+github-actions[bot]@users.noreply.github.com",
                "commit",
                "--quiet",
                "-m",
                "catalogue: the current release of every addon",
            ],
            tree,
        )
        run(["git", "tag", ROLLING_TAG], tree)
        sha = run(["git", "rev-parse", "HEAD"], tree).strip()
        run(
            [
                "git",
                "push",
                "--force",
                "--quiet",
                url,
                f"HEAD:refs/heads/{RELEASES_BRANCH}",
            ],
            tree,
        )
        run(
            ["git", "push", "--force", "--quiet", url, f"refs/tags/{ROLLING_TAG}"], tree
        )
    return sha


def publish(repo: str, files: list[Path], index_path: Path) -> None:
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
    # A release that is still there would get the new tag (a release is not moved
    # by re-pointing its tag), so the next step is doomed: say why instead of
    # "release already exists".
    if gh("release", "view", ROLLING_TAG, "--repo", repo, check=False).strip():
        raise RuntimeError(f"the previous {ROLLING_TAG} release could not be deleted")
    sha = commit_catalogue(repo, index_path)
    ref = json.loads(gh("api", f"repos/{repo}/git/ref/tags/{ROLLING_TAG}"))
    pointed = ref.get("object", {}).get("sha") if isinstance(ref, dict) else None
    if pointed != sha:
        raise RuntimeError(
            f"the {ROLLING_TAG} tag points at {pointed}, not at the new commit {sha}"
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
        NOTES,
        *[str(path) for path in files],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument(
        "--index",
        default="build/index.json",
        help="the catalogue tools/publish_index.py wrote",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="fetch and check, but do not touch the release",
    )
    parser.add_argument(
        "--publish", action="store_true", help=f"recreate the `{ROLLING_TAG}` release"
    )
    args = parser.parse_args()

    if "/" not in args.repo:
        print("rolling_release: --repo owner/name is required", file=sys.stderr)
        return 1

    index_path = Path(args.index)
    try:
        entries = load_entries(index_path)
        with tempfile.TemporaryDirectory() as tmp:
            workdir = Path(tmp)
            files = [fetch_copy(args.repo, entry, workdir) for entry in entries]
            total = sum(path.stat().st_size for path in files)
            print(
                f"rolling_release: {len(files)} archive(s), {total} bytes, "
                "verified against the catalogue"
            )
            if args.dry_run or not args.publish:
                return 0
            publish(args.repo, [*files, index_path], index_path)
        print(f"rolling_release: {ROLLING_TAG} release recreated")
    except RuntimeError as failure:
        print(f"rolling_release: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
