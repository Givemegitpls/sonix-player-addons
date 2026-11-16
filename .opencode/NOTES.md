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

**Verified on the device, 2026-11-07 02:23–02:39:** fifteen minutes of 96/24
playback, six track changes and five complete downloads (73, 45, 72, 54 and
105 s, every one `1023 KB/s`, about 350 MB written) with
`dmesg | grep -c "timeout 3000ms"` staying at 5 and
`grep -c "write error"` staying at 94 — not one stall, every stats line
`0 write recoveries`. So the fix holds; the single timeout seen at 01:19:15 was
before this and has not come back.

**The other sweep.** Qobuz still ran the "clear the whole cache every ten
tracks" pass, and the log caught it in the act on 2026-11-07:
`02:46:27 qobuzcache: 10 tracks since the last sweep, cleaning up` followed by
`cleared (15 files)` — tens of megabytes deleted in one burst, from the thread
that also feeds playback, while the next track was downloading. That is exactly
what was removed from the Yandex cache. `QOBUZCACHE_CLEAR_EVERY`,
`TIDALCACHE_CLEAR_EVERY`, `ZVUKCACHE_CLEAR_EVERY` and `SUBSONICACHE_CLEAR_EVERY`
are now 0 too; the ceiling's prune, `clear()` and `clear_on_exit()` are
untouched. A `jzmmc` timeout at 02:44 happened with no download running at all
(previous one finished at 02:42:36, the next started at 02:46:25), which is the
same story from the other side: the card stalls on something other than the
addon's writes.

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
- Release zips are `qobuz subsonic yandex gearboy flappybird dino cassette`; `hello` is
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
- Each released addon carries its own `<id>/README.txt` (link to
  `docs/SETUP.md#<n>-<slug>`): `make pack` copies `*.txt`/`*.png`/`*.ini` and
  `assets/` from the addon folder, so the file lands at the root of the release
  zip and on the card. It is not read by the loader; keep it short.
  **Layout since 2026-11-16:** two separate documents, English first, then the
  same in Russian after one blank line. Never a sentence followed by its
  translation, never `INSTALL / УСТАНОВКА`; section headings stay in their
  block's language. The rule lives in `.opencode/skills/sonix-addon`,
  "README.txt: two blocks". gravity's README repeats the licence note in the
  Russian block as well.

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
  opusfile and ALSA are installed on this machine, **wavpack is not** (2026-11-10:
  `build_host/system/decode/opusdec.o` fails with "Package wavpack was not found"),
  so the simulator does not build here and the addons' own `make host` is the
  only host build to run. Audio output is not: the simulator logs
  `Cannot open PCM device 'hw:0,0'` and every track "ends"
  immediately, so playback cannot be heard here — decode startup is still
  visible in the log (`aac: libfdk-aac loaded`, `growfile: following ...`).
- LVGL cloned fine on 2026-11-10 (`Cloning LVGL v9.6.0` in `make host`); the
  tarball route above is only the fallback when git gives "early EOF".
- The cross toolchain is not in a fresh checkout and `sonix-player/` starts
  empty: `git submodule update --init` first, and `make target` / `check-abi`
  need the 20-60 minute toolchain build (CI has it cached).

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

## The simulator as an addon test bench (2026-11-08)

- Run: `SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=<card> SONIX_PANEL=320x480
  SONIX_CONFIG=<card>/device_config.ini ./sonix_player_host` from the player
  tree. Addons are read from `<card>/Addons/<id>/` and must be the **host**
  build (`make host`). Touches go in as `printf 'x y' > /tmp/sonix/tap` (or
  `drag x1 y1 x2 y2`), screenshots come back in `<card>/Screenshots/` after
  `touch /tmp/sonix/shot`.
- Put `[power]\nscreen_off_seconds = 0` in the config: the panel blanks after
  30 s and the first tap after that only wakes it, which looks like "input does
  not work".
- The date/time setup is shown on every run (the clock cannot be set without
  permission, so it never counts as set): drag up on the right edge, then
  `160 450` confirms.
- **Reloading an addon crashes the host build.** `addon: <id> changed on the
  card, reloading` is followed by `SIGSEGV` — reproduced with `hello`, so it is
  the player's dlclose path, not the addon. Copy a new `.so` and restart the
  player instead of re-tapping the tile.
- The card is reported read-only when `/mnt/sd_0` is not a real mount, and then
  the library scan finds nothing (`0 tracks indexed`) — the file browser still
  browses `<card>`, but a row tap does not start playback, so "the addon with a
  track playing" cannot be checked this way.
- `LV_USE_LINE` is 0 in the player's `lv_conf.h`: `lv_line_*` does not exist in
  the build (and is not exported). Animate with plain objects (positions/sizes)
  or a static transform; a transform layer per frame is too much for this CPU.
