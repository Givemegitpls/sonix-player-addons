//! The audio half: a track id in, decrypted bytes out.
//!
//! What Spotify serves is an AES-CTR encrypted file on a CDN, with the key
//! coming from the session's keymaster. librespot's own player reads it through
//! `AudioFile` -- a temporary file that grows as the file downloads -- and then
//! decrypts on the way into the decoder. This engine cannot do that: the
//! device has 58 MB of RAM in total and `/tmp` is a 28 MB tmpfs, so a track in
//! a temporary file is a track in RAM. Instead the encrypted bytes are fetched
//! in ranges and piped straight into `AudioDecrypt`, and the decrypted result
//! goes into the HTTP body. The whole path holds two 64 KB chunks at most, no
//! matter how long the track is.
//!
//! Two facts about the container, both from librespot 0.8's player:
//!
//!   * an Ogg Vorbis file has a 167-byte Spotify header in front of the first
//!     `OggS` page (normalisation data lives in it, at offset 144). The header
//!     is part of the encrypted file, so it is decrypted and then dropped:
//!     the player is handed a plain Ogg stream starting at `OggS`.
//!   * a FLAC file has no such header (`SPOTIFY_OGG_HEADER_END` is only used
//!     for Vorbis): the decrypted stream starts at `fLaC`, and ReplayGain, when
//!     there is any, is in the FLAC's own Vorbis comments.
//!
//! `total` in the CDN's `Content-Range` counts the encrypted file, header
//! included, so the number of plaintext bytes is `total - header`.
//!
//! Nothing here is exercised without an account: what `--self-test
//! --stream-from-file` proves is the HTTP side (chunked framing, the bytes
//! arriving intact, the slot being released), not the CDN and the key.

use std::collections::VecDeque;
use std::io::{self, Read};
use std::path::PathBuf;
use std::time::Duration;

use bytes::Bytes;
use http_body::Frame;
use hyper::body::Incoming;
use hyper::Response;
use librespot_audio::AudioDecrypt;
use librespot_core::audio_key::AudioKey;
use librespot_core::cdn_url::CdnUrl;
use librespot_core::spotify_id::SpotifyId;
use librespot_core::{Session, SpotifyUri};
use librespot_metadata::audio::{AudioFileFormat, AudioFiles, AudioItem};
use tokio::sync::{mpsc, OwnedSemaphorePermit};
use tokio::task::JoinHandle;

use crate::catalog;
use crate::http::ChannelBody;
use crate::{log, App};

/// Spotify's Ogg header: 167 bytes, then `OggS`.
const OGG_HEADER_BYTES: usize = 167;
const OGG_MAGIC: [u8; 4] = *b"OggS";
const FLAC_MAGIC: [u8; 4] = *b"fLaC";

/// One CDN request, and the largest one after the first. Small enough that a
/// slow link starts playing before the whole file is here, large enough that
/// the request overhead does not matter.
const FIRST_RANGE: usize = 64 * 1024;
const NEXT_RANGE: usize = 1024 * 1024;

/// What one read of the decrypting side hands to the HTTP body.
const OUT_CHUNK: usize = 32 * 1024;

/// How long one CDN response may take to produce its next frame, and how long
/// the first one may take to arrive at all. The player has its own timeout on
/// top; these only make sure a dead CDN does not hold a stream slot forever.
const RANGE_TIMEOUT: Duration = Duration::from_secs(15);

pub struct Stream {
	pub content_type: &'static str,
	pub body: ChannelBody,
}

/// A byte range of the encrypted file, as `Content-Range` describes it.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Span {
	start: usize,
	end: usize,
	total: usize,
}

/// `bytes <start>-<end>/<total>`, and nothing looser than that: the total has
/// to fit the arithmetic that follows, and an end before the start is a broken
/// answer, not a range.
fn span(value: &str) -> Option<Span> {
	let (range, total) = value.strip_prefix("bytes ")?.split_once('/')?;
	let (start, end) = range.split_once('-')?;
	let span = Span {
		start: start.parse().ok()?,
		end: end.parse().ok()?,
		total: total.parse().ok()?,
	};
	(span.start <= span.end && span.end < span.total && span.total <= i32::MAX as usize)
		.then_some(span)
}

