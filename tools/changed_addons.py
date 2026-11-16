#!/usr/bin/env python3
"""Which addons a range of commits touches, and which of them may be released.

    tools/changed_addons.py --base <sha> --head <sha> --release "dino qobuz"
    tools/changed_addons.py --all
    tools/changed_addons.py --base <sha> --github-output "$GITHUB_OUTPUT"

Prints the ids, one per line, or writes `ids=`/`pack=`/`any=` when
--github-output is given. The CI builds `ids` and packs `pack`, which is `ids`
intersected with --release: an addon outside that list still compiles, but is
never put in a release.

Two things are not "one addon changed":

- `common/` is compiled into every addon, and the player's submodule decides
  how they are compiled (flags, headers, ABI), so a change in either means
  every addon;
- `Makefile` and `.gitmodules` decide the same.

`sonix-player` appears in a diff as the submodule path itself, which is why it
is checked as a prefix and as a whole path.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

BUILD_ALL_FILES = {"Makefile", ".gitmodules"}
BUILD_ALL_PREFIXES = ("common/", "sonix-player/")


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, check=True
    ).stdout


def addons() -> list[str]:
    return sorted(p.name for p in Path(".").iterdir() if (p / "addon.json").is_file())


def diff_names(base: str, head: str) -> list[str]:
    try:
        out = git("diff", "--name-only", f"{base}...{head}")
    except subprocess.CalledProcessError:
        out = git("diff", "--name-only", base, head)
    return [line for line in out.splitlines() if line]


def zero_sha(sha: str) -> bool:
    return not sha or set(sha) <= {"0"}


def build_all(path: str) -> bool:
    if path in BUILD_ALL_FILES:
        return True
    if path == "sonix-player":
        return True
    return path.startswith(BUILD_ALL_PREFIXES)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base",
        default="",
        help="commit to diff from; empty or all zero means everything",
    )
    parser.add_argument("--head", default="HEAD")
    parser.add_argument(
        "--release", default="", help="the addons that may be packed or published"
    )
    parser.add_argument(
        "--all", action="store_true", help="every addon regardless of the diff"
    )
    parser.add_argument(
        "--github-output", default="", help="write ids=/pack=/any= there"
    )
    args = parser.parse_args()

    known = addons()
    if args.all or zero_sha(args.base):
        picked = known
        why = "everything"
    else:
        paths = diff_names(args.base, args.head)
        if any(build_all(p) for p in paths):
            picked = known
            why = "common/, Makefile or the player changed"
        else:
            touched = {p.split("/", 1)[0] for p in paths if "/" in p}
            picked = [i for i in known if i in touched]
            why = f"{len(paths)} path(s)"

    releasable = args.release.split()
    pack = [i for i in picked if i in releasable] if releasable else []

    ids = " ".join(picked)
    print(f"changed_addons: {why}: {ids or '(none)'}", file=sys.stderr)
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as out:
            out.write(f"ids={ids}\n")
            out.write(f"pack={' '.join(pack)}\n")
            out.write(f"any={'true' if picked else 'false'}\n")
    else:
        print(ids)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
