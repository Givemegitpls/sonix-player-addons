//! The catalog: search, playlists, albums and one track, over Spotify's **Web
//! API**.
//!
//! Why not librespot's metadata, which this addon used first: in librespot 0.8
//! every catalog read goes through `spclient`, and `spclient` asks
//! `login5().auth_token()` for a bearer token. `login5` only accepts a *stored
//! credential* (the blob an old username/password login produced); with the
//! access token a user copies out of the web player it answers
//! `Login request was denied: INVALID_CREDENTIALS`, for a free account and a
//! Premium one alike. Verified against the real service on 2026-11-03: the same
//! token is accepted by `api.spotify.com` (401 for a bogus one, 429 with a valid
//! one), so the catalog is built here instead. Playback still goes through
//! librespot: the access point accepts that token and the audio key is the
//! server's decision.
//!
//! Every op answers the same shape, because the addon draws every list with one
//! row layout:
//!
//! ```json
//! {"ok":true,"protocol":1,
//!  "items":[{"id":"...","kind":"track","title":"...","artist":"...",
//!            "album":"...","duration_ms":214000,"cover_url":"https://..."}],
//!  "total":40,"more":true}
//! ```
//!
//! `total` is what the service says the collection holds and `more` whether
//! another page exists. Ids are the 22-character base62 form Spotify uses in
//! these endpoints, never a URI: they travel in a URL path (`/stream/<id>`) and
//! in a JSON line, and the base62 alphabet is what makes both safe. Local files
//! and podcast episodes come back with no id and are dropped -- they cannot be
//! streamed from here.
//!
//! The token is the one in `credentials.ini` (or in the cache after a sign-in):
//! `catalog_op` in main.rs takes it from the session, so a sign-in and a resume
//! are the same thing to this file. It expires about an hour after the user
//! copies it, and then every op answers `token_expired` -- which the page says
//! in words, because pasting a new one is the fix.

use bytes::Bytes;
use http::{header, Request};
use librespot_core::http_client::HttpClient;
use librespot_core::error::ErrorKind;
use librespot_metadata::audio::{AudioFileFormat, AudioFiles};
use serde::Serialize;
use serde_json::Value;

/// Rows per answer when the request does not say, and the most it may ask for.
/// The Web API takes up to 50 for these endpoints, and fifty rows is still a few
/// kilobytes of JSON.
pub const PAGE_DEFAULT: usize = 20;
pub const PAGE_MAX: usize = 50;

/// The base of every request. Overridable so the simulator can answer from a
/// local mock -- the engine then has no idea it is not talking to Spotify.
const BASE_DEFAULT: &str = "https://api.spotify.com/v1";

#[derive(Serialize, Default)]
pub struct Item {
	pub id: String,
	pub kind: &'static str,
	pub title: String,
	pub artist: String,
	pub album: String,
	pub duration_ms: u32,
	pub cover_url: String,
}

/// What a page of a collection came to: the rows, how much the collection holds,
/// and whether another page exists.
pub struct Page {
	pub items: Vec<Item>,
	pub total: usize,
	pub more: bool,
}

/// A Spotify id, in the form that is safe in a URL path and in JSON. Anything
/// else -- a URI, a path, `../credentials` -- is not an id.
pub fn valid_id(id: &str) -> bool {
	id.len() == 22 && id.bytes().all(|b| b.is_ascii_alphanumeric())
}

/// Titles come from Spotify and go on a 320-pixel screen: control characters
/// out (a newline in a title would break the line protocol of the addon), and a
/// cap so one absurd name cannot push a page past the response budget.
fn short(text: &str) -> String {
	text.chars()
		.filter(|c| !c.is_control())
		.take(160)
		.collect()
}

/// The cover nearest 320 pixels wide, as a URL. Empty when Spotify sent no
/// image at all, which is a normal answer for a local track.
fn image_url(images: Option<&Value>) -> String {
	let Some(images) = images.and_then(Value::as_array) else {
		return String::new();
	};
	let nearest = images
		.iter()
		.filter_map(|image| {
			let width = image.get("width").and_then(Value::as_u64).unwrap_or(0);
			let url = image.get("url").and_then(Value::as_str)?;
			Some((width, url))
		})
		.min_by_key(|(width, _)| (*width as i64 - 320).abs());
	nearest
		.or_else(|| {
			images
				.first()
				.and_then(|image| image.get("url"))
				.and_then(Value::as_str)
				.map(|url| (0, url))
		})
		.map(|(_, url)| url.to_string())
		.unwrap_or_default()
}