pub async fn open(app: &App, id: &str, bitrate: u16) -> Result<Stream, &'static str> {
	let slot = app
		.streams
		.clone()
		.try_acquire_owned()
		.map_err(|_| "busy")?;

	// The self-test's local file stands in for a track: it goes through the
	// same chunked body, which is the part that has no other way of being
	// checked on this device.
	if id == "selftest" {
		if let Some(path) = app.self_test_file.clone() {
			return local(path, slot);
		}
	}

	let session = app.session.session().await?;
	cdn(&session, id, bitrate, slot).await
}

/// Streams a file from the card as if it were a track. Only reachable in
/// `--self-test`, where `--stream-from-file` names it.
fn local(path: PathBuf, slot: OwnedSemaphorePermit) -> Result<Stream, &'static str> {
	use std::io::{Seek, SeekFrom};

	let mut file = std::fs::File::open(&path).map_err(|_| "stream_failed")?;
	let mut magic = [0u8; 4];
	let read = file.read(&mut magic).map_err(|_| "stream_failed")?;
	let content_type = match &magic[..read] {
		b"OggS" => "audio/ogg",
		b"fLaC" => "audio/flac",
		b"ID3" => "audio/mpeg",
		_ => "application/octet-stream",
	};
	file.seek(SeekFrom::Start(0))
		.map_err(|_| "stream_failed")?;

	let (sender, receiver) = mpsc::channel(2);
	tokio::task::spawn_blocking(move || copy_file(file, sender));
	Ok(Stream {
		content_type,
		body: ChannelBody::new(receiver, slot),
	})
}

fn copy_file(mut file: std::fs::File, sender: mpsc::Sender<Result<Frame<Bytes>, io::Error>>) {
	let mut buffer = vec![0u8; OUT_CHUNK];
	loop {
		match file.read(&mut buffer) {
			Ok(0) => return,
			Ok(count) => {
				let chunk = Bytes::copy_from_slice(&buffer[..count]);
				if sender.blocking_send(Ok(Frame::data(chunk))).is_err() {
					return;
				}
			}
			Err(error) => {
				let _ = sender.blocking_send(Err(error));
				return;
			}
		}
	}
}

/// Resolves the track to a CDN file, fetches its first range, and starts the
/// two halves of the pipe: an async task that pulls ranges off the CDN, and a
/// blocking task that decrypts them into the HTTP body.
async fn cdn(
	session: &Session,
	id: &str,
	bitrate: u16,
	slot: OwnedSemaphorePermit,
) -> Result<Stream, &'static str> {
	let (key, url, format) = source(session, id, bitrate).await?;

	let (first, first_span) = range(session, &url, 0, FIRST_RANGE).await?;

	let header = if AudioFiles::is_ogg_vorbis(format) {
		OGG_HEADER_BYTES
	} else {
		0
	};
	let magic = if AudioFiles::is_ogg_vorbis(format) {
		OGG_MAGIC
	} else if AudioFiles::is_flac(format) {
		FLAC_MAGIC
	} else {
		return Err("unsupported_format");
	};
	let content_type = catalog::stream_type(format).ok_or("unsupported_format")?;
	if first_span.total <= header + magic.len() {
		return Err("stream_failed");
	}
	let total = first_span.total;
	let plain = total - header;

	let (encrypted_sender, encrypted_receiver) = mpsc::channel::<Bytes>(2);
	let (output_sender, output_receiver) = mpsc::channel::<Result<Frame<Bytes>, io::Error>>(2);

	let writer = tokio::task::spawn_blocking(move || {
		decrypt(key, encrypted_receiver, output_sender, header, magic)
	});
	tokio::spawn(pump(
		session.clone(),
		url,
		first,
		first_span,
		encrypted_sender,
		writer,
		plain,
	));

	Ok(Stream {
		content_type,
		body: ChannelBody::new(output_receiver, slot),
	})
}

