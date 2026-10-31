//! The catalog: search, playlists, albums and one track, on librespot's
//! metadata.
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
//! `total` is what the engine knows of the collection and `more` says whether
//! there is another page; for a collection that is walked page by page (a
//! playlist, a search) `total` can be a lower bound when the walk stopped at
//! the end of a page -- `more` is the field to page on, `total` is the field to
//! show.
//!
//! Ids are the 22-character base62 form (`0JQ5DAqbMKF...`), never a URI: they
//! travel in a URL path (`/stream/<id>`) and in a JSON line, and the base62
//! alphabet is what makes both safe. `kind` is `track`, `album`, `artist` or
//! `playlist`; the fields a kind has no answer for are empty (an album has no
//! duration, an artist has no album).
//!
//! Playlists and searches are read through librespot's context endpoint, which
//! is what the Spotify client itself uses for "the list behind this URI": it
//! returns pages of `spotify:track:` URIs plus a link to the next page. The
//! ids are then turned into items one by one, so a page costs one metadata
//! request per row. That is deliberate: the alternative is a mirror of the
//! catalog in this process, and the card's 58 MB of RAM is not the place for
//! it.

use std::collections::VecDeque;

use librespot_core::spotify_id::SpotifyId;
use librespot_core::{Session, SpotifyUri};
use librespot_metadata::audio::{AudioFileFormat, AudioFiles};
use librespot_metadata::image::Images;
use librespot_metadata::{Album, Artist, Metadata, Playlist, Track};
use librespot_protocol::playlist4_external::SelectedListContent;
use protobuf::Message;
use serde::Serialize;
use serde_json::Value;

/// Rows per answer when the request does not say, and the most it may ask for.
/// Fifty is where one answer is still a few kilobytes of JSON and fifty
/// metadata requests, which is the most this CPU should do for one tap.
pub const PAGE_DEFAULT: usize = 20;
pub const PAGE_MAX: usize = 50;

/// The most URIs a walk over context pages collects for one request, and the
/// most pages it follows. Both bound the work one op can cause.
const URIS_MAX: usize = 500;
const PAGES_MAX: usize = 8;

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

/// What a page of a collection came to: the rows, how much the engine knows of
/// the collection, and whether another page exists.
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

/// The cover nearest 320 pixels wide, in the form the addon can fetch. Empty
/// when Spotify sent no image at all, which is a normal answer for a local
/// track and for some podcasts.
fn cover(images: &Images) -> String {
	images
		.0
		.iter()
		.filter(|image| image.width > 0)
		.min_by_key(|image| (image.width - 320).abs())
		.or_else(|| images.0.first())
		.and_then(|image| image.id.to_base16().ok())
		.map(|id| format!("https://i.scdn.co/image/{id}"))
		.unwrap_or_default()
}

fn artists(track: &Track) -> String {
	short(
		&track
			.artists
			.0
			.iter()
			.map(|artist| artist.name.as_str())
			.collect::<Vec<_>>()
			.join(", "),
	)
}

fn track_item(id: &str, info: &Track) -> Item {
	Item {
		id: id.to_string(),
		kind: "track",
		title: short(&info.name),
		artist: artists(info),
		album: short(&info.album.name),
		duration_ms: info.duration.max(0) as u32,
		cover_url: cover(&info.album.covers),
	}
}

pub async fn track(session: &Session, id: &str) -> Result<Page, &'static str> {
	if !valid_id(id) {
		return Err("invalid_id");
	}
	let uri = SpotifyUri::Track {
		id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
	};
	let info = Track::get(session, &uri).await.map_err(|_| "metadata_failed")?;
	Ok(Page {
		items: vec![track_item(id, &info)],
		total: 1,
		more: false,
	})
}

async fn album_item(session: &Session, id: &str) -> Result<Option<Item>, &'static str> {
	if !valid_id(id) {
		return Err("invalid_id");
	}
	let uri = SpotifyUri::Album {
		id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
	};
	let info = Album::get(session, &uri).await.map_err(|_| "metadata_failed")?;
	Ok(Some(Item {
		id: id.to_string(),
		kind: "album",
		title: short(&info.name),
		artist: short(
			&info
				.artists
				.0
				.iter()
				.map(|artist| artist.name.as_str())
				.collect::<Vec<_>>()
				.join(", "),
		),
		album: String::new(),
		duration_ms: 0,
		cover_url: cover(&info.covers),
	}))
}

async fn artist_item(session: &Session, id: &str) -> Result<Option<Item>, &'static str> {
	if !valid_id(id) {
		return Err("invalid_id");
	}
	let uri = SpotifyUri::Artist {
		id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
	};
	let info = Artist::get(session, &uri).await.map_err(|_| "metadata_failed")?;
	Ok(Some(Item {
		id: id.to_string(),
		kind: "artist",
		title: short(&info.name),
		artist: String::new(),
		album: String::new(),
		duration_ms: 0,
		cover_url: cover(&info.portraits),
	}))
}