/// "Aphex Twin, Brian Eno" -- what the row shows under a title.
fn artist_names(artists: Option<&Value>) -> String {
	let Some(list) = artists.and_then(Value::as_array) else {
		return String::new();
	};
	short(
		&list
			.iter()
			.filter_map(|artist| artist.get("name").and_then(Value::as_str))
			.collect::<Vec<_>>()
			.join(", "),
	)
}

fn track_item(track: &Value, album_hint: Option<&Value>) -> Option<Item> {
	let id = track.get("id").and_then(Value::as_str)?;
	if !valid_id(id) {
		// A local file or an episode: no base62 id, nothing to stream from here.
		return None;
	}
	let album = track.get("album").or(album_hint);
	Some(Item {
		id: id.to_string(),
		kind: "track",
		title: short(track.get("name").and_then(Value::as_str).unwrap_or("")),
		artist: artist_names(track.get("artists")),
		album: short(album.and_then(|album| album.get("name")).and_then(Value::as_str).unwrap_or("")),
		duration_ms: track
			.get("duration_ms")
			.and_then(Value::as_u64)
			.unwrap_or(0)
			.min(u32::MAX as u64) as u32,
		cover_url: image_url(album.and_then(|album| album.get("images"))),
	})
}

fn album_item(album: &Value) -> Option<Item> {
	let id = album.get("id").and_then(Value::as_str)?;
	if !valid_id(id) {
		return None;
	}
	Some(Item {
		id: id.to_string(),
		kind: "album",
		title: short(album.get("name").and_then(Value::as_str).unwrap_or("")),
		artist: artist_names(album.get("artists")),
		album: String::new(),
		duration_ms: 0,
		cover_url: image_url(album.get("images")),
	})
}

fn playlist_item(playlist: &Value) -> Option<Item> {
	let id = playlist.get("id").and_then(Value::as_str)?;
	if !valid_id(id) {
		return None;
	}
	Some(Item {
		id: id.to_string(),
		kind: "playlist",
		title: short(playlist.get("name").and_then(Value::as_str).unwrap_or("")),
		artist: short(
			playlist
				.get("owner")
				.and_then(|owner| owner.get("display_name"))
				.and_then(Value::as_str)
				.unwrap_or(""),
		),
		album: String::new(),
		duration_ms: 0,
		cover_url: image_url(playlist.get("images")),
	})
}

/// The rows of a `{"items":[...]}` list, `map` applied to each entry.
fn items_of(list: Option<&Value>, map: impl Fn(&Value) -> Option<Item>) -> Vec<Item> {
	list.and_then(|list| list.get("items"))
		.and_then(Value::as_array)
		.map(|rows| rows.iter().filter_map(map).collect())
		.unwrap_or_default()
}

fn total_of(list: Option<&Value>) -> usize {
	list.and_then(|list| list.get("total"))
		.and_then(Value::as_u64)
		.unwrap_or(0) as usize
}

/// Percent-encodes everything that is not a letter or a digit: a query goes into
/// a URL, and `&`, `#` or `+` in a title would otherwise reach Spotify as syntax.
fn encode_query(query: &str) -> String {
	query
		.bytes()
		.map(|byte| {
			if byte.is_ascii_alphanumeric() {
				(byte as char).to_string()
			} else {
				format!("%{byte:02X}")
			}
		})
		.collect()
}

/// The catalog, as one request at a time with one token. Cheap to build: the
/// HTTP client underneath is the one librespot already carries (rustls, proxy
/// aware, and it retries a 429 after the service's own `retry-after`).
pub struct Catalog {
	client: HttpClient,
	base: String,
	token: String,
}

impl Catalog {
	/// `proxy` is the card's own setting (`proxy` in credentials.ini): the Web
	/// API refuses a country it does not serve, and the way around that is the
	/// user's own egress, which this device cannot otherwise reach. An HTTP
	/// proxy is what librespot's client speaks -- a SOCKS one has to be bridged
	/// on the machine that hosts it.
	pub fn new(token: String, proxy: Option<&str>) -> Self {
		let base = std::env::var("SONIX_SPOTIFY_API").unwrap_or_else(|_| BASE_DEFAULT.to_string());
		let proxy_url = proxy
			.filter(|text| !text.is_empty())
			.and_then(|text| url::Url::parse(text).ok());
		if proxy_url.is_some() {
			crate::log("web api: going through the proxy from credentials.ini");
		}
		Self {
			client: HttpClient::new(proxy_url.as_ref()),
			base,
			token,
		}
	}

