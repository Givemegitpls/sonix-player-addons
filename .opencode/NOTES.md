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

**2026-11-07: the write rate is not what stalls the card; read/write
interleaving is.** Measured on the device with the player idle (`dd` models,
`dmesg | grep -c "timeout 3000ms"` before/after, 60 s each):

| writer | reader | timeouts |
|---|---|---|
| 32 KB every 30 ms (1 MB/s) | same file, 32 KB blocks, unthrottled | 0 |
| 64 KB every 62 ms (1 MB/s) | same file, 64 KB, unthrottled | 0 |
| 512 KB every 0.5 s (1 MB/s) | same file, 512 KB, unthrottled | **5** |
| 64 KB every 62 / 125 / 250 ms | ~300 KB/s (playback rate) | 0 |
| 2 writers, 64 KB every 62 ms (2 MB/s total) | ~300 KB/s | 0 |
| 64 KB every 15.5 ms (4 MB/s, unpaced) | ~300 KB/s | 0 |
| 64 KB every 62 ms + log-like appends every 50 ms | ~300 KB/s | 0 |
| none | 512 KB or 64 KB blocks, unthrottled | 0 |
| 32 KB every 30 ms | none | 0 |

So neither the rate nor the grain is the trigger on its own: writes alone are
clean at 4 MB/s, reads alone are clean, and writes plus playback-rate reads are
clean at every rate tried. The one configuration that stalls is a writer racing
an *unthrottled* reader that reads in 512 KB requests — large read bursts keep
the queue busy and a multi-block write is not serviced within the driver's 3 s.
On the real device that can only come from the player's own read bursts (decode
readahead, thumbnail or library scan), not from the addon's writes.

The card is healthy: 100 MB `dd … conv=fsync` through a USB reader on the host
25.8 MB/s, on the device 5.4 MB/s, both with zero timeouts.

Also fixed on 2026-11-07: the original `pace_to_card()` computed
`written * 1000L` in a 32-bit `long`, which overflows at 2.1 MB on mipsel — the
deadline wrapped into the past, the pause stopped, and the rest of every file
went to the card at network speed (a 39 MB track reported 2035 KB/s against a
1024 KB/s limit). `common/cardpace.c` now uses `long long`, and a real qobuz
download reports exactly 1023 KB/s.

The download writes a 25–45 MB lossless track as fast as the network gives it
(2–4 MB/s of dirty pages); with `MemFree` around 1 MB the kernel throttles the
writer, every `fsync` in the player waits for the pages, and a read that lands
on a page being written back waits too. It can last minutes and then recover by
itself, which is why the same thing has been reported both as a freeze and as
"the player died with the panel dark".

Fix in this repository: `common/cardpace.h` / `common/cardpace.c`, 1 MB/s, one
`cardpace_t` per download — `cardpace_reset()` before it starts writing, then
`cardpace_note(&d->pace, bytes_so_far)` after every write of track audio. In
`yandexcache.c` it is called from `note_written()` so raw, unwrapped and
detached-save writes are all paced; qobuz, tidal (FILE and DASH), zvuk and
subsonic call it from their own write choke points. The
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

## Docs

- `docs/SETUP.md` is the user-facing side (install + per-service sign-in),
  bilingual: every section is English first, then the same in Russian. Keep the
  two halves in step; `docs/ADDONS.md` stays English and is for internals.
- Release zips are `qobuz subsonic yandex gearboy flappybird dino`; `hello` is
  built by CI but `PACK_EXCLUDE ?= hello` keeps it out of `build/Addons`, so
  there is no `hello.zip`. The archive holds the addon folder's contents at its
  root (`addon.json`, `addon.so`, `icon.png`, optional `assets/`).
- Qobuz token for `Addons/qobuz/token.txt`: the web player sends it as the
  `x-user-auth-token` request header on `POST play.qobuz.com/api.json/0.2/user/login`
  (Network tab); the same value is in Local Storage under `user_auth_token`.
- Yandex addon's canonical sign-in is the OAuth device flow it runs itself: the
  page's **Code** row shows the eight characters, **Where** the address
  (`ya.ru/device`, scheme stripped), **Waiting** the countdown. `token.txt` is a
  legacy import only (no refresh token) — docs must not present it as a way to
  type a token in.
- Each released addon carries its own `<id>/README.txt` (English + Russian, link
  to `docs/SETUP.md#<n>-<slug>`): `make pack` copies `*.txt`/`*.png`/`*.ini` and
  `assets/` from the addon folder, so the file lands at the root of the release
  zip and on the card. It is not read by the loader; keep it short.

## Building without docker (this machine, 2026-11-06)

- The root `Makefile`'s default goal is `lvgl`, not `target`: a bare `make` only
  checks LVGL and exits. Use `make target ADDON_IDS=...` / `make host ...`.
- `docker/build.sh` passes the toolchain target relatively
  (`make -C <player> rockbox-toolchain/bin/...`) while the rule is written with
  the absolute `$(CROSS_CC)`; the first run therefore fails with "No rule to make
  target". Build it directly:
  `docker run --rm -v $PWD:$PWD -w $PWD --user $(id -u):$(id -g) -e HOME=/tmp tempotec-player-build make -C $PWD/sonix-player/sonix-player $PWD/sonix-player/sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc`