/// One URI from a context page, as an item. `None` for the kinds this addon
/// does not draw -- episodes, shows, local files -- which is not an error: a
/// playlist with a podcast in it still has to open.
async fn item_for_uri(session: &Session, uri: &str) -> Result<Option<Item>, &'static str> {
	// A context track may carry a query (`spotify:track:<id>?from=...`); the id
	// is what comes before it.
	let uri = uri.split('?').next().unwrap_or(uri);
	if let Some(id) = uri.strip_prefix("spotify:track:") {
		if !valid_id(id) {
			return Ok(None);
		}
		let info = Track::get(
			session,
			&SpotifyUri::Track {
				id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
			},
		)
		.await
		.map_err(|_| "metadata_failed")?;
		return Ok(Some(track_item(id, &info)));
	}
	if let Some(id) = uri.strip_prefix("spotify:album:") {
		return album_item(session, id).await;
	}
	if let Some(id) = uri.strip_prefix("spotify:artist:") {
		return artist_item(session, id).await;
	}
	Ok(None)
}

struct Uris {
	ids: Vec<String>,
	/// The collection's own idea of its size, when it says: a playlist context
	/// carries `length` in its metadata. `None` means "what the walk found".
	length: Option<usize>,
	more: bool,
}

/// Walks a context URI (`spotify:playlist:...`, `spotify:search:...`) until it
/// has `want` URIs or runs out of pages, and says whether another page is
/// still out there.
async fn context_uris(session: &Session, uri: &str, want: usize) -> Result<Uris, &'static str> {
	let context = session
		.spclient()
		.get_context(uri)
		.await
		.map_err(|_| "catalog_failed")?;

	let length = context
		.metadata
		.get("length")
		.and_then(|value| value.parse::<usize>().ok());

	let mut ids: Vec<String> = Vec::new();
	let mut next = String::new();
	for page in context.pages {
		if let Some(url) = page.next_page_url {
			if !url.is_empty() {
				next = url;
			}
		}
		ids.extend(
			page.tracks
				.into_iter()
				.filter_map(|track| track.uri)
				.filter(|uri| uri.starts_with("spotify:")),
		);
	}

	let mut walked = 0;
	while ids.len() < want && !next.is_empty() {
		if walked >= PAGES_MAX || ids.len() >= URIS_MAX {
			break;
		}
		walked += 1;
		let response = session
			.spclient()
			.get_next_page(&next)
			.await
			.map_err(|_| "catalog_failed")?;
		let page: Value = serde_json::from_slice(response.as_ref()).map_err(|_| "catalog_failed")?;
		let follow = page
			.get("next_page_url")
			.or_else(|| page.get("nextPageUrl"))
			.and_then(Value::as_str)
			.unwrap_or_default()
			.to_string();
		if follow == next {
			// The same page handed back twice would be an endless walk.
			return Err("catalog_failed");
		}
		next = follow;
		if let Some(tracks) = page.get("tracks").and_then(Value::as_array) {
			ids.extend(
				tracks
					.iter()
					.filter_map(|track| track.get("uri").and_then(Value::as_str))
					.map(str::to_string),
			);
		}
	}

	let more = !next.is_empty();
	Ok(Uris {
		ids,
		length,
		more,
	})
}

/// The window `offset..offset+limit` of a URI list, resolved into items.
async fn window(
	session: &Session,
	ids: &[String],
	offset: usize,
	limit: usize,
) -> Result<Vec<Item>, &'static str> {
	let end = offset.saturating_add(limit).min(ids.len());
	let mut items = Vec::with_capacity(end.saturating_sub(offset));
	for uri in ids.iter().take(end).skip(offset) {
		if let Some(item) = item_for_uri(session, uri).await? {
			items.push(item);
		}
	}
	Ok(items)
}

pub async fn search(
	session: &Session,
	query: &str,
	offset: usize,
	limit: usize,
) -> Result<Page, &'static str> {
	let query = query.trim();
	if query.is_empty() || query.chars().count() > 256 {
		return Err("invalid query");
	}
	// A search URI is percent-encoded: `spotify:search:never+gonna`. Encoding
	// everything that is not a letter or a digit keeps a colon, a slash or a
	// percent sign in a user's query from reaching the endpoint as syntax.
	let encoded: String = query
		.bytes()
		.map(|byte| {
			if byte.is_ascii_alphanumeric() {
				(byte as char).to_string()
			} else {
				format!("%{byte:02X}")
			}
		})
		.collect();
	let uri = format!("spotify:search:{encoded}");

	let walked = context_uris(session, &uri, offset.saturating_add(limit)).await?;
	let items = window(session, &walked.ids, offset, limit).await?;
	let total = walked.length.unwrap_or(walked.ids.len()).max(walked.ids.len());
	Ok(Page {
		items,
		total,
		more: walked.more || offset.saturating_add(limit) < walked.ids.len(),
	})
}

