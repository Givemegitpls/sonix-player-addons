# `spotify-engine-mips` — the librespot half, as a process

The Spotify engine of this addon: librespot, tokio and serde, built for the
TempoTec V3 as a **static mipsel-musl binary** and spoken to over a unix socket.
It replaces the earlier "hello from mips" proof, which existed to answer one
question -- can this tree's docker image produce a Rust binary that starts on
the device -- and is gone now that the answer is yes.

What is proven here, and what is not:

| | |
|---|---|
| builds for `mipsel-unknown-linux-musl`, static, no dynamic section | yes |
| starts on the device (Ingenic X1000, MIPS32r2, kernel 3.10.14) | yes |
| answers `{"op":"probe"}` over a unix socket | yes |
| librespot 0.8.0 is linked in and runs (not just compiles) | yes, `--self-test` |
| ring's C half computes correctly on this CPU | yes, `--self-test` |
| audio, catalog, HTTP, Spotify authentication | **not yet** -- next step |

```
$ make                       # build the engine in the docker image
$ make client                # build the unix-socket probe client
$ make check                 # file/readelf/size: static, MIPS, no dynamic section
$ make probe                 # push both, start on the device, ask it `probe`
$ make clean
```

## The command

From this directory, in the image `sonix-player/docker/Dockerfile` builds
(`docker build -t tempotec-player-build sonix-player/docker`):

```sh
RUSTFLAGS="-C linker=mipsel-linux-musl-gcc \
           -C link-self-contained=no \
           -C target-feature=-soft-float,+crt-static" \
cargo +nightly-2026-10-01 build --release \
    --target mipsel-unknown-linux-musl \
    -Z build-std=std,panic_abort
```

`make` wraps exactly this, including the way `docker/build.sh` runs the build
(tree mounted at its own absolute path, the calling user, `HOME=/tmp`), so that
what is proven here is what a `make addons` rule would do.

Toolchain, as it comes out of the image:

```
rustc 1.101.0-nightly (21b707e3f 2026-09-30)
cargo 1.101.0-nightly (f3865b2a4 2026-09-29)
mipsel-linux-musl-gcc (GCC) 11.2.1 20211120     # musl.cc, /opt/mipsel-linux-musl-cross
```

## What is linked

Direct dependencies:

| crate | version | why |
|---|---|---|
| `librespot-core` | 0.8.0 | the session, and the version string `probe` reports |
| `librespot-audio` | 0.8.0 | the fetch/decrypt half; needed by the stream op |
| `librespot-metadata` | 0.8.0 | catalog; needed by the track/playlist ops |
| `librespot-protocol` | 0.8.0 | the protobuf messages those two speak |
| `tokio` | 1.54.1 | `rt`, `net`, `io-util`, `sync`, `time`, `signal` |
| `serde` / `serde_json` | 1.0.229 / 1.0.154 | the line protocol, `serde_json` with `preserve_order` |
| `ring` | 0.17.14 | direct only for `--self-test`; rustls reaches it anyway |

The interesting transitives: `rustls` 0.23.46 (and 0.22.4, through
`tokio-tungstenite`), `rustls-webpki` 0.103.16, `webpki-roots` 1.0.11,
`hyper` 1.12.0, `hyper-rustls` 0.27.10, `reqwest` 0.12.28, `protobuf` 3.7.2,
`uuid` 1.29.0, `flate2` 1.1.10.

Counts: **245 packages in the dependency graph for this target**, 318 in
`Cargo.lock` (the rest are Windows/wasm/Apple crates that are resolved but never
built here). `cargo fetch --target mipsel-unknown-linux-musl` pulls all of them
from `static.crates.io`; there is no vendor directory and none is needed. The
image's registry cache is in `/opt/cargo` and lives as long as the container, so
a fresh `docker run` downloads them again (about half a minute); what makes a
rebuild cheap is `target/`, not the cache.

Nothing failed to build for mips. Exactly **one** crate in the tree has C code:
`ring`, through the `cc` crate and `mipsel-linux-musl-gcc`. There is no
`aws-lc-rs` (it wants CMake and has no mips target), no `openssl-sys`, no
`libz-sys` (`flate2` picks `miniz_oxide`, pure Rust).

## What had to be switched off, and the one thing that had to be pinned

**`default-features = false, features = ["rustls-tls-webpki-roots"]`** on every
librespot crate. The default is `native-tls`, i.e. OpenSSL, which the musl.cc
toolchain does not carry and which would be a second C library next to musl.
`rustls-tls-webpki-roots` also keeps the trust anchors inside the binary: the
device has no `/etc/ssl`, so `rustls-tls-native-roots` would have nothing to
read. Dropping defaults also drops `with-dns-sd` (libmdns, mDNS discovery),
which this addon does not use -- Sonix finds the engine on the card.