	async fn get(&self, path: &str) -> Result<Value, &'static str> {
		let uri = format!("{}{path}", self.base);
		let request = Request::builder()
			.method("GET")
			.uri(&uri)
			.header(header::ACCEPT, "application/json")
			.header(header::AUTHORIZATION, format!("Bearer {}", self.token))
			.body(Bytes::new());
		let request = match request {
			Ok(request) => request,
			Err(error) => {
				crate::log(format!("web api {path}: {error}"));
				return Err("invalid request");
			}
		};
		match self.client.request_body(request).await {
			Ok(body) => serde_json::from_slice(&body).map_err(|error| {
				crate::log(format!("web api {path}: not json: {error}"));
				"catalog_failed"
			}),
			Err(error) => {
				if error.kind == ErrorKind::Unauthenticated {
					// The token has expired (they last about an hour) or was
					// revoked. This is the one failure the user can fix by pasting
					// a fresh one, so it gets its own word instead of a shrug.
					crate::log(format!("web api {path}: token refused"));
					return Err("token_expired");
				}
				crate::log(format!("web api {path}: {error}"));
				Err("catalog_failed")
			}
		}
	}

	pub async fn search(&self, query: &str, offset: usize, limit: usize) -> Result<Page, &'static str> {
		let query = query.trim();
		if query.is_empty() || query.chars().count() > 256 {
			return Err("invalid query");
		}
		let doc = self
			.get(&format!(
				"/search?q={}&type=track,album&limit={limit}&offset={offset}",
				encode_query(query)
			))
			.await?;

		// Tracks first, albums after: the addon's row layout draws both, and a
		// search that answers with tracks only would hide half of what was found.
		let tracks = doc.get("tracks");
		let albums = doc.get("albums");
		let mut items = items_of(tracks, |track| track_item(track, None));
		items.extend(items_of(albums, album_item));
		let total = total_of(tracks) + total_of(albums);
		let more = offset + items.len() < total;
		Ok(Page { items, total, more })
	}

	pub async fn playlists(&self, offset: usize, limit: usize) -> Result<Page, &'static str> {
		let doc = self
			.get(&format!("/me/playlists?limit={limit}&offset={offset}"))
			.await?;
		let items = items_of(Some(&doc), playlist_item);
		let more = offset + items.len() < total_of(Some(&doc));
		Ok(Page {
			items,
			total: total_of(Some(&doc)),
			more,
		})
	}

	pub async fn playlist_tracks(
		&self,
		id: &str,
		offset: usize,
		limit: usize,
	) -> Result<Page, &'static str> {
		if !valid_id(id) {
			return Err("invalid request");
		}
		let doc = self
			.get(&format!("/playlists/{id}/tracks?limit={limit}&offset={offset}"))
			.await?;
		// A playlist row is `{"track":{...}}`, and a removed entry has no track.
		let items = items_of(Some(&doc), |row| track_item(row.get("track")?, None));
		let more = offset + items.len() < total_of(Some(&doc));
		Ok(Page {
			items,
			total: total_of(Some(&doc)),
			more,
		})
	}

	pub async fn album_tracks(
		&self,
		id: &str,
		offset: usize,
		limit: usize,
	) -> Result<Page, &'static str> {
		if !valid_id(id) {
			return Err("invalid request");
		}
		// The tracks of an album do not carry the album's own name or cover, and
		// a row that shows neither is a row of "unknown" lines: one request for
		// the album, then the page of tracks with that as their context.
		let album = self.get(&format!("/albums/{id}")).await?;
		let doc = self
			.get(&format!("/albums/{id}/tracks?limit={limit}&offset={offset}"))
			.await?;
		let items = items_of(Some(&doc), |track| track_item(track, Some(&album)));
		let more = offset + items.len() < total_of(Some(&doc));
		Ok(Page {
			items,
			total: total_of(Some(&doc)),
			more,
		})
	}

	pub async fn track(&self, id: &str) -> Result<Page, &'static str> {
		if !valid_id(id) {
			return Err("invalid request");
		}
		let doc = self.get(&format!("/tracks/{id}")).await?;
		let items: Vec<Item> = track_item(&doc, None).into_iter().collect();
		let total = items.len();
		Ok(Page {
			items,
			total,
			more: false,
		})
	}
}

