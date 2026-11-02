#!/usr/bin/env python3
"""A stand-in for the Yandex Music API, for the simulator.

The addon's addresses are settings (yandexconf.h: "base_url", "auth_url"), which
is what makes it testable without an account:

    <card>/Addons/yandex/settings.ini   base_url = http://127.0.0.1:8765
    <card>/Addons/yandex/token.txt      any 16+ character token

then "Sign in from the card" on the page (the token is checked against
/account/status, which this answers). The catalogue is synthetic -- three
artists, their albums, sixty liked tracks, two playlists, a chart and new
releases -- and shaped like the real answers where yandex.c reads them.

It is a host-side probe, not part of the addon: addons/Makefile packs only
addon.json, addon.so, images, text, ini and assets/, so this folder stays in the
tree.

    python3 addons/yandex/tools/yandex_mock.py [port]
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

UID = 1

ARTISTS = [
    {"id": 1001, "name": "Mock Artist One"},
    {"id": 1002, "name": "Mock Artist Two"},
    {"id": 1003, "name": "Mock Artist Three"},
]

ALBUMS = {}
TRACKS = {}
LIKED = []


def add_album(artist, number, year):
    album_id = artist["id"] * 10 + number
    album = {
        "id": album_id,
        "title": f"{artist['name']} — Record {number}",
        "year": year,
        "trackCount": 3,
        "artists": [{"id": artist["id"], "name": artist["name"]}],
        "type": "album",
        "volumes": [],
    }
    volume = []
    for position in range(1, 4):
        track_id = album_id * 100 + position
        track = {
            "id": track_id,
            "title": f"Track {position} of record {number}",
            "durationMs": (2 * 60 + position * 7) * 1000,
            "available": True,
            "type": "music",
            "artists": [{"id": artist["id"], "name": artist["name"]}],
            "albums": [
                {
                    "id": album_id,
                    "title": album["title"],
                    "trackPosition": {"index": position, "volume": 1},
                }
            ],
        }
        TRACKS[track_id] = track
        volume.append(track)
        LIKED.append({"id": track_id, "albumId": album_id})
    album["volumes"] = [volume]
    ALBUMS[album_id] = album
    return album


for index, artist in enumerate(ARTISTS, start=1):
    add_album(artist, 1, 2019 + index)
    add_album(artist, 2, 2022 + index)

# More liked tracks than one page (yandexlist.h fetches fifty at a time), so the
# list pages: the sixtieth is on the second page.
for n in range(4, 24):
    for artist in ARTISTS:
        LIKED.append({"id": 990000 + n * 10 + artist["id"] % 10, "albumId": 0})

PLAYLISTS = [
    {
        "kind": 1001,
        "owner": {"uid": UID},
        "title": "Mock playlist one",
        "trackCount": len(LIKED),
        "tracks": [dict(ref) for ref in LIKED],
    },
    {
        "kind": 1002,
        "owner": {"uid": UID},
        "title": "Mock playlist two",
        "trackCount": len(LIKED[:3]),
        "tracks": [dict(ref) for ref in LIKED[:3]],
    },
]

CHART = [dict(ref) for ref in LIKED[:5]]


def track_details(refs):
    """POST /tracks: ids, each "id" or "id:album"; unknown ids are left out."""
    out = []
    for ref in refs:
        try:
            track_id = int(ref.split(":")[0])
        except ValueError:
            continue
        if track_id in TRACKS:
            out.append(TRACKS[track_id])
        else:
            # A liked id the catalogue does not carry: the mock still answers,
            # because the page's job is to show what came.
            out.append(
                {
                    "id": track_id,
                    "title": f"Mock track {track_id}",
                    "durationMs": 180000,
                    "available": True,
                    "type": "music",
                    "artists": [{"id": 1001, "name": "Mock Artist One"}],
                    "albums": [],
                }
            )
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: %s %s\n" % (self.command, self.path))

    def answer(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def not_found(self):
        self.answer({"error": "the mock does not know this route"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode()
        path = urlparse(self.path).path
        form = parse_qs(body)
        if path == "/tracks":
            refs = []
            for value in form.get("track-ids", []):
                refs.extend(value.split(","))
            self.answer({"result": track_details(refs)})
            return
        if path == "/albums":
            ids = []
            for value in form.get("album-ids", []):
                ids.extend(value.split(","))
            self.answer(
                {"result": [ALBUMS[int(i)] for i in ids if i.strip().isdigit() and int(i) in ALBUMS]}
            )
            return
        if path == "/users/%d/likes/tracks/add-multiple" % UID or path.startswith(
            "/users/%d/likes/" % UID
        ):
            self.answer({"result": {"revision": 1}})
            return
        self.not_found()

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path == "/account/status":
            self.answer({"result": {"account": {"uid": UID, "displayName": "Mock Account"}}})
            return

        if path == "/users/%d/likes/tracks" % UID:
            self.answer({"result": {"library": {"tracks": LIKED}}})
            return
        if path == "/users/%d/likes/artists" % UID:
            self.answer({"result": ARTISTS})
            return
        if path == "/users/%d/likes/albums" % UID:
            self.answer({"result": [{"album": album} for album in ALBUMS.values()]})
            return
        if path == "/users/%d/playlists/list" % UID:
            self.answer({"result": PLAYLISTS})
            return
        if path.startswith("/users/%d/playlists/" % UID):
            kind = path.rstrip("/").rsplit("/", 1)[-1]
            for playlist in PLAYLISTS:
                if str(playlist["kind"]) == kind:
                    self.answer({"result": {"tracks": playlist["tracks"]}})
                    return
            self.not_found()
            return
        if path == "/feed":
            self.answer(
                {
                    "result": {
                        "generatedPlaylists": [
                            {"ready": True, "data": PLAYLISTS[0]},
                            {"ready": False, "data": PLAYLISTS[1]},
                        ]
                    }
                }
            )
            return

        if path.startswith("/artists/"):
            try:
                artist_id = int(path.split("/")[2])
            except ValueError:
                self.not_found()
                return
            artist = next((a for a in ARTISTS if a["id"] == artist_id), None)
            if artist is None:
                self.not_found()
                return
            if path.endswith("/tracks"):
                tracks = [
                    t
                    for t in TRACKS.values()
                    if t["artists"] and t["artists"][0]["id"] == artist_id
                ]
                self.answer({"result": {"tracks": tracks, "pager": {"total": len(tracks)}}})
                return
            if path.endswith("/direct-albums"):
                albums = [a for a in ALBUMS.values() if a["artists"][0]["id"] == artist_id]
                self.answer({"result": {"albums": albums}})
                return
            if path.endswith("/track-ids"):
                ids = [
                    "%d:%d" % (t["id"], t["albums"][0]["id"])
                    for t in TRACKS.values()
                    if t["artists"] and t["artists"][0]["id"] == artist_id
                ]
                self.answer({"result": {"trackIds": ids}})
                return
            self.not_found()
            return

        if path.startswith("/albums/"):
            parts = path.split("/")
            try:
                album_id = int(parts[2])
            except (IndexError, ValueError):
                self.not_found()
                return
            album = ALBUMS.get(album_id)
            if album is None:
                self.not_found()
                return
            self.answer({"result": album})
            return

        if path == "/landing3/chart":
            self.answer({"result": {"chart": {"tracks": CHART}}})
            return
        if path == "/landing3/new-releases":
            self.answer(
                {"result": {"newReleases": [{"id": album_id} for album_id in list(ALBUMS)[:3]]}}
            )
            return

        if path == "/search":
            text = (query.get("text") or [""])[0].lower()
            artists = [a for a in ARTISTS if text in a["name"].lower()]
            albums = [a for a in ALBUMS.values() if text in a["title"].lower()]
            tracks = [t for t in TRACKS.values() if text in t["title"].lower()][:40]
            self.answer(
                {
                    "result": {
                        "artists": {"results": artists},
                        "albums": {"results": albums},
                        "tracks": {"results": tracks},
                    }
                }
            )
            return

        self.not_found()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    sys.stderr.write(
        "mock: %d artists, %d albums, %d liked tracks on http://127.0.0.1:%d\n"
        % (len(ARTISTS), len(ALBUMS), len(LIKED), port)
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