- `stop()` runs **before** the shell deletes the addon's screens, so a FreeType
  face created by the addon must be handed back with
  `lv_obj_set_style_text_font(obj, &font_ui_XX, 0)` before
  `lv_freetype_font_delete()`; otherwise LVGL touches a freed face while the
  page is torn down.
- The host build has no usable ALSA output here (`default` resolves to dmix and
  fails: `snd_pcm_dmix_open: unable to open slave`), so every track "ends" the
  moment it starts and `device_state` comes back STOPPED with no file. An addon
  that animates "only while playing" therefore looks frozen in the simulator;
  the cassette addon spins while `status != PAUSED` for exactly that reason.

## Cassette addon: three layers (2026-11-10)

- The page is the owner's flat render, drawn at **480x320** and turned 90°
  clockwise to fill 320x480, so a drawing point lands on the page as
  `x = 320 - y_drawing`, `y = x_drawing`. Nothing is redrawn in C any more.
- Bottom to top: **one flat rectangle of `pal.body` filling the panel** (no
  radius, no edge) — **`assets/hub.png`**, the reel with its tape and the black
  disc as one picture, twice — **`assets/overlay.png`** 320x480 ARGB, which is
  everything else including the black page, the shell's bevel and shadows, the
  strip the reels sit in, the window, the screws, the label and the reflections.
  The overlay is *above* the hubs: its alpha is where the colour and the reels
  show through, and it frames a hub of any size. The artist/album labels are the
  only thing above it, with no background of their own.
- Hub pivots are fixed at `(181,143)` and `(181,336)` and are placed as
  `centre - size/2` from the sprite's own `header.w/h`, so a hub drawn at
  another size keeps the pivot. `lv_image_set_rotation` pivots on the picture's
  centre by default in LVGL 9.6 (no `lv_image_set_pivot` needed).
- `tools/addon_assets.py` writes both files, already turned to the page. Input
  is one of `--static PNG` (one ready layer, 480x320 or 320x480), `--pieces DIR`
  (the four pre-rotated pieces the first version drew: sticker, reflection,
  window, plate) or `--blender-dir DIR --render PNG` (cut them out of the
  project). `--hub PNG` overrides the hub drawing. The old four sprites are not
  in the repository any more; take them from commit `341945a` if the overlay has
  to be rebuilt exactly as it was.
- Stb hands back R,G,B,A and LVGL wants **B,G,R,A** here; the loader in
  `cpage.c` swizzles and the pixels are freed in `stop()`. Every picture is
  decoded once at init with `stbi_load_from_memory` (exported) into
  `LV_COLOR_FORMAT_ARGB8888`; a file that is not on the card is simply skipped.
- The overlay costs **614 KB** of RAM while the page is open (five pieces used
  to be ~480 KB); the `.so` is unchanged and the pictures live on the card.
- **Light**: two optional panel-sized pictures, `assets/shadow.png` and
  `assets/light.png`, drawn above everything including the labels — the shadow
  with `LV_BLEND_MODE_MULTIPLY` (`lv_image_set_blend_mode`), the light with
  `LV_BLEND_MODE_ADDITIVE`; alpha is the strength in both. LVGL's multiply is
  `dest * src >> 8` and the result is mixed by the source alpha, so a white
  pixel is 255/256 of the colour rather than exactly it, and a transparent one
  changes nothing. Non-normal blend modes are only safe because these two are
  plain images — no radius, no transform, `LV_OPA_COVER` — so LVGL draws them
  straight into the area buffer; give them a radius or a rotation and they go
  through a layer, where the blend has nothing under it to read.
- The stack was checked offline, not on a device: composited from the old pieces
  it differs from the old page by nothing above 2/255 on any pixel, and the two
  light layers were previewed with LVGL's own arithmetic over that page.
  `make host ADDON_IDS=cassette` builds clean with `-Wextra -Wshadow`; the
  three layers and the light have not been on a device yet.
- Cost with everything on the card: overlay + shadow + light = three 320x480
  ARGB buffers, 1.8 MB of RAM while the page is open.

## Cassette palette follows the picture, not the track (2026-11-09)

- `player_cover_image()` returns `&current_cover.dsc` — an address that is the
  same for the whole run — and `reload_cover()` deliberately keeps the previous
  sleeve up until the new one is decoded (`cover: on screen 1767 ms after the
  track change`). So a palette rebuilt on the track change is the *previous*
  record's and nothing rebuilds it when the real sleeve lands: the cassette wore
  SOFT PLAY's green over Red Hot Chili Peppers. `cpage.c` now fingerprints the
  pixels in `poll_cb` (FNV over w/h/stride and every 64th byte, ~3k reads).
- Checking a palette offline: compile `cassette/cpal.c` with the tone functions
  cut out of the player's `cover.c` (`/tmp/opencode/toneimpl.c`) plus a stub
  `lv_color_make`, feed it a 320x320 RGB565 dump of a `player_covers` row. The
  device's shell colour is that output through RGB565 bit replication
  (`#11944c` → `(16,150,74)`), which is how a screenshot can be matched back to
  the record it was taken from.
