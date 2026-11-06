# TODO — sonix-player-addons

## Restore the shipping cache policy before any build goes out (open, 2026-11-06)

The tree is in **device-test mode**: `yandex/src/system/streaming/yandexcache.h`
carries

```c
#define YANDEXCACHE_MAX_BYTES (150L * 1024 * 1024)   // test: was 1 GB
#define YANDEXCACHE_PRUNE_HEADROOM 0L                // test: was 96 MB
#define YANDEXCACHE_CLEAR_EVERY 0                    // stays 0 (no sweep, on purpose)
```

The 150 MB / 0 pair was set to exercise "delete the oldest track on every new
track" on the device; it makes the cache re-download far more than it needs to.
Restore `1 GB` and `96 MB`, keep `CLEAR_EVERY 0`, and the header comment above
the defines says the same. The 1 MB/s pacing in `pace_to_card()` stays in both
cases.

## The one ALSA `Broken pipe` that ended a log is unexplained (open, 2026-11-06)

On 2026-11-06 at 13:38:20 the log ends on

```
audio[1495887]: write error: Broken pipe (recovery 1)
```

and nothing follows it until the reboot — while a later run (18:13:31, same
message, screen going off) shows `1 write recoveries` on the next stats line and
plays on. The 13:38 run had no `jzmmc` lines around it (the jzmmc timeouts in the
log are all from 17:19), so it is not established that the card caused it.

What would settle it, next time it happens: `/proc/<pid>/task/*/stack` for the
interface and `playback` threads, `dmesg | tail -40`, and `MemFree/Writeback`
from `/proc/meminfo` — **before** the power button, because the log is on the
card. The recipe is in `docs/ADDONS.md`, "When the player freezes on the device".

## Pace the other download caches to the card (open, 2026-11-06)

Only `yandexcache.c` limits how fast it writes the card (`pace_to_card()`,
1 MB/s). The other four write at whatever rate the network gives, which on this
board is what puts the interface and playback into `D` state: 25–45 MB arriving
at 2–4 MB/s of dirty pages, a card that answers a write with `jzmmc … timeout
3000ms`, and every `fsync` in the player waiting behind it. The long version is
`docs/ADDONS.md`, "The card is the slowest thing in the player"; the evidence is
the 2026-11-06 entry in `.opencode/NOTES.md`.

The reference implementation, `yandex/src/system/streaming/yandexcache.c`:

- `CARD_WRITE_BYTES_PER_SEC` — 1 MB/s;
- `mono_ms()` and `pace_to_card(download_t *d, long written)` — the sleep is
  what is left of the time `written` bytes should have taken;
- called at the end of `note_written()`, so every write path (raw, unwrapped,
  detached save) is paced, not just one loop;
- the completion line says `took N s, M KB/s (the card is paced to N KB/s)`.

Each of the four caches has one write helper to hook and a `download_t` to hold
the clock (`started_ms` + `pace_at_ms`, or reuse the existing progress counter):

| addon | file | where |
|---|---|---|
| qobuz | `qobuz/src/system/streaming/qobuzcache.c` | `download_pump()`, write at :717 |
| tidal | `tidal/src/system/streaming/tidalcache.c` | `pump_file()` :809, and the DASH segment path |
| zvuk | `zvuk/src/system/streaming/zvukcache.c` | `copy_raw()` :409 |
| subsonic | `subsonic/src/system/streaming/subsoniccache.c` | `write_chunk()` :304 |

Cover writes (`*.jpg`, a few tens of KB) do not need it.

Better than copying the same twenty lines four times: put it in `common/` as a
small module (`cardpace.h/.c`: `cardpace_reset(&p)`, `cardpace_note(&p, bytes)`,
a rate constant), the way `common/store` and `common/listrow` are shared.
Everything in `common/` is compiled into every addon, and `--gc-sections` drops
the copies a given addon does not use, so the cost is one header include. Do
that first, move `yandexcache.c` onto it, then hook the other four.

The rate is a property of the board, not of a PC: measure it before choosing.
On the device, `dd` to `/mnt/sd_0` with `conv=fsync`, and
`/sys/kernel/debug/mmc0/ios` for what the driver negotiated. Measured
2026-11-06: 4 bits, sd high-speed, 48 MHz; 9 MB/s write, 18 MB/s read, no
timeouts on a 1 GB sustained write. 1 MB/s of download leaves room for the log,
the library journal and playback reads; 2 MB/s is probably still safe.

Check: the pacing line in each cache's completion log, `frames:` lines with
`0 slow` while a prune runs, and `dmesg` free of `jzmmc … timeout` over a
20-track run.

Yandex itself is done and measured: 35 tracks, 21 prunes (78 files) while
playing, zero `jzmmc` timeouts, no stall longer than the 10 s stats cadence —
the 2026-11-06 entry in `.opencode/NOTES.md` has the numbers.