/// The track's encrypted file on the CDN, with the key that opens it.
///
/// A track that is not available in this country answers with alternatives
/// (the same recording under another id, usually a re-release), so a few of
/// those are tried before giving up. FLAC first: this player decodes FLAC and
/// MP3 on the stream path, and FLAC is the lossless one.
async fn source(
	session: &Session,
	id: &str,
	bitrate: u16,
) -> Result<(AudioKey, String, AudioFileFormat), &'static str> {
	if !catalog::valid_id(id) {
		return Err("invalid_id");
	}
	let id = SpotifyId::from_base62(id).map_err(|_| "invalid_id")?;

	let wanted: &[AudioFileFormat] = match bitrate {
		96 => &[AudioFileFormat::OGG_VORBIS_96],
		160 => &[
			AudioFileFormat::FLAC_FLAC,
			AudioFileFormat::OGG_VORBIS_160,
			AudioFileFormat::OGG_VORBIS_96,
		],
		_ => &[
			AudioFileFormat::FLAC_FLAC,
			AudioFileFormat::OGG_VORBIS_320,
			AudioFileFormat::OGG_VORBIS_160,
			AudioFileFormat::OGG_VORBIS_96,
		],
	};

	let mut candidates = VecDeque::from([SpotifyUri::Track { id }]);
	for _ in 0..8 {
		let Some(uri) = candidates.pop_front() else {
			break;
		};
		let item = AudioItem::get_file(session, uri)
			.await
			.map_err(|_| "track_unavailable")?;
		if let Some(alternatives) = item.alternatives {
			candidates.extend(alternatives.0.into_iter().take(8));
		}
		if item.availability.is_err() {
			continue;
		}
		for format in wanted {
			let Some(file) = item.files.get(format).copied() else {
				continue;
			};
			let track = SpotifyId::try_from(&item.track_id).map_err(|_| "track_unavailable")?;
			let key = session
				.audio_key()
				.request(track, file)
				.await
				.map_err(|_| "stream_failed")?;
			let urls = CdnUrl::new(file)
				.resolve_audio(session)
				.await
				.map_err(|_| "stream_failed")?;
			let url = urls
				.try_get_urls()
				.map_err(|_| "stream_failed")?
				.into_iter()
				.find(|url| url.starts_with("https://"))
				.ok_or("stream_failed")?
				.to_string();
			return Ok((key, url, *format));
		}
	}
	Err("track_unavailable")
}

/// One range request against the CDN, checked against what was asked for: a
/// 206 whose `Content-Range` starts where it should and does not run past the
/// end of the request. Anything else is refused rather than trusted, because
/// an offset in the middle of an AES-CTR stream that is off by one byte is not
/// a slightly wrong track, it is noise.
async fn range(
	session: &Session,
	url: &str,
	at: usize,
	count: usize,
) -> Result<(Response<Incoming>, Span), &'static str> {
	use futures_util::StreamExt;

	if !url.starts_with("https://") || count == 0 {
		return Err("stream_failed");
	}
	let mut request = session
		.spclient()
		.stream_from_cdn(url, at, count)
		.map_err(|_| "stream_failed")?;
	let response = tokio::time::timeout(RANGE_TIMEOUT, request.next())
		.await
		.map_err(|_| "timeout")?
		.ok_or("stream_failed")?
		.map_err(|_| "stream_failed")?;
	if response.status().as_u16() != 206 {
		return Err("stream_failed");
	}
	let header = response
		.headers()
		.get("content-range")
		.and_then(|value| value.to_str().ok())
		.and_then(span)
		.ok_or("stream_failed")?;
	if header.start != at || header.end >= at.saturating_add(count) {
		return Err("stream_failed");
	}
	Ok((response, header))
}