- The player's accent is not the tone: `alt_mark()` keeps the hue, cuts the
  saturation to 72 % of the grey distance and pins luma at 196, so very different
  sleeves give the same pale tan play button.
- `make` on its own only ensures LVGL is cloned (the file's first target is
  `lvgl`): build with `make target` / `make host`, `ADDON_IDS=...` to narrow. The
  device has `nm` but no `readelf` and no `input`.

## The Yandex page leads with the code, and says where the keys come from (2026-11-11)

- The concept the owner wants: "Sign in with a code" is the whole sign-in and
  nothing is typed on the player. Since 2026-11-11 that holds literally —
  `yandex/settings.ini` **ships filled in** (`make pack`'s `*.ini` glob puts it
  on the card), so the four keys are not asked of anyone. `rebuild_home()`
  (`yandex/src/gui/streaming/yandexstate.c`) then has three notes: no keys (only
  when the file is missing — it names `Addons/yandex/settings.ini`), not signed
  in (tap the code row), and — only when it is really on the card — the old
  `token.txt`. The `Code`/`Where`/`Waiting` rows are state, and a note under them
  says the code is typed on a phone.
- **`client_header` is the whole header line**, not just the client string:
  `yandex.c` puts it into the request as `"%s\r\n"` (`key_header`), so the value
  must be `X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.95.0`. A bare
  `YandexMusicDesktopAppWindows/5.95.0` parses, shows "keys present", and fails
  at playback.
- The owner chose to publish the official client's credentials (2026-11-11),
  knowing the pair ages and that ToS/DMCA risk sits on the repository: a newer
  release zip overwrites a card's `settings.ini`, so README.txt and
  `docs/SETUP.md` say not to do it over an edit. `settings.ini.example` is gone.
- **Verified end to end on the device, 2026-11-11:** with a card holding only
  the shipped five files (no session, no `token.txt`) the page says "not signed
  in", "Sign in with a code" gets a code from the addon itself
  (`Code`/`Where`/`Waiting`), confirming it on a phone writes
  `yandex_session.ini` **with a `refresh_token`** and rebuilds the page to the
  account's name — so the shipped `client_id`/`client_secret` are accepted for
  both grants. A session imported from `token.txt` has none of that
  (`refresh_token` absent, verified on the same card): the code is the only way
  that renews.
- A code lives 300 s and the page keeps counting while the panel sleeps
  (`screen_off_seconds`), so a code read off a dark screen is already dead —
  wake the panel before asking for one.
- Every action that cannot work without keys says the same thing:
  `NO_KEYS_POPUP` in the same file, one macro rather than three copies.
- `docs/SETUP.md` section 3 links the upstream files instead of naming them:
  `MarshalX/yandex-music-api/blob/main/yandex_music/_client/device_auth.py`
  (`_DEFAULT_CLIENT_ID`, `_DEFAULT_CLIENT_SECRET`) and
  `.../yandex_music/utils/sign_request.py` (`DESKTOP_SIGN_KEY`,
  `DESKTOP_CLIENT`). Both answered 200 on 2026-11-11; `main` is the branch.

## Driving the player over adb after a reboot (2026-11-11)

- `/tmp` is cleared by the reboot the launcher does after `killall
  sonix_player`, so `/tmp/touch` and `/tmp/key` have to be pushed again; both
  are crossed from `/tmp/oc/touch.c` with the tree's
  `rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc -static`.
- **A tap needs an `ABS_MT_SLOT` event.** Without it the injected sequence is
  read back fine from `/dev/input/event2` (`cat` shows the events) and the player
  ignores it, which looks like a frozen UI: the helper in `/tmp/oc/touch.c` has
  no slot and did nothing on the 2026-11-07 build, while the same sequence with
  `ABS_MT_SLOT 0` before `ABS_MT_TRACKING_ID` works (`/tmp/opencode/touch2.c`).
- The panel has double-tap wake **off** and `screen_off_seconds = 45`, so a
  single tap on a dark screen only arms nothing: wake it with the power key
  instead — `EV_KEY` 116 on `/dev/input/event1` (`/tmp/opencode/key.c`), and
  check `/sys/class/backlight/pwm-backlight.0/brightness` (0 = dark, 247 = on).
- `log_to_sd = 0` in `/data/device_config.ini` means the player's stdout and
  stderr are `/dev/null` (`/proc/<pid>/fd/1`), and `/data/sonix-launch.log` holds
  the launcher's lines only. Set it to 1 for a device run; the log is
  `/mnt/sd_0/.local/sonix_player.log` and it **appends** across restarts.
- Screenshot: `dd if=/dev/fb0` is 320x480 BGRA with stride 1280; the panel is
  readable while the backlight is on only.

## One-line labels: dots, not a character count (2026-11-11)