`rustls` comes in through `hyper-rustls`, which librespot pins with the `ring`
feature, so `rustls` 0.23's default `aws-lc-rs` provider never enters the graph.
That is the whole reason ring is the only C dependency; if a future librespot
switches to `aws-lc-rs` or `rustls`' default provider, the MIPS build stops
there and the fix is a `ring`-provider feature, not a new toolchain.

**`vergen` is pinned to 9.0.6 in `Cargo.lock`, and that is part of the recipe.**
`librespot-core` 0.8.0's build script uses `vergen` + `vergen-gitcl` to stamp the
git sha into the binary. With the newest pair cargo resolves `vergen` 9.1.0
(which depends on `vergen-lib` 9.1.0) next to `vergen-gitcl` 1.0.8 (which still
depends on `vergen-lib` 0.1.6), and the build script does not compile:

```
error[E0277]: the trait bound `&vergen::feature::build::Build: CoerceUnsized<&dyn vergen_lib::entries::Add>` is not satisfied
  --> librespot-core-0.8.0/build.rs:16:27
note: there are multiple different versions of crate `vergen_lib` in the dependency graph
```

`cargo update -p vergen --precise 9.0.6` collapses it to one `vergen-lib`
(0.1.6) and the build script compiles. It is a host-side build-script problem,
not a mips one, but it stops the build all the same. **Do not run
`cargo update` without re-running `make`.**

## The artifact

```
target/mipsel-unknown-linux-musl/release/spotify-engine-mips:
  ELF 32-bit LSB executable, MIPS, MIPS32 rel2 version 1 (SYSV),
  statically linked, stripped                                  593 468 bytes
```

`readelf -d` prints *"There is no dynamic section in this file"* -- there is no
`libc.so.6` to ask for a symbol version, which is the point: the device's glibc
is 2.16 and the tree's glibc 2.17 problem does not exist for this binary.
`readelf -A`: `ISA: MIPS32r2`, `Tag_GNU_MIPS_ABI_FP: Hard float (double
precision)`, `CPR1 size: 32`.

The size is smaller than "librespot, tokio and rustls" sounds because `lto`,
`opt-level = "s"` and `panic = "abort"` plus the fact that only `probe` and
`--self-test` call anything: everything unreachable from those two is dropped.
The engine that actually streams will be several MB, and that is normal.

## On the device

```
$ make probe
target/mipsel-unknown-linux-musl/release/spotify-engine-mips: 1 file pushed...
target/mipsel-linux-musl/probe-client: 1 file pushed...
tools/probe-device.sh: 1 file pushed...
engine pid=2038
socket /tmp/spotify-engine.sock is up after about 1s
--- probe
{"ok":true,"protocol":1,"librespot":"0.8.0","arch":"mipsel"}
--- unknown op
{"ok":false,"protocol":1,"error":"unknown op"}
--- self-test (librespot runs, getrandom works)
{"protocol":1,"librespot":"0.8.0","arch":"mipsel","os_random_ready":true,"audio_fetch":{"minimum_download_size":65536,"prefetch_threshold_factor":4.0},"metadata_parser":"rejected a default message","ring_sha256_ok":true}
self-test exit=0
--- log
spotify-engine: starting: librespot 0.8.0, mipsel
spotify-engine: credentials /data/spotify/credentials (not read at this step)
spotify-engine: listening on /tmp/spotify-engine.sock (protocol 1)
engine stopped
```

`--self-test` is the part worth reading. It is not a version print:

- `os_random_ready` -- two `SessionConfig::default()` calls mint different
  UUIDs. The kernel here is 3.10.14, which has **no `getrandom` syscall** (3.17),
  so this is also a check that the fallback to `/dev/urandom` works in a static
  musl binary.
- `audio_fetch.prefetch_threshold_factor` -- an `f32` out of library code, the
  one float this step can show; `-soft-float` exists precisely so floats that
  cross into C are not silently wrong.
- `metadata_parser` -- `librespot_metadata` parsed a default protobuf message
  and rejected it, which is the correct answer for a message with no uri and
  proves the parser ran rather than being dead code.
- `ring_sha256_ok` -- SHA-256 of the empty string computed by ring's C half on
  the X1000 equals `e3b0c442...b855`. ring has no MIPS assembly, so this is the
  C fallback, i.e. exactly the code TLS will run.

## Protocol

One request line per connection, one response line, then the engine closes the
connection. `probe` and `--self-test` are the only ops in this step; the next
step adds `authenticate`, `track`, `playlists`, `playlist`, `album`, `liked`,
`search`, `stream`, `status`.

```text
-> {"op":"probe"}
<- {"ok":true,"protocol":1,"librespot":"0.8.0","arch":"mipsel"}

-> {"op":"nonsense"}
<- {"ok":false,"protocol":1,"error":"unknown op"}

-> not-json
<- {"ok":false,"protocol":1,"error":"invalid request"}
```

Arguments: `--socket <path>` (required), `--credentials <path>` (accepted and
remembered, **not read yet**: the file does not have to exist at this step, and
nothing in it is logged). `--version` and `--self-test` print one JSON line and
exit.

