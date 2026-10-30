# The Spotify engine

The protocol half of the Spotify addon: a Rust `cdylib` that the addon opens
with `dlopen()` from the card. It is a library of its own rather than part of
`addon.so` because the two halves have nothing in common — the addon is C
against the player's headers and LVGL, the engine is Rust with a memory and
threading model of its own — and because the engine is the part that will be
replaced when Spotify changes something, on a card, without a new firmware
image.

```
<card>/Addons/spotify/
  addon.so                   the interface (addons/spotify/addon.c)
  assets/spotify_engine.so   this library
  session.ini                settings, through addons/common/store
  token.txt                  a token too long for session.ini
```

The contract is `addons/spotify/spotify_engine.h`, and it is the only file both
halves read:

| function | what it does |
|---|---|
| `spotify_engine_abi()` | the ABI number; the addon refuses anything else |
| `spotify_engine_init(config_json)` | starts the engine, idempotent |
| `spotify_engine_login_start(request_json)` | one login request, answered through `poll` |
| `spotify_engine_poll(buffer, capacity)` | one event, a line of JSON, or 0 |
| `spotify_engine_shutdown()` | stops the worker and joins it |

Events, one line each, printed by the addon as `spotify: <line>`:

```
{"v":1,"event":"init","engine":"spotify","version":"0.1.0","abi":1,"transport":"unavailable","state":"ready"}
{"v":1,"event":"log","level":"info","message":"..."}
{"v":1,"event":"login","state":"authenticating"}
{"v":1,"event":"login","state":"failed","error":"transport_unavailable","message":"..."}
{"v":1,"event":"login","state":"authenticated","offline":true,"account":"self-test"}
{"v":1,"event":"login","state":"signed_out"}
{"v":1,"event":"shutdown","state":"closed"}
```

An event the addon does not know is logged and ignored, which is what lets this
library grow without the addon being rebuilt.

## What is here, and what is not

Here: the C surface, the session state machine (`ready` → `authenticating` →
`authenticated` / `failed` / `signed_out` → `closed`), a bounded event queue
with the oldest event dropped when the addon falls behind, the configuration,
the token's validation and its "never in the log" rule, and a scripted
self-test that walks the protocol end to end with no account and no network.

**Not here: the transport.** The part that actually speaks Spotify is
[librespot](https://github.com/librespot-org/librespot), and this tree has no
vendored copy of it and no network at build time. So `login` with a token ends
in

```
{"event":"login","state":"failed","error":"transport_unavailable"}
```

rather than pretending to connect. `{"kind":"selftest"}` is the way to see the
protocol work without an account; every event it produces carries
`"offline":true` so a screenshot of it cannot be mistaken for a session.

## Building

```
make addons-host      # build/host/spotify_engine.so, the simulator's copy
make addons           # the card's copy, into spotify/assets/, if allowed
make -C addons spotify-engine-test
```

There are **no dependencies**. Not one crate: the JSON is 300 lines of this
tree's own (`src/json.rs`), the queue is a `VecDeque` behind a `Mutex`, and the
worker is `std::thread`. That is deliberate. An addon is a package that arrives
on a card, and a build of it must not need a crates.io index, a `vendor/`
directory, a lockfile from somebody's laptop, or a network. `cargo build
--offline` with an empty `[dependencies]` is a promise that holds on any
machine, including the build image.

### The device build

Rust's `mipsel-unknown-linux-gnu` standard library is built against **glibc
2.17**; this device has **2.16** (`sonix-player/src/system/core/glibc_compat.h`
explains what that costs elsewhere). A library linked from that std asks for
symbols the device does not have: `make addons-checks` would catch it, and on
the device it is a `dlopen()` that fails with a version in the message.

So `addons/spotify/addon.mk` does not build the device half by default. It
prints why, the addon is packed without an engine, and the screen says
**engine not built**. The two ways forward, both for the owner:

1. a custom target JSON whose std is built against 2.16 — `-Z build-std` and
   nightly, which is what the rekkod tree's `spotify-engine/targets/
   mipsel-unknown-linux-musl.json` did — or
2. an engine that is a **static musl binary** started as a child process and
   spoken to over a Unix socket, the way that tree shipped it. A cdylib cannot
   be static, so this is a change to the protocol: same functions, but a
   process on the other side of them.

`SPOTIFY_ENGINE_ALLOW_TARGET=1` forces the first attempt anyway, for whoever is
testing a target that does satisfy the version:

```
make addons SPOTIFY_ENGINE_ALLOW_TARGET=1
```

## Threads

Every C function is called from the addon's UI thread, and `poll` returns in
microseconds: it copies one string out of a queue. The engine's work happens on
a thread it creates and reports through that queue — it never calls back into
the addon, so no `lv_*` is ever reached from the wrong thread. `shutdown` is the
one call that can block, for as long as the worker's current step (120 ms at
most today).

## The token

The addon keeps it, on the card, in `session.ini` through `common/store` when
it fits the store's 256-byte value and in `token.txt` when it does not. The
engine holds it for the length of an attempt and writes it nowhere: the log
carries its length and never its text, and there is a test for that
(`a_token_attempt_ends_with_the_truth_about_the_transport`).