- `common/oneline.{h,c}` — `oneline_label(lv_obj_t *label)`: `LV_LABEL_LONG_MODE_DOTS`
  plus `lv_obj_set_height(label, lv_font_get_line_height(font))`. LVGL writes the
  `...` itself and cuts with the real metrics of the font that draws (the
  `LV_LABEL_LONG_DOT` spelling is a v9.1 compat alias; the enum in this LVGL is
  `LV_LABEL_LONG_MODE_DOTS`). Called before the label's font and width are set,
  the height is measured from the wrong face.
- **The height is the whole trick.** `lv_label.c` computes dots only when
  `size.y > lv_area_get_height(&txt_coords)` and cuts at the last visible line
  (`p.y` rounded down to a whole line). Height of exactly one line = one line of
  text ending in dots; `LV_SIZE_CONTENT` = it wraps and nothing is cut.
- Why no magic number: the faces are proportional. Measured with FreeType on
  `cassette/assets/Caveat.ttf` against the cassette's 300 px label — artist
  22 px: 38 lower-case letters, 26 capitals, 35 Cyrillic lower-case, 31 digits;
  "The Weeknd, Playboi Carti & Madonna" is 35 chars / 297 px (fits by 3 px),
  "WWWWWWWWWW" is 10 chars / 160 px. Record line 16 px: 51 / 37 / 48 / 44.
- The player's `src/gui/fonts/fonts.c` recognises a label pinned to one line by
  its height being a whole number of its font's lines, so a UI size change keeps
  it one line — that is a second reason to set the height and not just the mode.
- Side effect worth knowing: `oneline_label()` replaces `LV_LABEL_LONG_WRAP`, so
  the page must be able to afford a single line's height in its layout (cassette:
  300x54 block holds 22 px + 16 px lines).
- The library index is a 8.7 MB SQLite on the card, `/mnt/sd_0/.local/library.db`;
  copy it to the device's `/tmp` first (`adb shell 'cp ... /tmp/lib.db'`) and pull
  that, so a scan writing the real file cannot produce a torn read. Useful tables:
  `MEDIA_TABLE(path,name,album,artist,...)` 3331 tracks, `ALBUM_ARTIST_LINK(path,album_artist)`
  — the second is what `metadata_shown_artist()` answers with by default, and a file
  with no album-artist tag has no row there, so the track's own artist shows instead.
## Gravity Defied: порт игры на слой платформы (2026-11-14)
- Игра `rgimad/gravity_defied_cpp` (GPL-2.0) лежит в `gravity/` целиком: `game/` — дерево
  апстрима с сохранёнными путями, `platform/` — то, что раньше было SDL (lcdui). Пять
  удалённых файлов и все правки перечислены в `gravity/NOTICE.md`; апстрим взят на
  `91bd283` (2025-05-12).
- Апстрим ставит `main()`/SDL только в `src/main.cpp`, `src/lcdui/{Graphics,Image,Font,CanvasImpl}.cpp`,
  `src/utils/{Time,EmbedFileStream}.cpp`, `src/tests/` и `cmrc`. Больше ничто в игре SDL не
  трогает: 7700 строк, физика на фиксированной точке (`MathF16`), звука нет, ассеты 264 КБ.
- **Игра крутится на своём потоке**, а не на `lv_timer`: `Micro::run()` — это `while` с
  `Time::sleep(30)`, в пошаговую функцию его переписывать не надо. Остановка —
  `Micro::field_249 = false` и `pthread_join`; кадр уходит на UI-поток через `gui_post`.
- Кадр 480x320 RGB565 (пиксели панели в ландшафте) в 3 буферах: рисование, снимок,
  буфер LVGL. `lv_image_set_rotation(900)` + `lv_image_set_scale(256*screen_w/320)` показывает
  его на весь экран (на V3 — 1:1, в симуляторе 1.5x).
- **C++ + C**: функции платформы объявлены в `extern "C"` (`platform/gravity.h`,
  `assets.h`, `font_data.h`), иначе `dlopen` говорит `undefined symbol: gravity_quit`.
  `gui_post` из C++ объявляется руками: `src/gui/shell/gui.h` — C-заголовок без
  `extern "C"` и с ним внутри C++ не собирается.
- Шрифт запечён: `gravity/tools/bake_font.py` (Pillow) рендерит `FontSansSerif.ttf` в 12/16/32
  px plain+bold в `platform/font_data.c` (367 КБ исходника). Bold синтезируется смазом на
  1 px — в TTF одно начертание. Перегенерировать после смены шрифта.
- `make` без цели — это `lvgl`; собирать `make target ADDON_IDS=gravity` /
  `make host ADDON_IDS=gravity`. `check-exports` молча пропускается, если в дереве нет
  `sonix-player/sonix-player/sonix_player`.
