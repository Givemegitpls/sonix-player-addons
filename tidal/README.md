# Tidal

`addons/tidal/` — Tidal (tidal.com) as a service addon: tile `order 1` on the
Streaming page, `"service": true` in the manifest, and the two entry points the
loader asks for (`sonix_addon_entry`, `sonix_addon_service_entry`).

The account is the user's: a device-code sign-in (the QR or the code on the
screen), then a session the addon keeps on the card. Nothing of the account is
in this repository.

## The application pair, and where it comes from

The addon authenticates as **Tidal's official client**, not as the user: the
OAuth2 `device_authorization` call needs an application `client_id`, and the
token exchange needs that pair. Those two values are the same ones every open
Tidal client carries, and they are published:

| what | where | note |
|---|---|---|
| `client_id`, `client_secret` | `EbbLabs/python-tidal` — [tidalapi](https://github.com/EbbLabs/python-tidal), `tidalapi/session.py`, `Session.__init__`, the "OAuth Client Authorization" block | the project `docs/ADDONS.md` means by "tidalapi"; licence LGPL-3.0 |
| the same two values | [`tidal_player_api::auth`](https://docs.rs/tidal-player-api/latest/tidal_player_api/auth/) — `CLIENT_ID`, `CLIENT_SECRET` | described there as "Public client ID/secret of the official Tidal app" |

`tidal.c` carries upstream's four constants **verbatim**, including the
base64-of-base64 wrapping: `TIDAL_CLIENT_ID_A/B` and
`TIDAL_CLIENT_SECRET_A/B` (`tidal.c:104-107`) are the same halves tidalapi
stores, joined and decoded twice by `unwrap()` (`tidal.c:110-134`). The
wrapping is upstream's, and it is the reason both projects do it: to keep the
pair out of `strings` of an unpacked image. It is not protection, and it is not
meant to be — the values are public and any scanner that decodes base64 finds
them.

Verifying the provenance, without copying the values out of the file: upstream
stores the same four halves, so the check compares the constants, not the
decoded text.

```sh
python3 - <<'PY'
import re, urllib.request
src = open("sonix-player/addons/tidal/src/system/streaming/tidal.c").read()
ours = re.findall(r'TIDAL_CLIENT_(?:ID|SECRET)_[AB]\s+"([^"]+)"', src)
up = urllib.request.urlopen(
    "https://raw.githubusercontent.com/EbbLabs/python-tidal/main/tidalapi/session.py"
).read().decode()
theirs = set(re.findall(r'b64decode\(b"([^"]+)"\)', up))
print("halves found here:", len(ours), "identical in tidalapi:", all(h in theirs for h in ours))
PY
```

It prints `halves found here: 4 identical in tidalapi: True`. Moving the pin to
another tidalapi revision is the same check, and upstream splitting the id at 16
characters and the secret at 48 is why the halves are comparable at all. **One trap:** `nathom/streamrip` publishes a *near*
copy of the same pair — `streamrip/client/tidal.py`, also base64 — that differs
from this one by two characters (`1Nm5…` against `1Nn9…` of the secret). It is a
different client registration; a pair copied from there answers
`invalid_client` on the device-flow exchange.

## Overriding it, and rotation

`<card>/Addons/tidal/settings.ini` may name its own pair (`client_id`,
`client_secret`); `tidalconf_keys()` reads it, and when both keys are there the
built-in pair is not even decoded (`tidal.c:330-333`, `tidalconf.h`). That is
the path for a card provisioned with a pair of its own, and the path that
survives this constant becoming stale.

With no pair at all — no file, no compiled fallback — the addon stops before any
request: `tidal_configured()` is false, `api_call()` and `tidal_login_begin()`
return `api_keys_unavailable` before a socket is opened (`tidal.c:553`, `:705`).

If Tidal withdraws the pair (the device-flow exchange starts answering
`invalid_client`, which is also what `note_api_error` shows), replace the four
constants. Recomputing the halves:

```sh
python3 -c 'import base64,sys; print(base64.b64encode(base64.b64encode(sys.argv[1].encode())).decode())' '<value>'
```

and split the result at any point into the `_A` and `_B` constants — `unwrap()`
joins them before decoding. Upstream splits the id at 16 characters and the
secret at 48, and keeping their split makes the next comparison with
`tidalapi/session.py` a `grep` rather than a diff.

## What it keeps on the card

| path | what is in it |
|---|---|
| `Addons/tidal/settings.ini` | `client_id`, `client_secret`, `base_url`, `auth_url`, `scope`, `quality`, `cover_size`, `migrated` (`tidalconf.c`, through `common/store`) |
| `Addons/tidal/session.ini` | `access_token`, `refresh_token`, `expires_at` — the addon's own writer, because a Tidal token is a JWT of eight to nine hundred characters and `store_t` holds 256 per value (`tidalconf.c`) |
| `Addons/tidal/cache/` | the files fetched for playback, named by the manifest's `cache_path` |

A card written before the move to addons is read once: `settings.ini` first,
then `<card>/sonix/streaming.json` / `streaming-session.json`, then the
`[tidal]` section of `streaming-keys.ini` (or the sealed `.bin`, through
`streamkeys_legacy_get()`). `migrated = 1` is written whatever was found, so a
sign-out is not undone by the old files still being there.

## Status

**Not built in CI and not verified on a device.** The addon is excluded from the
whitelist the pipeline builds (`addons/Makefile`, `ADDON_IDS`): it compiles, but
the sign-in and playback paths are exactly the ones nobody has been able to
check. Treat it as unproven work, not as a shipped service.

What is known to be unverified, concretely: the device-code flow end to end, the
token refresh against a live account, and `cover_size`/quality negotiation
against the CDN.

The rest — the interface, the queue, the cache, the service table — follows the
same shape as Zvuk's and Qobuz's; `docs/ADDONS.md` carries the general
description ("The services, at a glance", "Porting a service page").