/// Pulls the CDN's bytes into the decrypting side, one range at a time. It ends
/// when the file is done, when the CDN breaks, or when the decrypting side has
/// gone away (the player closed the connection).
async fn pump(
	session: Session,
	url: String,
	mut response: Response<Incoming>,
	mut current: Span,
	sender: mpsc::Sender<Bytes>,
	writer: JoinHandle<io::Result<usize>>,
	plain: usize,
) {
	use http_body_util::BodyExt;

	let total = current.total;
	let result = async {
		loop {
			let expected = current.end - current.start + 1;
			let mut received = 0usize;
			loop {
				let frame = tokio::time::timeout(RANGE_TIMEOUT, response.body_mut().frame())
					.await
					.map_err(|_| ())?;
				let Some(frame) = frame else {
					break;
				};
				let frame = frame.map_err(|_| ())?;
				let Ok(bytes) = frame.into_data() else {
					continue;
				};
				received = received.checked_add(bytes.len()).ok_or(())?;
				if received > expected {
					return Err(());
				}
				let mut bytes = bytes;
				while !bytes.is_empty() {
					let count = bytes.len().min(FIRST_RANGE);
					sender.send(bytes.split_to(count)).await.map_err(|_| ())?;
				}
			}
			if received != expected {
				return Err(());
			}
			if current.end + 1 == total {
				break;
			}
			let next = current.end + 1;
			let (next_response, next_span) = range(
				&session,
				&url,
				next,
				(total - next).min(NEXT_RANGE),
			)
			.await
			.map_err(|_| ())?;
			if next_span.total != total {
				return Err(());
			}
			response = next_response;
			current = next_span;
		}
		Ok::<(), ()>(())
	}
	.await;

	drop(sender);

	// A player that closed the connection shows up as a failed send above, and
	// that is not a fault: the writer's own send failed too and it stopped. Only
	// a transfer that ran to the end and still came up short is worth a line.
	match writer.await {
		Ok(Ok(sent)) if result.is_ok() && sent != plain => {
			log(format!("stream: sent {sent} of {plain} bytes"));
		}
		Ok(Err(error)) if result.is_ok() => log(format!("stream: {error}")),
		_ => {}
	}
}

/// Reads the encrypted bytes out of the channel, decrypts them, drops the
/// Spotify header if the format has one, checks the container's magic, and
/// hands the rest to the HTTP body. Returns how many plaintext bytes went out,
/// header and magic excluded.
fn decrypt(
	key: AudioKey,
	receiver: mpsc::Receiver<Bytes>,
	sender: mpsc::Sender<Result<Frame<Bytes>, io::Error>>,
	header: usize,
	magic: [u8; 4],
) -> io::Result<usize> {
	let mut input = AudioDecrypt::new(
		Some(key),
		Segment {
			receiver,
			pending: Bytes::new(),
		},
	);
	let mut buffer = vec![0u8; OUT_CHUNK];
	let mut skip = header;
	let mut head: Vec<u8> = Vec::with_capacity(4);
	let mut sent = 0usize;

	loop {
		let count = match input.read(&mut buffer) {
			Ok(0) => break,
			Ok(count) => count,
			Err(error) => return Err(error),
		};
		let mut slice = &buffer[..count];

		if skip > 0 {
			let drop = skip.min(slice.len());
			skip -= drop;
			slice = &slice[drop..];
		}
		if slice.is_empty() {
			continue;
		}

		if head.len() < magic.len() {
			let need = magic.len() - head.len();
			let take = need.min(slice.len());
			head.extend_from_slice(&slice[..take]);
			slice = &slice[take..];
			if head.len() == magic.len() && head[..] != magic[..] {
				return Err(io::ErrorKind::InvalidData.into());
			}
			if slice.is_empty() {
				continue;
			}
		}

		sent += slice.len();
		if sender
			.blocking_send(Ok(Frame::data(Bytes::copy_from_slice(slice))))
			.is_err()
		{
			// The player closed the connection; that is how a stream ends.
			return Ok(sent);
		}
	}

	if head.len() < magic.len() {
		return Err(io::ErrorKind::UnexpectedEof.into());
	}
	Ok(sent)
}

/// The encrypted side of the pipe as a `Read`: blocking, fed by the async task
/// that owns the CDN response. A closed channel is end of file, which is what
/// the decrypting side turns into a short transfer.
struct Segment {
	receiver: mpsc::Receiver<Bytes>,
	pending: Bytes,
}