- Симулятор: карта `/tmp/sd` (или любая), `Addons/gravity/{addon.json,addon.so,assets/}`,
  запуск `SDL_VIDEODRIVER=offscreen SONIX_SD_ROOT=/tmp/sd SONIX_LANG_DIR=… ./sonix_player_host`,
  тапы `printf 'x y' > /tmp/sonix/tap`, скриншоты в `<card>/Screenshots/` по
  `touch /tmp/sonix/shot`. Плитка More → «Gravity Defied» на `(100,400)`, затем `(85,400)`.
- `topbar_set_hidden(true)` из `init()` не держится: шелл показывает бар при переключении
  экрана, поэтому нужен колбэк на `LV_EVENT_SCREEN_LOADED`.
- Этап 1 (каркас + слой графики) пройден 2026-11-14: игра грузится, рисует меню поверх
  уровня, `saves/` создаётся на карте. Управления ещё нет — оно этап 2.
## Gravity Defied на устройстве: три вещи, которые не видны на хосте (2026-11-14)
- **Панель не присылает `ABS_MT_SLOT`.** Ядро отбрасывает событие, у которого значение
  не изменилось, а панель всегда слот 0 — значит `ABS_MT_SLOT` в потоке не появляется
  никогда. Код, который определяет протокол по этому событию (как `gbinput.c` в Gearboy),
  читает байты и молча разбирает ноль пакетов: тап на девайсе не доходит, на симуляторе
  всё работает, в логе пусто. Протокол B определяется по `ABS_MT_TRACKING_ID` или
  `ABS_MT_POSITION_*`, если в потоке ещё не было `SYN_MT_REPORT` (platform/touch.c).
- **C++-аддон линкуется с `-static-libstdc++ -static-libgcc`.** На девайсе
  `libstdc++.so.6.0.21` (GLIBCXX_3.4.21, GCC 5), тулчейн — GCC 9 (до 3.4.26), поэтому
  динамическая линковка даёт «the addon … did not start: libstdc++.so.6: cannot open
  shared object file» в попапе шелла. Геарбой ровно по этой причине несёт те же два флага.
- **Кадр 480x320 в ландшафте на девайсе:** 21 кадр/с, копия полного экрана 6–7 мс
  (`copy avg 6 … (100% of a screen)` в строке `frames:` плеера). Это наш кадр, не панель.
## Карта была повреждена (2026-11-14)
`dmesg` каждую секунду: `FAT-fs (mmcblk0p1): error, fat_get_cluster: invalid cluster chain`
писаться лог плеера (файл был битым инодом), а `log_to_sd=1` продолжал туда писать.
Лечение на месте: удалить `.local/sonix_player.log`, ошибки прекратились, `mount -o
remount,rw /mnt/sd_0` держится. Настоящее лечение — `fsck.vfat` на компьютере: на девайсе
есть только busybox без dosfsck. На карте остались `FSCK0000.REC…` от прошлых попыток.
Внимание: пока FAT битый, `killall sonix_player` перезагружает девайс, а `/tmp` на нём
чистится — статические хелперы (`/data/touch3`) надо держать в `/data`.
## Владение тач-панелью привязано к экрану, а не к init() (2026-11-14)
- Аддон, который забирает панель у LVGL (`panel_touch_enable(false)`) в `init()`, должен
  отдавать её при любом уходе с экрана, иначе шелл остаётся с мёртвым тачскрином до
  перезапуска плеера: на девайсе это выглядело как «тапы перестали работать» при живом
  экране. Правильное место — `LV_EVENT_SCREEN_LOADED` / `LV_EVENT_SCREEN_UNLOADED`:
  на loaded запускать игровой поток (`gravity_run`), на unloaded — `gravity_quit()`,
  который закрывает evdev и включает indev обратно.
- Подсказки зон (`platform/hints.c`) рисуются в готовый кадр в `CanvasImpl::repaint()`
  перед `surface_hand_over()`: игра каждый шаг заливает кадр белым, так что всё, что
  нарисовано раньше, исчезает. Каждая подсказка гаснет при первом нажатии своей зоны.
- Газ/тормоз на панели (320x480, портрет) — это правый верх / правый низ; в повёрнутом
  кадре 480x320 это x=270..479, y=0..159 и y=160..319. Подписи сдвинуты внутрь, к
  центральной полосе: у левого края их перекрывает текст меню игры.
## Тач на V3 однопальцевый, и это меняет управление (2026-11-14)
- Хотя в конфиге панели (`/proc/gt9xx_config`) записано `Touch Points = 5`, драйвер
  отдаёт **один контакт**: четыре дампа `/dev/input/event2` с двумя пальцами в разных
  углах — в каждом пакете максимум один контакт. Пакеты type A (`SYN_MT_REPORT`),
  `ABS_MT_SLOT` не заявлен. Байты конфига: `40 01` = 320, `E0 01` = 480, байт 5 = 5 точек.
- Поэтому ввод Gravity Defied переехал на `gpio-keys` (`/dev/input/event1`): там пять
  клавиш — `PLAYPAUSE`(164) газ, `PREVIOUSSONG`(165) тормоз, `VOLUMEUP`(115) наклон назад,
  `VOLUMEDOWN`(114) наклон вперёд, `NEXTSONG`(163) меню игры, плюс `POWER`(116).
