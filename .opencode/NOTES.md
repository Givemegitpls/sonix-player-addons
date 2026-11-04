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

## Local build state

`sonix-player/sonix-player/rockbox-toolchain` builds in 20-60 minutes; CI caches
it keyed on `sonix-player/sonix-player/docker/Dockerfile` + `rockboxdev/**`.
