# TODO — sonix-player-addons

## One `jzmmc` timeout in twenty minutes, with no download running (open, 2026-11-07)

Everything the addons can control is now fixed and measured:

- every cache paces at 1024 KB/s through `common/cardpace.c`; the 32-bit
  overflow that killed the pause after 2.1 MB (`written * 1000L`) is gone, and a
  real download reports exactly `1023 KB/s`;
- fifteen minutes of 96/24 playback with five complete downloads produced no
  timeout and no ALSA `Broken pipe`;
- the periodic "clear the whole cache every ten tracks" sweep is off in all five
  caches, after the log caught Qobuz doing `cleared (15 files)` in one burst
  while the next track was downloading.

What is left: at 02:44 a `jzmmc … timeout 3000ms` (write, 512 KB request) landed
with **no download in flight** — the previous one had finished at 02:42:36 and
the next started at 02:46:25. The `dd` matrix in `.opencode/NOTES.md` says the
same thing from the other side: writes alone are clean at 4 MB/s, and only a
writer racing an *unthrottled* 512 KB reader stalls the card.

So the remaining suspect is the player's own I/O, not an addon: decode readahead,
thumbnail or cover work, the library scan, the log, or the Wi-Fi driver's
`dhd_tcpdata_info_get … No more free tdata_psh_info` (hundreds of lines while
streaming, and it has nothing to do with the card). Next run: ten minutes with
the player idle and no music at all, then ten minutes of a cached track only,
counting `dmesg | grep -c "timeout 3000ms"` in each. That tells whether the
stall needs playback reads at all.
