# Project notes — sonix-player-addons

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
