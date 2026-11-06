# TODO — sonix-player-addons

## The card stalls are on the read side, not the download's writes (open, 2026-11-07)

What is settled: every cache paces at 1024 KB/s through `common/cardpace.c`, and
a real qobuz download reports exactly that (`took 70 s, 1023 KB/s` on 72 MB).
The upstream `pace_to_card()` had a 32-bit overflow at 2.1 MB (`written *
1000L`), which meant the pause silently stopped after the first two megabytes of
every file; that is fixed and verified on the device.

The `dd` matrix in `.opencode/NOTES.md` (2026-11-07) then says the write path is
not the trigger: writes alone are clean at 4 MB/s, reads alone are clean, and
writes plus playback-rate reads are clean at 1, 2 and 4 MB/s. The only
reproducing configuration is a writer racing an **unthrottled reader that reads
in 512 KB requests**.

A six-minute run after the fix — two cached 96/24 tracks playing plus one full
72 MB download at 1023 KB/s — produced no `jzmmc … timeout 3000ms` and no
`audio: write error: Broken pipe` at all. One timeout in an earlier 86 s
download remains unexplained; it is the only evidence left that something still
stalls the card.

The ALSA `Broken pipe` is no longer a separate mystery: it lands in the same
second as the `jzmmc` timeout, so it is the playback thread starving behind the
card, not an independent bug.

Next, if a timeout comes back: play a *cached* track for ten minutes with no
download running and watch `dmesg | grep -c "timeout 3000ms"`. If it grows, the
trigger is on the read side — decode readahead, thumbnails, a library scan — and
no cache can pace it. Second suspect in the same run: the Wi-Fi driver's
`dhd_tcpdata_info_get … No more free tdata_psh_info` (hundreds of lines while
streaming), which has nothing to do with the card.