- Узел **грабится** (`EVIOCGRAB 1`) на время игры: иначе газ меняет громкость и рисует
  попап. Кнопка питания при этом воспроизводится руками, как в `system.c`: зажатие
  ≥ 1200 мс → `powermenu_request_show()` (меню питания), короткое нажатие по отпусканию
  → `power_notify_power_button()` (переключает экран). Одного `power_notify_power_button()`
  по нажатию **мало**: это всегда «короткое», и меню питания становится недоступным.
  Так как плеер не видит ни клавиш, ни тача (indev паркуется), каждый переход состояния
  зовёт `power_notify_activity()` — иначе панель гаснет посреди заезда.
- Уход из аддона: LVGL-индев больше не паркуется, поэтому шелловская стрелка «назад»
  работает, а тап по экрану подменяет клавишу FIRE (ею меню игры выбирает пункт).
- Музыка глушится `audio_pause()` (экспортирован) при открытии экрана игры.
- Проверено на девайсе 2026-11-14: газ двигает байк (таймер 0:00 → 0:21), `NEXT`
  открывает меню, тап выбирает «Continue», стрелка возвращает в More, громкость при
  нажатиях в игре не меняется (в логе плеера ни одного `key code` после граба), после
  выхода граб снят (VOLUMEUP снова меняет громкость 40% → 41%).
- Для тестов на девайсе удобно временно ставить `[power] screen_off_seconds = 0`:
  c `standby_mem_seconds` при подключённом USB можно не возиться — зарядка запрещает
  mem-suspend. Возвращать обратно обязательно.
- `KEY_POWER` **переключает** экран, поэтому «разбудить» нажатием 116 можно случайно
  погасить рабочий экран — проверять `backlight` перед серией тапов.
## Меню игры затемняется, стрелка выхода перекрашивается (2026-11-14)
- Кадр игры белый, поэтому шелловская стрелка «назад» на нём была невидима: лечится
  `back_btn_tint_on_screen(gravity_screen, true, lv_color_black())` на
  `LV_EVENT_SCREEN_LOADED` (и `false` на UNLOADED). Это API шелла именно для страниц
  «со своими цветами», рисовать подложку под стрелку не нужно.
- Затемнение в меню делается в `surface_hand_over(dim)`: умножение каналов RGB565
  прямо в копировании кадра, которое и так идёт каждый кадр, поэтому лишнего прохода
  нет. Флаг — `Micro::isInGameMenu` (любое меню игры — модальный цикл). Замер на
  девайсе: фон 255 → 181 (`r>>1 + r>>2`, ~0.71 по каналу), текст остаётся чёрным.
- Стрелка выхода прячется вне меню: `back_btn_force_hidden(!menu)` из колбэка кадра
  (UI-поток), флаг — `gravity_menu_open()`, то есть `Micro::isInGameMenu` с прошлого
  кадра. Начальное состояние «неизвестно» (`-1`), иначе первый кадр ничего не выставит
  и стрелка останется висеть поверх заезда. На UNLOADED — `back_btn_force_hidden(false)`.
## Иконка Gravity Defied (2026-11-14)
- Глиф нарисован руками: `gravity/icon.svg` — мотоцикл из кругов и штрихов, без райдера
  (с фигуркой райдера на 74 px он превращается в кашу; проверил оба варианта рядом).
  Пересобрать: `python3 tools/make_icon.py --glyph gravity/icon.svg --hue 120 --out
  gravity/icon.png --preview /tmp/p.png`. Стиль тот же, что у остальных плиток: белый
  глиф, зелёное (hue 120) диагональное поле, повёрнутая карточка сзади.
- `icon.png` на карте, ключ `icon` в `addon.json`; на девайсе плитка читается рядом с
  Cassette и Calculator.
## Плеер перезаписывает `/data/device_config.ini` при выходе (2026-11-14)
- Правка конфига `sed`-ом **не держится**: живой плеер хранит значения в памяти и на
  завершении (в том числе по SIGTERM от `killall`) пишет их обратно. Я так дважды
  потерял возвращённые `screen_off_seconds`/`standby_mem_seconds`.
- Правильно: `kill -9 $(pidof sonix_player)` (без обработчика — не сохранит), затем
  `sed` по `/data/device_config.ini`, затем дать лаунчеру поднять плеер (~50 с) и
  проверить, что значения остались. Изменение через UI настроек тоже сохраняется.
- `log_to_sd = 0` в конфиге глушит `stdout` плеера в `/dev/null` (файл лога не пишется).
## Build quirks (2026-11-15)
- `make` with no goal builds **nothing**: the default goal is `.DEFAULT_GOAL := lvgl`
  (Makefile:121). `make` in the Makefile's own comment and in README/AGENTS says
  "all of them"; the working command is `make target` (`make host` for the simulator).
