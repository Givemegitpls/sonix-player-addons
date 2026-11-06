# Project notes — sonix-player-addons

## A download that writes the card at network speed stalls the whole player (2026-11-06)

Reproduced on the device with the Yandex addon, and it is not the addon's own
code that is at fault — it is the board's path to the card (`jzmmc`), which is
much slower than the card itself. Do not read the card as the culprit: the same
card through a good USB reader is several times faster, and the player's own
export is slower still (the owner's measurement, 2026-11-06).

Symptom, in the owner's words: "hangs after a few tracks, the interface first
and then the sound", and earlier "feels like a memory leak". It is neither: the
interface thread, the log writer and the playback thread all end up in `D`:

```
123 sonix_player  D  sleep_on_page    do_fsync → fat_file_fsync → filemap_write_and_wait_range
128 logwriter     D  sleep_on_buffer  do_fsync → fat_file_fsync
165 playback      D  sleep_on_page_killable  generic_file_aio_read → __lock_page_killable
2117 yandex download …               balance_dirty_pages_ratelimited
```

and `dmesg` says why:

```
jzmmc_v1.2: timeout 3000ms op:25 w sz:524288  ... blks:19/1024
mmcblk0: unknown error -5 sending read/write command, card status 0x900
end_request: I/O error, dev mmcblk0, sector 21762
FAT-fs (mmcblk0p1): FAT read failed (blocknr 19714)
```

The download writes a 25–45 MB lossless track as fast as the network gives it
(2–4 MB/s of dirty pages); with `MemFree` around 1 MB the kernel throttles the
writer, every `fsync` in the player waits for the pages, and a read that lands
on a page being written back waits too. It can last minutes and then recover by
itself, which is why the same thing has been reported both as a freeze and as
"the player died with the panel dark".

Fix in this repository: `pace_to_card()` in
`yandex/src/system/streaming/yandexcache.c`, 1 MB/s, called from
`note_written()` so raw, unwrapped and detached-save writes are all paced. The
log line `took N s, M KB/s (the card is paced to 1024 KB/s)` confirms it. The
periodic "clear the whole cache every 10 tracks" sweep was removed as well
(`YANDEXCACHE_CLEAR_EVERY 0`) — deleting tens of megabytes in a burst is the
same problem from the other side.

The rate is a floor, not a measurement of the board: 1 MB/s was chosen to leave
the interface, the log and the library journal room on top of the download. What
the board itself can take is still unmeasured — `jzmmc` is the suspect, and the
way to find out is `dd` on the device, not on a host. For the record, the same
card through a generic USB 2.0 reader: 4.0 MB/s write and 6.8 MB/s read
(O_DIRECT, 256 MB), and the reader is the ceiling there, not the card.

That was measured on the device afterwards: 4 bits, sd high-speed, 48 MHz, and
**9 MB/s write / 18 MB/s read**, with no timeouts on a 1 GB sustained write. So
1 MB/s of download leaves most of the card idle.

The run that says the pacing works: 2026-11-06, 18:10–19:49 on the card, 35
tracks, 21 prunes (78 files deleted while playing), **0 `jzmmc` timeouts, 0 I/O
errors**, `available` steady at 21.3–22.3 MB, the largest gap between log lines
10.03 s (the stats cadence), two ALSA underruns that recovered by themselves.
Before the pacing, the same addon on the same card had put the whole process in
`D` (17:19) with `jzmmc … timeout 3000ms` and `FAT read failed`.

Not fixed here, and worth knowing:

- `qobuzcache.c`, `tidalcache.c`, `zvukcache.c` and `subsoniccache.c` do **not**
  pace their writes and have the same exposure — the work item with the exact
  functions to hook is `.opencode/TODO.md`, "Pace the other download caches";
- "no card at all" is not reachable from an addon: the player only plays files,
  `/data` has 36.5 MB free (one lossless track, and it is NAND), `/tmp` and
  `/dev/shm` are 28 MB tmpfs — and the player writes the card anyway
  (`library.db` + journal with an `fsync` on the interface thread, and the log
  when `log_to_sd = 1`).

Diagnostics, before the power button: `dmesg | tail -40`, the `D` states above,
`/proc/<pid>/task/*/stack`, `MemFree/Dirty/Writeback` from `/proc/meminfo`.
Reading the log **during** the stall hangs the shell — it is on the card.
`docs/ADDONS.md`, "The card is the slowest thing in the player", has the long
version.

## Layout of the player submodule

`sonix-player/` is the submodule, but it is the **wrapper** repository
`Givemegitpls/sonix-player-tempotec-v3-analog`, not the player tree. The player's
sources (`src/addon/addon.h`, `docker/Dockerfile`, `rockboxdev/`, `Makefile`,
`tools/addon_exports.sh`) live one level down, at `sonix-player/sonix-player/`.
`PLAYER_DIR` in the Makefile and `docker/build.sh` resolve that; any new path
that reaches into the player's tree needs the double `sonix-player/sonix-player/`.

The player's addon layer exists only on branch `feature/addons-system` (not on
`main`). `.gitmodules` pins the SHA and names the branch.

## CI pitfalls

- `docker/build-push-action@v6` needs `docker/setup-buildx-action@v3` before it:
  the default docker driver cannot export `cache-to: type=gha`.
- `.gitmodules` must keep an `https://` URL — GitHub Actions has no SSH key and
  the repository is public; `git@github.com:` URLs make `submodules: recursive`
  produce an empty checkout (the failure is later, `context ... not found`).

## Docker images

Two, not one:

- the player's `sonix-player/sonix-player/docker/Dockerfile` — Ubuntu 24.04, the
  packages the Rockbox toolchain and the cross-built libraries need. No Rust in
  it at all (it was removed: nothing in the player repo uses cargo/rustc);
- `spotify/engine-mips/docker/Dockerfile` — the same base digest plus pinned
  rustup/nightly + `rust-src` and musl.cc's `mipsel-linux-musl` gcc, for the
  Spotify addon's Rust half only. Built by `make -C spotify/engine-mips`.

The musl.cc tarball is 97 MB and its download needs `wget -c -t 5 -T 60` plus the
size check (`101686600`): a plain `wget` from a CI network dies with exit 4
mid-transfer, which is why the addons CI used to fail at that layer.

## Local build state

`sonix-player/sonix-player/rockbox-toolchain` builds in 20-60 minutes; CI caches
it keyed on `sonix-player/sonix-player/docker/Dockerfile` + `rockboxdev/**`.