/// What librespot calls a decoded format, in the words the player's HTTP side
/// needs (stream.rs answers `/stream/<id>` with this as its Content-Type).
pub fn stream_type(format: AudioFileFormat) -> Option<&'static str> {
	AudioFiles::mime_type(format)
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn ids_are_base62_and_nothing_else() {
		assert!(valid_id("0123456789abcdefghijkl"));
		assert!(!valid_id("../../credentials"));
		assert!(!valid_id("spotify:track:0123456789abcdefghijkl"));
		assert!(!valid_id("0123456789abcdefghijk"));
		assert!(!valid_id("0123456789abcdefghijkl/"));
	}

	#[test]
	fn titles_lose_control_characters_and_are_cut() {
		assert_eq!(short("title\n\0next"), "titlenext");
		assert_eq!(short(&"x".repeat(400)).chars().count(), 160);
	}

	#[test]
	fn a_query_is_percent_encoded_outside_letters_and_digits() {
		assert_eq!(encode_query("never gonna: 100%"), "never%20gonna%3A%20100%25");
	}

	#[test]
	fn a_track_without_a_base62_id_is_dropped() {
		let local = serde_json::json!({"id": null, "name": "a local file"});
		assert!(track_item(&local, None).is_none());
		let episode = serde_json::json!({"id": "0JQ5DAqbMKF", "name": "an episode"});
		assert!(track_item(&episode, None).is_none());
	}

	#[test]
	fn a_track_row_keeps_the_fields_the_page_draws() {
		let track = serde_json::json!({
			"id": "0123456789abcdefghijkl",
			"name": "Xtal",
			"duration_ms": 293000,
			"artists": [{"name": "Aphex Twin"}, {"name": "Brian Eno"}],
			"album": {
				"name": "Selected Ambient Works",
				"images": [
					{"width": 640, "url": "https://i.scdn.co/image/big"},
					{"width": 300, "url": "https://i.scdn.co/image/small"}
				]
			}
		});
		let item = track_item(&track, None).unwrap();
		assert_eq!(item.kind, "track");
		assert_eq!(item.title, "Xtal");
		assert_eq!(item.artist, "Aphex Twin, Brian Eno");
		assert_eq!(item.album, "Selected Ambient Works");
		assert_eq!(item.duration_ms, 293000);
		assert_eq!(item.cover_url, "https://i.scdn.co/image/small");
	}

	#[test]
	fn a_search_reads_the_web_api_shape_over_the_wire() {
		use std::io::{Read, Write};
		use std::net::TcpListener;

		// One request, one canned answer: this is the only way to exercise the
		// HTTP path (headers, URL, parsing) without an account and without the
		// real service's rate limit.
		let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
		let port = listener.local_addr().unwrap().port();
		let server = std::thread::spawn(move || {
			let (mut socket, _) = listener.accept().unwrap();
			let mut buffer = [0u8; 2048];
			let _ = socket.read(&mut buffer);
			assert!(
				String::from_utf8_lossy(&buffer).contains("authorization: Bearer test-token"),
				"the request must carry the session's token"
			);
			let body = concat!(
				r#"{"tracks":{"items":[{"id":"0123456789abcdefghijkl","name":"Xtal","duration_ms":293000,"#,
				r#""artists":[{"name":"Aphex Twin"}],"album":{"name":"SAW","images":[{"width":300,"url":"https://i.scdn.co/image/x"}]}}],"total":1},"#,
				r#""albums":{"items":[],"total":0}}"#
			);
			let response = format!(
				"HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\n\r\n{}",
				body.len(),
				body
			);
			let _ = socket.write_all(response.as_bytes());
		});

		std::env::set_var("SONIX_SPOTIFY_API", format!("http://127.0.0.1:{port}/v1"));
		let catalog = Catalog::new("test-token".to_string(), None);
		let page = tokio::runtime::Builder::new_current_thread()
			.enable_all()
			.build()
			.unwrap()
			.block_on(catalog.search("xtal", 0, 10))
			.unwrap();
		server.join().unwrap();

		assert_eq!(page.items.len(), 1);
		assert_eq!(page.items[0].title, "Xtal");
		assert_eq!(page.items[0].artist, "Aphex Twin");
		assert_eq!(page.total, 1);
		assert!(!page.more);
	}
}