- Building a PR in a `git worktree` outside the repo needs
  `PLAYER_DIR=<main checkout>/sonix-player/sonix-player`, otherwise lvgl, the headers
  and the toolchain are not found (the submodule is not populated in the worktree).
## Testing an addon's state machine on the host (2026-11-15)
`calc/calc.c` can be run natively without the SDL simulator: include the `.c`
directly from a harness, provide stub headers for `lvgl/lvgl.h`,
`src/addon/addon.h`, `src/gui/{fonts/fonts.h,shell/*.h}`, `src/system/core/lang.h`,
and implementation stubs whose `lv_label_set_text()` keeps the text in the fake
`lv_obj_t`. The statics (`display_label`, `expr_label`, `acc`, `fresh`, `pending`)
are then readable after each scripted `on_key()` call. Reusable copy:
`/tmp/opencode/stub/` (harness.c + stubimpl.c). This is how the `%` and `±` bugs in
the merged calculator were demonstrated rather than argued from the code.
## Gravity Defied: жизненный цикл игры и хранилище записей (2026-11-15)
- Экран аддона создаётся один раз (`init()` зовётся на загрузку библиотеки, не на тап),
  поэтому «вход в игру» — это `LV_EVENT_SCREEN_LOADED`, а игра каждый раз строится
  заново. Отсюда две вещи: пауза музыки живёт в `screen_loaded_cb` (в `init()` она
  срабатывала только при первом заходе за сессию), а `gravity_run()` сбрасывает
  статики игры (`Micro::gameLoadingStateStage`, `Micro::isInGameMenu`) и состояние
  ввода. Без сброса `gameLoadingStateStage` второй заход не проходил ни одного шага
  загрузки и падал по null `gamePhysics` — на девайсе это выглядело как зависание
  (SIGSEGV ловит обработчик плеера и «остаётся живым», музыка при этом играет).
- Остановка игры — свой флаг в `platform/canvas.cpp` (`platform/stop.h`), а не
  `Micro::field_249`: `startApp()` ставит его обратно в true, а последовательность
  загрузки его вообще не смотрит, поэтому выход во время загрузки вешал UI-поток в
  `pthread_join`. `Time::sleep` (нарезан по 50 мс) и `Micro::goLoadingStep` бросают
  `gravity::stopped`; `Micro::run()` ловит его и всё равно зовёт `destroyApp`, иначе
  терялся сейв (меню игры крутят свои модальные циклы внутри `method_201`).
- Хранилище записей (`saves/GDTRStates`, `saves/<league><level>`) чинилось в трёх
  местах: `RecordEnumerationImpl::destroy()` больше не чистит список хранилища (он же
  и перечисление), `getLevelInfo`/`pushLongAs5Bytes` берут буфер по ссылке (в Java это
  были массивы — по значению писались 96 нулей), имена терминируются, а битый файл
  читается как пустое хранилище. Проверять хранилище можно без игры и симулятора:
  `g++ -std=c++17 -I gravity/game/src harness.cpp RecordManager.cpp rms/RecordStore.cpp
  rms/RecordEnumerationImpl.cpp` — готовый harness в `/tmp/opencode/rms/harness.cpp`.
- Симулятор: тап по экрану = только FIRE (`gtouch_fire` из `tap_cb`), поэтому в меню
  игры активируется лишь подсвеченный пункт, а во время заезда тап не значит ничего;
  ключи `gpio-keys` на хосте не эмулируются. Выход из заезда в симуляторе невозможен —
  жать `kill -9` и поднимать заново.
- Проверено на девайсе 2026-11-15 (владелец): выход во время загрузки, повторный
  заход, музыка на входе и сейвы ведут себя как ожидается. `tictactoe` залит на карту
  рядом (`/mnt/sd_0/Addons/tictactoe/`) и виден в More как «Tic Tac Toe».

## Заливка аддона на карту (2026-11-15)

- Карта на устройстве: `/dev/mmcblk0p1` на `/mnt/sd_0`, vfat, при старте `rw`
  (`adb shell mount | grep sd_0`). Один раз видел `ro` — это `errors=remount-ro`
  после ошибки fs, а не нормальное состояние.
- `mount -o remount,ro /mnt/sd_0` при работающем плеере падает с `Device or resource
  busy`: pid `sonix_player` держит открытыми на запись `.local/sonix_player.log`,
  `.local/library.db`, `.local/audiobooks.db`, `.local/radio.db` и играющий flac.
  Искать держателей: перебор `readlink /proc/*/fd/*`.
- Плеер нельзя просто убить: `/usr/bin/sonix_launch` запускает `sonix_player`, после
  его выхода спит секунду и перезагружает устройство. То есть «перезапустить плеер»
  = `adb shell 'sync; reboot'` (~40 с до `adb wait-for-device`). После ребута карта
  снова `rw`, и это нормально.
