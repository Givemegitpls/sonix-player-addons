#!/usr/bin/env python3
"""What it takes to turn a page or a module of the player into an addon.

An addon is a separate `.so` on the card (docs/ADDONS.md). Moving an existing
feature into one is mostly mechanical -- the sources keep their includes, the
player exports what they call, the tile keeps its place -- but the answer to
"does this belong on a card at all?" is written in the feature's own code: how
many places in the player call into it, whether it keeps state in the player's
configuration, whether it has to clean up on exit (an addon has no `deinit`).

This tool reads a set of sources and says all of that before anyone moves a
file:

    python3 tools/addon_port.py spotify \\
        --src src/system/streaming/spotify.c \\
        --src src/gui/streaming/spotifypage.c \\
        --section streaming

It answers, in order:

  1. how much code it is, and how much of the tree it drags along (`#include`
     of files outside the set);
  2. which of its callbacks into the player are not symbols the player has --
     a real blocker, unlike "the player has it but does not export it", which
     `addons/build/exports.list` covers automatically;
  3. what state it keeps where: `config_get*/config_set*` keys (those have to
     move to the card, `addons/common/store.h`), sealed stream keys, absolute
     paths;
  4. how it plays audio (`radio_play` is portable, `audio_play(file)` and a
     download cache are not -- see docs/ADDONS.md);
  5. what it expects from the lifecycle: `switch_screen`, `SCREEN_LOADED`,
     threads, and anything named `*_clear_on_exit`/`*_deinit`, which the addon
     ABI does not have (an addon is never unloaded);
  6. how many places in the rest of the player call into it -- the number that
     decides "app" (a page and its state) against "service" (woven into Now
     Playing, the playlists, the power manager);
  7. with --scaffold: `addons/<id>/addon.json`, `addon.mk` and an `addon.c`
     stub, plus the `git mv` lines, and it prints rather than does.

Symbol work needs the tree built (`make target`, or docker/build.sh): it reads
`build_target/*.o` with the cross `nm`. Without it the report says so and skips
that section rather than guessing.

    --no-objects    skip the symbol analysis on purpose
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

TOOLCHAIN = "rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-"
# Interface directories: everything here is exported to an addon for free
# (addons/exports-base.sh lists them). A call into anything else is a symbol the
# player must define and the addon's own UND list will publish -- automatically,
# which is why it is not a blocker either.
INTERFACE_DIRS = (
    "gui/shell",
    "gui/fonts",
    "system/core",
    "system/net",
    "system/image",
    "system/streaming",
    "system/audio",
    "system/library",
    "system/playback",
    "system/device",
    "addon",
)
# Library symbols the device provides; asked of the toolchain's own libc when it
# is there, guessed at when it is not.
FALLBACK_LIBC = {
    "malloc", "free", "calloc", "realloc", "memcpy", "memmove", "memset", "memcmp", "strlen",
    "strcmp", "strncmp", "strcpy", "strncpy", "strcat", "strncat", "strchr", "strrchr", "strstr",
    "strdup", "strtod", "strtol", "strtoul", "strtoll", "snprintf", "sprintf", "printf", "fprintf",
    "vprintf", "vfprintf", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "fflush",
    "fgets", "fputs", "fputc", "fgetc", "feof", "ferror", "fileno", "stat", "fstat", "lstat",
    "mkdir", "unlink", "rename", "remove", "access", "open", "close", "read", "write", "lseek",
    "opendir", "readdir", "closedir", "pthread_create", "pthread_join", "pthread_detach",
    "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_init", "pthread_mutex_destroy",
    "pthread_cond_wait", "pthread_cond_signal", "pthread_once", "usleep", "sleep", "time",
    "clock_gettime", "gettimeofday", "localtime", "localtime_r", "strftime", "mktime", "rand",
    "srand", "abs", "labs", "qsort", "powf", "sqrtf", "sinf", "cosf", "fabsf", "fmodf", "roundf",
    "lroundf", "floorf", "ceilf", "expf", "logf", "pow", "sqrt", "fabs", "floor", "ceil",
    "dlerror", "dlopen", "dlsym", "dlclose", "ioctl", "poll", "select", "fcntl", "getenv", "setenv",
    "exit", "abort", "assert_fail", "isspace", "isdigit", "isalpha", "tolower", "toupper",
}


def run(cmd: list[str], **kw) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=False, **kw).stdout
    except OSError:
        return ""


class Report:
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.verdict_reasons: list[str] = []

    def head(self, text: str) -> None:
        self.lines.append("")
        self.lines.append(text)
        self.lines.append("-" * len(text))

    def say(self, text: str = "") -> None:
        self.lines.append(text)

    def reason(self, text: str) -> None:
        self.verdict_reasons.append(text)

    def out(self) -> str:
        return "\n".join(self.lines).lstrip("\n")


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
def collect(tree: Path, given: list[str]) -> tuple[list[Path], list[Path]]:
    """The C and C++ files of the set, and whatever else was named."""
    sources: list[Path] = []
    others: list[Path] = []
    for item in given:
        path = (tree / item).resolve()
        if path.is_dir():
            for f in sorted(path.rglob("*")):
                if f.suffix in (".c", ".cpp"):
                    sources.append(f)
                elif f.suffix in (".h", ".hpp"):
                    others.append(f)
        elif path.is_file():
            (sources if path.suffix in (".c", ".cpp") else others).append(path)
        else:
            print(f"addon_port: {item}: no such file or directory", file=sys.stderr)
    return sources, others


def line_count(path: Path) -> int:
    try:
        return sum(1 for _ in path.open("rb"))
    except OSError:
        return 0


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def resolve_include(tree: Path, include: str, origin: Path) -> Path | None:
    for base in (origin.parent, tree, tree / "src", tree / "addons"):
        candidate = (base / include).resolve()
        if candidate.is_file():
            return candidate
    return None


def includes_outside(tree: Path, group: set[Path]) -> dict[Path, list[Path]]:
    """Which files outside the set are included, and by whom."""
    outside: dict[Path, list[Path]] = defaultdict(list)
    for src in sorted(group):
        if src.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        for include in INCLUDE_RE.findall(src.read_text(errors="ignore")):
            target = resolve_include(tree, include, src)
            if target and target not in group and tree in target.parents:
                if src not in outside[target]:
                    outside[target].append(src)
    return outside


HEADER_SYMBOL_RE = re.compile(r"\b([a-z_][a-z0-9_]{5,})\s*\(", re.IGNORECASE)
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([a-zA-Z_][a-zA-Z0-9_]*)")


def module_headers(sources: list[Path]) -> list[Path]:
    """The module's own headers, and only those.

    Its own <x>.h beside each <x>.c, plus a header in the same directory that one
    of the sources includes ("metronomepage.c" including "metronome.h" is still
    the module; "system/audio/audio.h" is the player's and is not). Taking every
    header of the directory instead pulls in the neighbours' -- and then "who
    calls into it" counts the whole player.
    """
    found: list[Path] = []

    def add(header: Path) -> None:
        if header not in found and header.is_file():
            found.append(header)

    for src in sources:
        add(src.with_suffix(".h"))
        add(src.with_suffix(".hpp"))
        for include in INCLUDE_RE.findall(src.read_text(errors="ignore")):
            candidate = (src.parent / include).resolve()
            if candidate.suffix in (".h", ".hpp") and candidate.parent == src.parent.resolve():
                add(candidate)
    return found


def header_symbols(headers: list[Path]) -> set[str]:
    """Names the module publishes: what the rest of the player may be calling."""
    names: set[str] = set()
    for header in headers:
        text = header.read_text(errors="ignore")
        names.update(m.lower() for m in HEADER_SYMBOL_RE.findall(text))
        names.update(m for m in DEFINE_RE.findall(text) if len(m) >= 6)
    # Keywords and obvious noise that the regex above cannot tell from a call.
    names -= {
        "define", "include", "ifndef", "ifdef", "endif", "return", "sizeof", "static", "const",
        "struct", "typedef", "switch", "while", "for", "else", "extern", "inline", "volatile",
        "unsigned", "signed", "default", "break", "continue", "goto",
    }
    return names


# ---------------------------------------------------------------------------
# Symbols
# ---------------------------------------------------------------------------
def tool(name: str) -> str | None:
    local = TOOLCHAIN + name
    if Path(local).is_file() and os.access(local, os.X_OK):
        return local
    return shutil.which("mipsel-rockbox-linux-gnu-" + name)


def object_for(tree: Path, source: Path) -> Path | None:
    try:
        rel = source.relative_to(tree / "src")
    except ValueError:
        return None
    build = tree / "build_target" / rel
    candidate = build.with_suffix(".opp" if source.suffix == ".cpp" else ".o")
    return candidate if candidate.is_file() else None


def nm_symbols(nm: str, files: list[Path], defined: bool) -> set[str]:
    if not files:
        return set()
    flag = "--defined-only" if defined else "-u"
    out = run([nm, "-g" if defined else "", *([flag] if defined else ["-u"]), *map(str, files)])
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1].split("@")[0]
        if name and not name.startswith("."):
            symbols.add(name)
    return symbols


def libc_symbols(tree: Path) -> set[str]:
    readelf = tool("readelf")
    libs = [
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot/lib/libc.so.6",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot/lib/libm.so.6",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot/lib/libpthread.so.0",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot/lib/librt.so.1",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot/lib/libdl.so.2",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/lib/libstdc++.so.6",
        tree / "rockbox-toolchain/mipsel-rockbox-linux-gnu/lib/libgcc_s.so.1",
    ]
    present = [lib for lib in libs if lib.is_file()]
    if not readelf or not present:
        return set(FALLBACK_LIBC)
    names = set(FALLBACK_LIBC)
    for lib in present:
        for line in run([readelf, "--dyn-syms", "-W", str(lib)]).splitlines():
            parts = line.split()
            if len(parts) >= 8 and parts[6] != "UND":
                names.add(parts[7].split("@")[0])
    return names


def defined_map(tree: Path, nm: str) -> dict[str, Path]:
    """symbol -> the object that defines it, for the whole build."""
    objects = sorted((tree / "build_target").rglob("*.o")) + sorted((tree / "build_target").rglob("*.opp"))
    if not objects:
        return {}
    out = run([nm, "-g", "--defined-only", *map(str, objects)])
    mapping: dict[str, Path] = {}
    current: Path | None = None
    for line in out.splitlines():
        if not line.strip():
            continue
        if line.endswith(":"):
            current = Path(line[:-1])
            continue
        parts = line.split()
        if current is not None and len(parts) >= 3:
            name = parts[-1].split("@")[0]
            if name and not name.startswith("."):
                mapping.setdefault(name, current)
    return mapping


def in_base(tree: Path, obj: Path) -> bool:
    """True for an object whose symbols an addon gets without anything being done.

    The published base (addons/exports.base, docs/ADDONS.md) is the shell's own
    widgets and fonts, core/net/image/streaming/audio/library/playback/device --
    and LVGL. A symbol from anywhere else still works: the addons' own UND list is
    appended to the player's --dynamic-list, so `make addons && make target`
    exports it. That is worth two different sentences in the report, because only
    the first group is "no work at all".
    """
    try:
        rel = obj.relative_to(tree / "build_target")
    except ValueError:
        return False
    parts = rel.parts
    if parts[0] == "lvgl":
        return True
    return len(parts) > 1 and f"{parts[0]}/{parts[1]}" in INTERFACE_DIRS


def symbol_report(tree: Path, report: Report, sources: list[Path], no_objects: bool) -> None:
    report.head("Symbols")
    if no_objects:
        report.say("skipped (--no-objects)")
        return

    nm = tool("nm")
    objects = {src: object_for(tree, src) for src in sources}
    built = [obj for obj in objects.values() if obj]
    missing_objects = [src for src, obj in objects.items() if not obj]
    if not nm or not built:
        report.say("no objects: build the player first (make target), then run this again.")
        report.say("Without that this section cannot tell a blocker from an export the build adds by itself.")
        return
    if missing_objects:
        report.say(f"{len(missing_objects)} source(s) have no object under build_target, skipped:")
        for src in missing_objects[:6]:
            report.say(f"  {src.relative_to(tree)}")
        report.say("")

    group_defined = nm_symbols(nm, built, defined=True)
    group_undefined = nm_symbols(nm, built, defined=False)
    where = defined_map(tree, nm)
    libc = libc_symbols(tree)

    unresolved = group_undefined - group_defined - libc
    blockers = sorted(sym for sym in unresolved if sym not in where)
    foreign = sorted(sym for sym in unresolved if sym in where and not in_base(tree, where[sym]))
    known = sorted(sym for sym in unresolved if sym in where and in_base(tree, where[sym]))

    report.say(f"in the published base, nothing to do ({len(known)}): LVGL, the shell's widgets and fonts,")
    report.say("  core/net/image/streaming/audio/library/playback/device")
    report.say("")
    report.say(f"defined in the player but outside that base ({len(foreign)}):")
    if foreign:
        for name in foreign:
            report.say(f"  {name:32s} {where[name].relative_to(tree / 'build_target')}")
        report.say("  Exported automatically for this tree's addons: addons/build/exports.list. Run")
        report.say("  `make addons` then `make target`; `make addons-checks` proves it.")
    else:
        report.say("  none")
    report.say("")
    report.say(f"defined nowhere in the player ({len(blockers)}):")
    if blockers:
        for name in blockers:
            report.say(f"  {name}")
        report.say("  These have to come into the addon itself: a copy of the function, the library it lives in,")
        report.say("  or a file that joins the module. Nothing in the player can export them.")
        report.reason(f"{len(blockers)} symbol(s) come from outside the player: they must be built into the addon")
    else:
        report.say("  none")

# ---------------------------------------------------------------------------
# The rest of the checks: state, audio, lifecycle, who calls it
# ---------------------------------------------------------------------------
CONFIG_RE = re.compile(r'config_(?:get|set)_\w*\(\s*"([^"]+)"\s*,\s*"([^"]+)"')
AUDIO_RE = {
    "radio_play": "streams through the player's transport (portable)",
    "radio_station_problem": "checks a station before playing (portable)",
    "audio_play": "plays a local FILE through the player's queue (needs the file on the card first)",
    "audio_external_begin": "takes the DAC directly (games, metronome)",
    "audio_external_begin_latency": "takes the DAC directly, short buffer (games)",
    "audio_external_write": "writes frames to the DAC itself",
    "play_file": "the player's own file playback",
    "playlist_": "feeds the player's queue",
}
LIFECYCLE_NOTES = (
    ("switch_screen", "navigates by itself — the addon owns its screens, fine"),
    ("SCREEN_LOADED", "reloads something when the page appears — the addon's init() runs once, so this is where "
                      "per-visit state belongs"),
    ("SCREEN_UNLOADED", "frees something when the page goes away — an addon is not unloaded, but its SCREEN_UNLOADED "
                        "still fires"),
    ("pthread_create", "has a thread of its own — every lv_* call from it must go through gui_post()"),
    ("lv_async_call", "uses lv_async_call() — gui_post() is the way back to the UI thread"),
)
CLEANUP_RE = re.compile(r"\b[a-z_0-9]*(?:clear_on_exit|_deinit|deinit)\b")


def state_and_lifecycle(tree: Path, report: Report, sources: list[Path]) -> None:
    keys: Counter[tuple[str, str]] = Counter()
    audio: Counter[str] = Counter()
    lifecycle: dict[str, list[str]] = defaultdict(list)
    cleanup: list[str] = []
    hard_paths: list[str] = []
    sealed: list[str] = []

    # The sources, not the headers: a comment in a player header saying
    # "audio_play()" is not the module calling it.
    for src in sorted(sources):
        text = src.read_text(errors="ignore")
        rel = src.relative_to(tree)
        for section, key in CONFIG_RE.findall(text):
            keys[(section, key)] += 1
        for needle in AUDIO_RE:
            if needle.endswith("_"):
                count = len(re.findall(r"\b" + needle, text))
            else:
                count = len(re.findall(r"\b" + re.escape(needle) + r"\b", text))
            if count:
                audio[needle] += count
        for needle, _ in LIFECYCLE_NOTES:
            count = len(re.findall(r"\b" + needle, text))
            if count:
                lifecycle[needle].append(f"{rel} ({count})")
        for match in CLEANUP_RE.findall(text):
            cleanup.append(f"{rel}: {match}")
        for path in re.findall(r'"(/(?:data|usr|var)[^"]*)"', text):
            hard_paths.append(f"{rel}: {path}")
        if "streamkeys_" in text:
            sealed.append(str(rel))

    report.head("State it keeps")
    if keys:
        report.say("keys in the player's device_config.ini — these have to move to the card:")
        for (section, key), count in sorted(keys.items()):
            report.say(f'  [{section}] {key}   ({count} use{"s" if count > 1 else ""})')
        report.say("  -> addons/common/store.h, file <host->dir>/<something>.ini")
        report.reason(f'keeps {len(keys)} setting(s) in device_config.ini — they must move to the card')
    else:
        report.say("  none in device_config.ini")
    if sealed:
        report.say("")
        report.say("uses sealed stream keys (streamkeys_*): " + ", ".join(sealed))
        report.say("  the key is compiled into the player; an addon can carry it only when this tree builds it.")
        report.say("  For a client from another fork, plan on the user importing a token instead.")
        report.reason("needs sealed service keys — token import is the only way in")
    if hard_paths:
        report.say("")
        report.say("absolute device paths (an addon's own data belongs under host->dir or host->sd_root):")
        for line in hard_paths[:10]:
            report.say(f"  {line}")
        report.reason("writes to absolute device paths")

    report.head("Audio")
    if audio:
        for needle, count in sorted(audio.items()):
            report.say(f"  {needle} x{count} — {AUDIO_RE[needle]}")
        if any(k in audio for k in ("audio_play", "play_file")) and "radio_play" not in audio:
            report.reason("plays files rather than streams: needs the cache-and-play path or a rework")
    else:
        report.say("  no audio at all")

    report.head("Lifecycle")
    for needle, note in LIFECYCLE_NOTES:
        if needle in lifecycle:
            report.say(f"  {needle}: {note}")
            report.say("    " + ", ".join(lifecycle[needle][:4]))
    if cleanup:
        report.say("")
        report.say("  runs cleanup on the way out (the addon ABI has no deinit):")
        for line in cleanup[:8]:
            report.say(f"    {line}")
        report.say("    An addon is never unloaded. Do the work after each write, or register the callback the")
        report.say("    player already calls (audio_external_release_cb() is the one that exists).")
        report.reason("has teardown code with nowhere to run")


def callers(tree: Path, report: Report, group: set[Path], symbols: set[str]) -> None:
    report.head("Who calls into it")
    if not symbols:
        report.say("no header symbols found to look for")
        return
    rg = shutil.which("rg")
    files = [p for p in tree.joinpath("src").rglob("*") if p.suffix in (".c", ".h", ".cpp") and p not in group]
    hits: Counter[Path] = Counter()
    # Call sites only, whole words only: an unanchored alternation counted "rss"
    # inside "address" and turned a page into a service.
    words = sorted(s for s in symbols if "_" in s and len(s) >= 6)
    if not words:
        report.say("nothing distinctive enough in its headers to look for")
        return
    if rg:
        pattern = r"\b(?:" + "|".join(re.escape(w) for w in words) + r")\s*\("
        out = run([rg, "--no-heading", "-o", "-e", pattern, *map(str, files)])
        for line in out.splitlines():
            path = line.split(":", 1)[0]
            hits[Path(path)] += 1
    else:
        pattern = re.compile(r"\b(?:" + "|".join(re.escape(w) for w in words) + r")\s*\(")
        for path in files:
            hits[path] += len(pattern.findall(path.read_text(errors="ignore")))
    ranked = [(path, count) for path, count in hits.most_common(15) if count]
    if not ranked:
        report.say("nothing outside the set mentions it: a page of its own, ready to move")
        return
    total = sum(hits.values())
    for path, count in ranked:
        report.say(f"  {count:4d}  {path.relative_to(tree)}")
    report.say(f"  {total} references in {len(hits)} file(s)")
    if total > 200:
        report.reason(f"{total} references from the rest of the player: a service, not a page")
    elif total > 60:
        report.reason(f"{total} references from the rest of the player: check what they do before moving it")
    else:
        report.reason(f"{total} references from the rest of the player: small enough to move")


# ---------------------------------------------------------------------------
# Scaffold
# ---------------------------------------------------------------------------
ADDON_JSON = """{{
  "api": 1,
  "title": "{title}",
  "section": "{section}",{order}
  "version": "0.1",
  "icon": "icon.png"
}}
"""

ADDON_MK = """# {id} as an addon.
#
# The sources are under src/ keeping the paths they have in the player, so that
# no #include inside them had to be rewritten: the addon's own folder is on the
# include path and from there the tree looks like the player's.
{id}_CFLAGS = -I$(CURDIR)/{id}

# Flags this addon needs and the others do not belong here, with the addon's
# name as the prefix (docs/ADDONS.md, "Building"):
#
# {id}_CXXFLAGS = ...
# {id}_LIBS = ...
# {id}_LD_target = $(CROSS_CXX) -static-libstdc++ -static-libgcc
"""

ADDON_C = """#include "src/addon/addon.h"

// {title}, as an addon.
//
// init() runs once, on the first tap on the tile, and returns the screen the
// shell switches to -- it must not switch there itself: a second switch to the
// screen already up is a no-op in LVGL, and anything the page does from its
// SCREEN_LOADED (hiding the status bar, say) then never happens.
//
// Everything the page keeps for itself goes on the card: <host->dir> for its
// settings (addons/common/store.h) and its files (host->sd_root for the user's
// own).
{candidates}
static lv_obj_t *{id}_init(const sonix_addon_host_t *host) {{
	(void)host;
	return NULL; // TODO: build the page's screens and return the one the tile opens
}}

static const sonix_addon_t {id} = {{
	.api = SONIX_ADDON_API,
	.title = "{title}",
	.init = {id}_init,
}};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {{
	return api == SONIX_ADDON_API ? &{id} : NULL;
}}
"""


def scaffold(tree: Path, report: Report, addon_id: str, sources: list[Path], section: str,
             order: int | None) -> None:
    report.head("Scaffold")
    addon_dir = tree / "addons" / addon_id
    if addon_dir.exists():
        report.say(f"{addon_dir.relative_to(tree)} is already there; nothing written")
        return
    addon_dir.mkdir(parents=True)
    title = addon_id[:1].upper() + addon_id[1:]
    order_line = f'\n  "order": {order},' if order is not None else ""
    (addon_dir / "addon.json").write_text(
        ADDON_JSON.format(title=title, section=section, order=order_line))
    (addon_dir / "addon.mk").write_text(ADDON_MK.format(id=addon_id))

    # What init() is likely to call: the *_init() of the page, if the set has one.
    candidates = []
    for src in sources:
        text = src.read_text(errors="ignore")
        for name in re.findall(r"\b([a-z_][a-z0-9_]*_init)\s*\(", text):
            candidates.append(name)
    unique = sorted(set(candidates))
    comment = ""
    if unique:
        comment = "\n".join(f"// {name}() is one of the page's own entry points: init(host) is where it belongs."
                            for name in unique[:3])
        comment += "\n"
    (addon_dir / "addon.c").write_text(ADDON_C.format(id=addon_id, title=title, candidates=comment))

    report.say(f"wrote {addon_dir.relative_to(tree)}/{{addon.c,addon.mk,addon.json}}")
    report.say("")
    report.say("move the sources so that their includes keep working:")
    for src in sources:
        try:
            rel = src.relative_to(tree)
        except ValueError:
            rel = src
        report.say(f"  git mv {rel} addons/{addon_id}/{rel}")
    report.say("then delete the tile from src/gui/shell/{morepage,streaming}.c, the screen from")
    report.say("src/gui/shell/gui.c, and run: make addons && make target && make addons-checks")


# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report what moving a page or module into an addon involves.",
        epilog="Run from sonix-player/ (or pass --tree). See docs/ADDONS.md, .opencode/skills/sonix-addon-port.")
    parser.add_argument("id", help="the addon's id: its folder name under addons/, lower case")
    parser.add_argument("--src", action="append", required=True, metavar="PATH",
                        help="a source file or a whole directory, relative to the tree; repeatable")
    parser.add_argument("--section", choices=("more", "streaming"), default="more",
                        help="which page the tile goes on (default: more)")
    parser.add_argument("--order", type=int, help="the tile's place on that page, 1-based")
    parser.add_argument("--tree", default=".", help="the sonix-player tree (default: .)")
    parser.add_argument("--scaffold", action="store_true",
                        help="write addons/<id>/{addon.c,addon.mk,addon.json} and print the git mv lines")
    parser.add_argument("--no-objects", action="store_true", help="skip the symbol analysis")
    args = parser.parse_args()

    tree = Path(args.tree).resolve()
    if not (tree / "src").is_dir():
        print(f"addon_port: {tree} does not look like sonix-player (no src/)", file=sys.stderr)
        return 2

    sources, headers = collect(tree, args.src)
    headers = list(dict.fromkeys(headers + module_headers(sources)))
    if not sources:
        print("addon_port: nothing to look at", file=sys.stderr)
        return 2

    group: set[Path] = set(sources) | set(headers)
    report = Report()

    report.say(f"porting {' + '.join(args.src)} to addons/{args.id} ({args.section})")
    report.head("Size")
    total_lines = sum(line_count(p) for p in sources)
    c_files = [p for p in sources if p.suffix == ".c"]
    cxx_files = [p for p in sources if p.suffix == ".cpp"]
    report.say(f"  {len(sources)} source(s), {total_lines} lines "
               f"({len(c_files)} C, {len(cxx_files)} C++), {len(headers)} header(s)")
    if cxx_files:
        report.say("  C++: the addon is linked by g++ with -static-libstdc++ on the target — see")
        report.say("  addons/gearboy/addon.mk for a working set of flags")
        report.reason("C++: works, but the library carries its own libstdc++ (a few MB)")

    outside = includes_outside(tree, group)
    report.head("It drags these along")
    if outside:
        private, base_headers = [], []
        for target, users in sorted(outside.items()):
            line = f"  {target.relative_to(tree)}   <- {', '.join(u.name for u in users[:3])}"
            try:
                parts = target.relative_to(tree / "src").parts
            except ValueError:
                parts = ()
            if (parts and parts[0] == "lvgl") or (len(parts) > 1 and f"{parts[0]}/{parts[1]}" in INTERFACE_DIRS):
                base_headers.append(line)
            else:
                private.append(line)
        report.say(f"  already in what the player publishes ({len(base_headers)}): the shell's widgets and")
        report.say("  fonts, core, net, image, audio, library, playback, device — included as today")
        if private:
            report.say("")
            report.say(f"  outside that base ({len(private)}): decide about each one")
            for line in private:
                report.say(line)
            report.say("  A header of the module itself moves with it. Anything else is exported too (the")
            report.say("  symbol section says) or has to become part of the addon.")
    else:
        report.say("  nothing: the set is self-contained")

    symbol_report(tree, report, sources, args.no_objects)
    state_and_lifecycle(tree, report, sources)
    callers(tree, report, group, header_symbols(headers))

    if args.scaffold:
        scaffold(tree, report, args.id, sources, args.section, args.order)

    report.head("Verdict")
    if report.verdict_reasons:
        for reason in report.verdict_reasons:
            report.say(f"  - {reason}")
        report.say("")
        report.say("  Apps move as they are (Gearboy, Flappy Bird: addons/gearboy, addons/flappybird).")
        report.say("  A service woven into the player needs a decision first: keep the service in the binary")
        report.say("  and move only its pages, or add a small interface in the player (docs/ADDONS.md,")
        report.say('  "Porting a service page").')
    else:
        report.say("  a page of its own: move it, keep its tile place with `order`, put its state on the card")

    print(report.out())
    return 0


if __name__ == "__main__":
    sys.exit(main())