- LVGL 9.6.0 does not clone reliably here (git: "early EOF" on the host and in
  the container). The tarball does:
  `curl -L -o lvgl.tgz https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v9.6.0`
  then `tar -xzf lvgl.tgz -C sonix-player/sonix-player/lvgl --strip-components=1`.
  The player's Makefile reads the version from `lvgl/include/lvgl/lv_version.h`.
- `make host` in the player tree builds `sonix_player_host`; SDL2, FreeType,
  opusfile, wavpack and ALSA are installed on this machine. Audio output is not:
  the simulator logs `Cannot open PCM device 'hw:0,0'` and every track "ends"
  immediately, so playback cannot be heard here — decode startup is still
  visible in the log (`aac: libfdk-aac loaded`, `growfile: following ...`).

## Piped (the YouTube addon's backend), measured 2026-11-05/06

- Working API instances: `https://api.piped.private.coffee` and
  `https://pipedapi.ducks.party`. Every other host from the projects' public
  lists was dead, unresolvable, 403 or 502.
- **2026-11-07: both instances answer `/config` and `/search`, but
  `/streams/{id}` returns HTTP 500 with
  `SignInConfirmNotBotException … LOGIN_REQUIRED: "Sign in to confirm that you're
  not a bot"`** — YouTube blocks the instances' IPs, so nothing plays. A video
  the instance has already fetched (`dQw4w9WgXcQ`) still answers 200, so it is
  per-video upstream blocking, not a dead mirror. Nothing to fix on the addon
  side; retry when the instance owners rotate their extraction.
- Both serve `/search`, `/config`, `/suggestions`, and `/streams/{id}` with a
  media URL on their own `/videoplayback` proxy (Range works). `audioStreams` is
  empty; only **itag 18** (muxed MP4, moov before mdat) exists. `/playlists/{id}`
  and `/channel/{id}` return 200 with `nextpage: null` and empty
  `relatedStreams` — do not build a UI on them.
- Fresh videos answer 500 with `SignInConfirmNotBotException: YouTube probably
  temporarily blocked anonymous access from this IP`; already-cached ones (e.g.
  `dQw4w9WgXcQ`) still work. That is the instances' IPs, not the player's.
- Thumbnails through the instance's image proxy come back as **WebP**, which the
  player's stb_image path does not decode — the Piped addon (formerly `youtube/`) skips those covers.

## On the device (2026-11-06)

- Card: `/mnt/sd_0` (vfat), addons under `/mnt/sd_0/Addons/<id>/`; player is
  `/usr/bin/sonix_player`, started by `/usr/bin/sonix_launch`. Killing the player
  makes the launcher **reboot** the device (~40 s), which is the only way to
  force a fresh addon scan; changing the files of a loaded addon is noticed on
  the next tile tap ("addon: <id> changed on the card, reloading").
- Log: `/mnt/sd_0/.local/sonix_player.log` (enabled by `[system] log_to_sd = 1`
  in `/data/device_config.ini`).
- Screenshot without the player's own feature: `adb shell 'dd if=/dev/fb0
  of=/tmp/fb.raw bs=614400 count=1'` then pull; 320x480 **BGRA**, stride 1280.
  `adb exec-out cat /dev/fb0` returns nothing on this adbd.
- Touch injection: `/dev/input/event2` is the GT9XX panel (INPUT_PROP_DIRECT);
  write `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y`, `BTN_TOUCH` + SYN_REPORT with
  a tiny static MIPS binary (source kept at `/tmp/oc/touch.c`, pushed as
  `/tmp/touch`, subcommands `tap X Y` / `drag X1 Y1 X2 Y2`). Coordinates are panel
  coordinates, identical to the simulator's: on-screen keyboard rows at
  y 275/326/377/428, columns q..p 18,49,80,112,143,175,206,238,269,301, OK 288,450.
- The card is found at boot only; `killall sonix_player` is the reboot switch.

## Diagnosing a freeze on the V3 (2026-11-06)

- The player's `watchdog` thread (`src/main.c`) writes to `<card>/.local/sonix_player.log`
  a full thread dump (`tid`, `comm`, `state`, kernel `wchan`) after 5 s without a UI
  heartbeat, and `watchdog: UI recovered after ~N s` when it comes back. So a freeze
  usually needs no external profiler: pull the log. `log_to_sd = 1` in
  `/data/device_config.ini`; `/data/sonix-launch.log` says who restarted the player.
- `adb shell` is root on this device, `/proc/<pid>/task/*/stack` is readable, and
  `strace 4.8` plus `timeout` are in the image — a host-side probe can snapshot
  everything (this session's: `/tmp/opencode/hang-probe.sh`, watches the log's tail
  and dumps thread stacks + 5 s of strace on the watchdog line).
- Freezes blamed on a streaming addon correlate with `jzmmc_v1.2: timeout 3000ms
  op:25 w sz:524288 ... blks:19/1024` — multi-block card writes stalling for seconds
  while the UI waits for a cover and the decoder for its frames (UI stall first,
  sound after). Seen with yandex, subsonic and radio; it is the write path, not the
  addon's locks. `yandexcache.c` therefore paces its writes (PACE_PIECE_BYTES/PACE_PAUSE_US,
  64 KB then 20 ms) and logs `complete (N bytes, M ms of card pacing)`.
- A `MemFree` of ~1 MB is normal here (MemTotal 57816 kB) and is not by itself
  evidence of a leak; check `available` in the audio profile lines instead.