- Минимальная заливка одного аддона без `make pack` всех:
  `make ADDON_IDS=daily pack` -> `adb push build/Addons/daily/{addon.json,addon.so,icon.png,README.txt} /mnt/sd_0/Addons/daily/`.
  `settings.ini` аддона в папке не перезаписывать — там его собственное состояние
  (трек дня, сессии сервисов).

## addonstore: установка аддонов из GitHub releases (2026-11-16)
- Плитка в More, называется **Store** (папка/id осталась `addonstore`). `repos.txt` (owner/name на строку) → `GET /repos/<r>/releases?per_page=1`,
  каждый `.zip` ассет = аддон; id = имя ассета без `.zip`.
- Обновление определяется по `size`+`digest` ассета из API, записанным в
  `Addons/addonstore/installed.tsv` при установке, а не по `version` в манифесте
  (там у всех `0.1`). Аддон, поставленный вручную, записи не имеет и апдейт не предлагает.
- Загрузка: `http_stream_open` в `Addons/addonstore/cache/*.part`, запись через
  `common/cardpace` 1 МБ/с, проверка `sha256:` из `digest`, распаковка в
  `Addons/.addonstore-new-<id>` и `rename()` (старое — в `.addonstore-old-<id>`).
- **`aszip.c` не должен включать `tinfl_impl.inc`**: `common/gunzip.c` уже кладёт tinfl в
  каждый аддон, повторное включение даёт multiple definition на линковке. Достаточно
  `extern void *tinfl_decompress_mem_to_heap(...)`.
- Новые/удалённые плитки появляются только после перескана карты: `addon_scan`/
  `addon_card_changed` не экспортируются. Об этом сказано в UI и docs/SETUP.md.
- Хостовые проверки: `addonstore/tools/run_store_test.sh <zip> [releases.json]` — сеть
  застаблена (`tools/stub_net.c`), проверены извлечение `dino.zip` побайтно против `7z x`,
  вложенный префикс каталога, stored/deflate, отказ на `../`, реальный JSON релиза.
- Симулятор на этой машине не собирается (нет wavpack), поэтому UI-часть на девайсе
  ещё не проверялась.
- Обновление теперь **по месту** (2026-11-16): первая установка — как раньше через
  `Addons/.addonstore-new-<id>` + `rename()`, а поверх существующего аддона пишутся только
  файлы из архива, через `<name>.asnew` + `rename()`, `addon.so` последним. `addon.json`
  не перезаписывается, настройки/сессии/сейвы/кэш и файлы, которых нет в архиве, остаются.
  Проверено в `run_store_test.sh` вторым zip: кастомный манифест, settings.ini и файл в cache/
  выживают, `addon.so` и README заменяются.
- Самообновление разрешено: `rename()` подменяет `addon.so`, старый inode остаётся
  смапленным и работающим, плеер загружает новый код при следующем тапе (удаление себя
  по-прежнему запрещено). Тост после self-install: «откройте плитку заново».
- До подмены проверяется payload любого релиза: `addon.so` — ELF 32-bit LE MIPS (e_machine=8),
  `addon.json` — `api` == 1. Иначе установка отклоняется, установленный аддон не тронут.
- `addonstore` добавлен в CI whitelist (`.github/workflows/addons.yml`), значит тег публикует
  `addonstore.zip` наравне со всеми; впервые руками скопированный магазин показывает
  «Update», а если его `addon.so` уже совпадает с релизным — «актуально» (up to date).
- Категории в списке — по `section` из манифеста, в порядке Music/Streaming/Wireless/More,
  неизвестное — в Other; пустая категория не рисуется. У установленных секция читается с карты,
  у опубликованных — из zip релиза один раз на digest (скачивание не тратит лимит API,
  результат кэшируется в `releases.tsv`: 8 колонок, старый 6-колоночный кэш читается).
- «Актуально»: два пути. Поставленный магазином — digest/size ассета из записи
  `installed.tsv` против последнего релиза (покрывает весь zip, включая assets и
  settings.ini). Скопированный вручную — sha256 `addon.so` **из zip релиза**
  (`so_sha256`, читается в `as_read_manifest`, 10 колонок в releases.tsv, старые 6/8
  читаются) против файла на карте. Ограничение: assets-only релиз для ручной копии не
  виден — задокументировано.
- Правило обновления файлов (asinst.c `update_replaces`/`update_keeps`): заменяются только
  `addon.so`, `icon.png`, `README.txt`, `assets/**`; любой другой файл пишется лишь если его
  нет. Поэтому `addon.json`, `settings.ini` (yandex везёт свой с ключами), `token.txt`,
  сессии, `*.tsv`, `cache/`, `saves/` переживают апдейт; сохранения gravity —
  `Addons/gravity/saves/` (platform/canvas.cpp), в архив не входят; gearboy — `Games/Saves`
  вне аддона. Удаление по-прежнему сносит папку целиком.