The socket is created with mode 0600, and a socket file left behind by a killed
engine is unlinked on start; a *live* socket makes the engine exit with
`AddrInUse` rather than stealing it. `SIGTERM`/`SIGINT` remove the socket and
exit 0. Logs go to stderr as `spotify-engine: ...`.

## Why each build flag

**`+nightly-2026-10-01`** -- `mipsel-unknown-linux-musl` is a Tier 3 target:
rustup has no prebuilt std for it, so the only route to a binary is `-Z
build-std`, nightly only, reading the sources from the `rust-src` component. The
date is pinned rather than `+nightly` on purpose: `+nightly` means "whatever
nightly is newest today", silently downloads a second toolchain (without
`rust-src`, so the build fails confusingly) and makes the artifact depend on the
day it was built.

**`-Z build-std=std,panic_abort`** -- `panic_abort` because nothing may unwind,
and it drops the unwinder from the link.

**`--target mipsel-unknown-linux-musl`, musl and not glibc** -- Rust's
`mipsel-unknown-linux-gnu` std is built against glibc 2.17 and the device is
2.16, so a gnu binary asks for symbol versions that are not there. Static musl
sidesteps the device's libc entirely. (This is a *separate process* answer; a
`cdylib` loaded into the player is a different story -- see `../engine/README.md`.)

**`-C linker=mipsel-linux-musl-gcc` and `-C link-self-contained=no`** -- rustc
assumes a distributed musl target whose crt objects and libunwind sit in a
`self-contained` directory next to the target's std. A Tier 3 target built here
has no such directory, so the link fails with *"cannot find crt1.o"*.
`link-self-contained=no` hands the start files back to the C compiler, which is
musl.cc's gcc from the image, whose `libc.a` and `crt1.o` are the target's. It
is also the `cc` that builds ring's C half.

**`-C target-feature=-soft-float`** -- not cosmetic. Rust's target spec says
`+soft-float`, while musl.cc's `crt1.o` and `libc.a` are hard-float o32 (FR=0).
A call across that boundary is compiled for two different calling conventions
and the link only prints a note. Measured on the device, calling musl's
`strtod("2.5")` from Rust: `0.00000000000000000` without the flag,
`2.50000000000000000` with it. A `f64` returned from C arrives in `$f0` in one
ABI and in `$v0/$v1` in the other.

## What is still fragile

- **The nightly pin.** `-soft-float` is still an unknown-but-passed-through
  feature: `warning: unknown and unstable feature specified for
  -Ctarget-feature: soft-float ... will become a hard error in a future release
  (issue #162235)`. On the pinned nightly it is a warning; on a later one it may
  be an error, and the fix then is a custom target JSON whose spec says
  `-soft-float` (the way the rekkod tree did it) or a nightly that made
  `soft-float` a proper feature. Moving the pin means re-running `make probe`.
- **`Cargo.lock` is part of the build**, because of the `vergen` pin above. A
  `cargo update`, a `cargo add`, or a `--locked`-less build on a machine with a
  different lock will break it in a way that reads like a compiler bug.
- **ring is only proven by SHA-256.** That exercises the C fallback and the
  hashing paths, not the TLS handshake: nothing here has opened a socket to
  Spotify. The first real handshake is the next step, and if it fails it fails
  at `ring`'s bignum/AES code, not at the transport.
- **`getrandom` on kernel 3.10.** `os_random_ready` is true, so the fallback
  works today. It is worth knowing that the syscall does not exist on this
  kernel at all: any dependency that insists on `getrandom(2)` without a
  `/dev/urandom` fallback will fail here, not on a desktop.
- **Size.** 593 KB is `probe` plus dead-code elimination, not the engine. Watch
  it when the audio path lands; the card has room, the firmware image gets none
  of it.
- **The device's shell is busybox 1.25 with no `nc`.** `probe-client` exists
  because `printf ... | nc -U` is not available there. `setsid` on that busybox
  eats the program's own `--options` (`setsid: unrecognized option '--socket'`),
  so the engine is started as a plain background job, and `pkill` does not
  exist -- only `pgrep`/`kill`. Careful with `pgrep -f`: matching on the
  binary's *path* also matches the `chmod ... /mnt/sd_0/tmp/spotify-engine-mips
  ...` shell that started the script, which then kills itself and exits with no
  output; `tools/probe-device.sh` matches the engine's `--socket` arguments and
  a pidfile instead.
- **`adb shell` here always allocates a pty** (`adb shell -T` answers "device
  only supports allocating a pty"), and a pty echoes a script fed to its stdin,
  which is why the device half is a pushed file and not a heredoc.

## Next step

`authenticate` (cache credentials from `--credentials`), then `track` /
`playlists` / `search` on `librespot-metadata`, then `stream` on
`librespot-audio` with the PCM going back over the same socket -- Sonix keeps
owning decoding, the queue, DSP and the DAC. `../engine/` and `../addon.c`
describe the C side that will spawn this binary and speak this protocol.