impl Read for Segment {
	fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
		if out.is_empty() {
			return Ok(0);
		}
		while self.pending.is_empty() {
			match self.receiver.blocking_recv() {
				Some(bytes) => self.pending = bytes,
				None => return Ok(0),
			}
		}
		let count = out.len().min(self.pending.len());
		out[..count].copy_from_slice(&self.pending.split_to(count));
		Ok(count)
	}
}

#[cfg(test)]
mod tests {
	use super::*;
	use std::io::Cursor;

	#[test]
	fn content_ranges_are_strict_and_bounded() {
		assert_eq!(
			span("bytes 0-99/100"),
			Some(Span {
				start: 0,
				end: 99,
				total: 100
			})
		);
		for invalid in [
			"bytes 0-100/100",
			"bytes 9-0/100",
			"bytes 0-9/*",
			"bytes 0-9/4294967295",
			"0-9/100",
		] {
			assert!(span(invalid).is_none(), "{invalid}");
		}
	}

	/// An encrypted stand-in for a Spotify Ogg file: 167 bytes of header, then
	/// the audio, all of it encrypted with the same key the engine would get
	/// from the keymaster.
	fn encrypted(header: usize, audio: &[u8]) -> Vec<u8> {
		let mut plain = vec![0u8; header];
		plain.extend_from_slice(audio);
		let mut out = Vec::new();
		AudioDecrypt::new(Some(AudioKey([7; 16])), Cursor::new(plain))
			.read_to_end(&mut out)
			.unwrap();
		out
	}

	fn run(header: usize, audio: &[u8], magic: [u8; 4], cut: usize) -> (io::Result<usize>, Vec<u8>) {
		let data = encrypted(header, audio);
		let end = data.len().saturating_sub(cut);
		let (encrypted_sender, encrypted_receiver) = mpsc::channel(2);
		let producer = std::thread::spawn(move || {
			for chunk in data[..end].chunks(113) {
				if encrypted_sender
					.blocking_send(Bytes::copy_from_slice(chunk))
					.is_err()
				{
					break;
				}
			}
		});
		let (sender, mut receiver) = mpsc::channel(2);
		let result = decrypt(AudioKey([7; 16]), encrypted_receiver, sender, header, magic);
		let mut out = Vec::new();
		while let Some(frame) = receiver.blocking_recv() {
			out.extend_from_slice(&frame.unwrap().into_data().unwrap());
		}
		producer.join().unwrap();
		(result, out)
	}

	#[test]
	fn the_header_is_dropped_and_the_magic_is_kept() {
		let mut audio = OGG_MAGIC.to_vec();
		audio.extend((0..50_000).map(|value| value as u8));
		let (result, out) = run(OGG_HEADER_BYTES, &audio, OGG_MAGIC, 0);
		assert_eq!(result.unwrap(), audio.len());
		assert_eq!(out, audio);
	}

	#[test]
	fn a_flac_file_has_no_header_to_drop() {
		let mut audio = FLAC_MAGIC.to_vec();
		audio.extend((0..10_000).map(|value| value as u8));
		let (result, out) = run(0, &audio, FLAC_MAGIC, 0);
		assert_eq!(result.unwrap(), audio.len());
		assert_eq!(out, audio);
	}

	#[test]
	fn a_wrong_magic_is_refused_and_not_served() {
		let mut audio = b"nope".to_vec();
		audio.extend((0..100).map(|value| value as u8));
		let (result, out) = run(OGG_HEADER_BYTES, &audio, OGG_MAGIC, 0);
		assert!(result.is_err());
		assert!(out.is_empty());
	}

	#[test]
	fn a_truncated_transfer_is_reported_as_short_and_not_as_complete() {
		let mut audio = OGG_MAGIC.to_vec();
		audio.extend((0..50_000).map(|value| value as u8));
		let (result, out) = run(OGG_HEADER_BYTES, &audio, OGG_MAGIC, 500);
		assert!(result.unwrap() < audio.len());
		assert!(out.len() < audio.len());
	}
}