pub async fn playlists(
	session: &Session,
	offset: usize,
	limit: usize,
) -> Result<Page, &'static str> {
	let data = session
		.spclient()
		.get_rootlist(offset, Some(limit))
		.await
		.map_err(|_| "catalog_failed")?;
	let mut list = SelectedListContent::parse_from_bytes(data.as_ref()).map_err(|_| "catalog_failed")?;
	let length = list.length();
	let Some(contents) = list.contents.take() else {
		return Ok(Page {
			items: Vec::new(),
			total: length.max(0) as usize,
			more: false,
		});
	};

	let count = contents.items.len();
	let mut items = Vec::with_capacity(count.min(limit));
	for (index, entry) in contents.items.into_iter().take(limit).enumerate() {
		let id = match SpotifyUri::from_uri(entry.uri.as_deref().unwrap_or_default()) {
			Ok(SpotifyUri::Playlist { id, .. }) => id.to_base62().ok(),
			_ => None,
		};
		let Some(id) = id else {
			continue;
		};
		// The rootlist carries the playlist's name in `meta_items`, in the same
		// order as the entries. When it does not, one metadata request fills it
		// in; an empty title would draw as a blank row.
		let mut title = contents
			.meta_items
			.get(index)
			.map(|meta| short(meta.attributes.get_or_default().name()))
			.unwrap_or_default();
		if title.is_empty() {
			if let Ok(playlist_id) = SpotifyId::from_base62(&id) {
				if let Ok(playlist) = Playlist::get(
					session,
					&SpotifyUri::Playlist {
						user: None,
						id: playlist_id,
					},
				)
				.await
				{
					title = short(playlist.name());
				}
			}
		}
		if title.is_empty() {
			title = "Spotify playlist".to_string();
		}
		items.push(Item {
			id,
			kind: "playlist",
			title,
			album: String::new(),
			artist: String::new(),
			duration_ms: 0,
			cover_url: String::new(),
		});
	}

	let total = (length.max(0) as usize).max(offset.saturating_add(count));
	Ok(Page {
		items,
		total,
		more: offset.saturating_add(count) < total || count >= limit,
	})
}

pub async fn playlist_tracks(
	session: &Session,
	id: &str,
	offset: usize,
	limit: usize,
) -> Result<Page, &'static str> {
	if !valid_id(id) {
		return Err("invalid_id");
	}
	let uri = format!("spotify:playlist:{id}");
	let walked = context_uris(session, &uri, offset.saturating_add(limit)).await?;
	let items = window(session, &walked.ids, offset, limit).await?;
	let total = walked.length.unwrap_or(walked.ids.len()).max(walked.ids.len());
	Ok(Page {
		items,
		total,
		more: walked.more || offset.saturating_add(limit) < walked.ids.len(),
	})
}

pub async fn album_tracks(
	session: &Session,
	id: &str,
	offset: usize,
	limit: usize,
) -> Result<Page, &'static str> {
	if !valid_id(id) {
		return Err("invalid_id");
	}
	let uri = SpotifyUri::Album {
		id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
	};
	let album = Album::get(session, &uri).await.map_err(|_| "metadata_failed")?;

	let ids: VecDeque<String> = album
		.tracks()
		.filter_map(|uri| match uri {
			SpotifyUri::Track { id } => id.to_base62().ok(),
			_ => None,
		})
		.collect();
	let total = ids.len();

	let end = offset.saturating_add(limit).min(total);
	let mut items = Vec::with_capacity(end.saturating_sub(offset));
	for id in ids.iter().take(end).skip(offset) {
		let info = Track::get(
			session,
			&SpotifyUri::Track {
				id: SpotifyId::from_base62(id).map_err(|_| "invalid_id")?,
			},
		)
		.await
		.map_err(|_| "metadata_failed")?;
		items.push(track_item(id, &info));
	}

	Ok(Page {
		items,
		total,
		more: end < total,
	})
}

/// The content type the player needs for a stream of this format, or `None`
/// for a format this player cannot decode.
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
	fn a_context_uri_with_a_query_keeps_its_id() {
		// What `item_for_uri` strips before it looks at the kind.
		assert_eq!(
			"spotify:track:0123456789abcdefghijkl?from=search"
				.split('?')
				.next(),
			Some("spotify:track:0123456789abcdefghijkl")
		);
	}

	#[test]
	fn a_search_uri_encodes_everything_but_letters_and_digits() {
		let encoded: String = "never gonna: 100%"
			.bytes()
			.map(|byte| {
				if byte.is_ascii_alphanumeric() {
					(byte as char).to_string()
				} else {
					format!("%{byte:02X}")
				}
			})
			.collect();
		assert_eq!(encoded, "never%20gonna%3A%20100%25");
	}
}
