# TODO — sonix-player-addons

Nothing open. The card-pacing work and the stutter it was chasing are done:
every cache paces at 1024 KB/s through `common/cardpace.c` (the 32-bit overflow
that used to kill the pause at 2.1 MB is fixed), and fifteen minutes of 96/24
playback with five complete downloads on 2026-11-07 produced no `jzmmc` timeout
and no ALSA `Broken pipe` at all.

The measurements, the `dd` matrix and the remaining suspects (decode readahead
and the Wi-Fi driver's `dhd_tcpdata_info_get … No more free tdata_psh_info`) are
in `.opencode/NOTES.md`, 2026-11-07.
