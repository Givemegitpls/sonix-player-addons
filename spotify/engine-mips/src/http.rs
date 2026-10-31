//! The HTTP half: a loopback listener the player opens as a URL.
//!
//! The player does not speak a line protocol to the engine and does not link
//! librespot: it plays a URL, the way it plays an internet radio station
//! (`radio_play()` in `src/system/streaming/radio.h`). So the engine serves one
//! and only one thing -- the decrypted bytes of a track -- at
//!
//! ```text
//! GET http://127.0.0.1:<http-port>/stream/<track id>
//! GET http://127.0.0.1:<http-port>/health
//! ```
//!
//! and everything else on that port is a 404. The listener is bound to
//! `127.0.0.1`, not to `0.0.0.0`: this is a player's own audio pipe, and nothing
//! on the network has any business reaching it.
//!
//! The stream answer has no `Content-Length` and is chunked, because the length
//! of a decrypted track is not known until the last byte: it is whatever the
//! CDN hands over. A player reads it sequentially and closes the connection
//! when it has had enough, which drops the body, which is how the download and
//! the decrypt stop -- there is no other cancellation to write.
//!
//! Every stream holds one of the engine's two stream slots for as long as its
//! body lives. Two tracks at once is what a player that is changing track
//! looks like (the old one has not been closed yet); a third is a bug on the
//! caller's side, not something to queue.

use std::convert::Infallible;
use std::io;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use bytes::Bytes;
use http_body::{Body, Frame};
use http_body_util::combinators::BoxBody;
use http_body_util::{BodyExt, Full};
use hyper::body::Incoming;
use hyper::header::{HeaderValue, CACHE_CONTROL, CONTENT_TYPE};
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use serde_json::{json, Value};
use tokio::net::TcpListener;
use tokio::sync::{mpsc, OwnedSemaphorePermit};

use crate::stream;
use crate::App;

pub type BoxedBody = BoxBody<Bytes, io::Error>;

/// A response body fed by a producer that is not hyper's: the decrypting
/// thread, or the file reader of `--self-test`. When this is dropped -- the
/// player closed the connection -- the sender's next `send` fails and the
/// producer stops.
pub struct ChannelBody {
	receiver: mpsc::Receiver<Result<Frame<Bytes>, io::Error>>,
	/// The stream slot, held for the life of the body rather than for the life
	/// of the request: the point of the limit is bytes in flight.
	_slot: OwnedSemaphorePermit,
}

impl ChannelBody {
	pub fn new(
		receiver: mpsc::Receiver<Result<Frame<Bytes>, io::Error>>,
		slot: OwnedSemaphorePermit,
	) -> Self {
		Self {
			receiver,
			_slot: slot,
		}
	}
}

impl Body for ChannelBody {
	type Data = Bytes;
	type Error = io::Error;

	fn poll_frame(
		self: Pin<&mut Self>,
		cx: &mut Context<'_>,
	) -> Poll<Option<Result<Frame<Self::Data>, Self::Error>>> {
		self.get_mut().receiver.poll_recv(cx)
	}
}

pub fn json_response(status: StatusCode, value: &Value) -> Response<BoxedBody> {
	let bytes = serde_json::to_vec(value).unwrap_or_else(|_| b"{\"ok\":false}".to_vec());
	let mut response = Response::new(body(Bytes::from(bytes)));
	*response.status_mut() = status;
	response
		.headers_mut()
		.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
	response
}

fn stream_response(content_type: &'static str, body: ChannelBody) -> Response<BoxedBody> {
	let mut response = Response::new(BoxBody::new(body));
	response
		.headers_mut()
		.insert(CONTENT_TYPE, HeaderValue::from_static(content_type));
	// A track is served once, for one player; nothing may cache it in between.
	response
		.headers_mut()
		.insert(CACHE_CONTROL, HeaderValue::from_static("no-store"));
	response
}

fn body(bytes: Bytes) -> BoxedBody {
	Full::new(bytes)
		.map_err(|never: Infallible| match never {})
		.boxed()
}

/// Why a stream op failed, as an HTTP status. `not_signed_in` is 401 because
/// that is what the player's own retry logic understands; the rest are a short
/// reason with the code that fits it.
fn status_for(reason: &str) -> StatusCode {
	match reason {
		"not_signed_in" => StatusCode::UNAUTHORIZED,
		"invalid_id" | "invalid query" => StatusCode::BAD_REQUEST,
		"busy" => StatusCode::SERVICE_UNAVAILABLE,
		"track_unavailable" | "premium_required" | "unsupported_format" => StatusCode::NOT_FOUND,
		_ => StatusCode::BAD_GATEWAY,
	}
}

/// `?bitrate=96|160|320`, defaulting to the best the account can have. The
/// player may ask for a cheaper stream on a slow card; anything else is the
/// default rather than an error, because a wrong query string is not a reason
/// to refuse to play.
fn bitrate_from_query(query: Option<&str>) -> u16 {
	query
		.and_then(|query| {
			query
				.split('&')
				.find_map(|pair| pair.strip_prefix("bitrate="))
		})
		.and_then(|value| value.parse::<u16>().ok())
		.filter(|value| matches!(value, 96 | 160 | 320))
		.unwrap_or(320)
}

async fn route(request: Request<Incoming>, app: Arc<App>) -> Response<BoxedBody> {
	if request.method() != Method::GET {
		return json_response(
			StatusCode::METHOD_NOT_ALLOWED,
			&json!({"ok": false, "error": "method not allowed"}),
		);
	}

	let path = request.uri().path().to_string();
	if path == "/health" {
		return json_response(StatusCode::OK, &json!({"ok": true}));
	}
	if let Some(id) = path.strip_prefix("/stream/") {
		let bitrate = bitrate_from_query(request.uri().query());
		return match stream::open(&app, id, bitrate).await {
			Ok(stream) => stream_response(stream.content_type, stream.body),
			Err(reason) => json_response(
				status_for(reason),
				&json!({"ok": false, "error": reason}),
			),
		};
	}
	json_response(
		StatusCode::NOT_FOUND,
		&json!({"ok": false, "error": "not found"}),
	)
}

/// Accepts for as long as the listener lives. Each connection is a task of its
/// own: a player that opens a stream and then a second one must not have the
/// first one wait for it.
pub async fn serve(listener: TcpListener, app: Arc<App>) {
	loop {
		let (stream, _peer) = match listener.accept().await {
			Ok(pair) => pair,
			Err(_) => continue,
		};
		let app = app.clone();
		tokio::spawn(async move {
			let service = service_fn(move |request: Request<Incoming>| {
				let app = app.clone();
				async move { Ok::<_, Infallible>(route(request, app).await) }
			});
			let _ = hyper::server::conn::http1::Builder::new()
				.serve_connection(TokioIo::new(stream), service)
				.await;
		});
	}
}

/// One `GET`, read to the end, for `--self-test`. It is deliberately a client
/// of the same wire protocol the player will use, not a call into the router:
/// what is being checked is that the listener, the framing and the status codes
/// are real, and a direct call would check none of that.
pub async fn get(port: u16, path: &str) -> io::Result<(u16, Vec<u8>)> {
	use tokio::io::{AsyncReadExt, AsyncWriteExt};

	let mut stream = tokio::net::TcpStream::connect(("127.0.0.1", port)).await?;
	let request =
		format!("GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
	stream.write_all(request.as_bytes()).await?;
	let mut raw = Vec::new();
	stream.read_to_end(&mut raw).await?;
	parse_response(&raw)
}

fn parse_response(raw: &[u8]) -> io::Result<(u16, Vec<u8>)> {
	let split = raw
		.windows(4)
		.position(|window| window == b"\r\n\r\n")
		.ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;
	let head = std::str::from_utf8(&raw[..split])
		.map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
	let body = &raw[split + 4..];

	let mut lines = head.split("\r\n");
	let status = lines
		.next()
		.and_then(|line| line.split_whitespace().nth(1))
		.and_then(|code| code.parse::<u16>().ok())
		.ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;

	let mut chunked = false;
	let mut length: Option<usize> = None;
	for line in lines {
		if let Some((name, value)) = line.split_once(':') {
			let name = name.trim().to_ascii_lowercase();
			let value = value.trim();
			if name == "transfer-encoding" && value.eq_ignore_ascii_case("chunked") {
				chunked = true;
			} else if name == "content-length" {
				length = value.parse::<usize>().ok();
			}
		}
	}

	if chunked {
		return Ok((status, dechunk(body)?));
	}
	match length {
		Some(length) if length <= body.len() => Ok((status, body[..length].to_vec())),
		_ => Ok((status, body.to_vec())),
	}
}

fn dechunk(mut body: &[u8]) -> io::Result<Vec<u8>> {
	let mut out = Vec::new();
	loop {
		let end = body
			.windows(2)
			.position(|window| window == b"\r\n")
			.ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;
		let size = std::str::from_utf8(&body[..end])
			.ok()
			.and_then(|line| usize::from_str_radix(line.trim(), 16).ok())
			.ok_or_else(|| io::Error::from(io::ErrorKind::InvalidData))?;
		body = &body[end + 2..];
		if size == 0 {
			return Ok(out);
		}
		if size > body.len() {
			return Err(io::ErrorKind::UnexpectedEof.into());
		}
		out.extend_from_slice(&body[..size]);
		body = &body[size..];
		if body.starts_with(b"\r\n") {
			body = &body[2..];
		}
	}
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn a_chunked_answer_is_put_back_together() {
		let raw = b"HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nOggS\r\n3\r\nabc\r\n0\r\n\r\n";
		let (status, body) = parse_response(raw).unwrap();
		assert_eq!(status, 200);
		assert_eq!(body, b"OggSabc");
	}

	#[test]
	fn a_content_length_answer_is_cut_to_its_length() {
		let raw = b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\n{\"ok\":false}extra";
		let (status, body) = parse_response(raw).unwrap();
		assert_eq!(status, 401);
		assert_eq!(body, b"{\"ok\":false}");
	}

	#[test]
	fn a_truncated_chunk_is_an_error_and_not_a_short_track() {
		let raw = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nOggS";
		assert!(parse_response(raw).is_err());
	}

	#[test]
	fn bitrate_is_read_from_the_query_and_bounded() {
		assert_eq!(bitrate_from_query(None), 320);
		assert_eq!(bitrate_from_query(Some("bitrate=96")), 96);
		assert_eq!(bitrate_from_query(Some("x=1&bitrate=160")), 160);
		assert_eq!(bitrate_from_query(Some("bitrate=1000")), 320);
		assert_eq!(bitrate_from_query(Some("bitrate=abc")), 320);
	}

	#[test]
	fn statuses_say_what_the_player_has_to_do() {
		assert_eq!(status_for("not_signed_in"), StatusCode::UNAUTHORIZED);
		assert_eq!(status_for("invalid_id"), StatusCode::BAD_REQUEST);
		assert_eq!(status_for("busy"), StatusCode::SERVICE_UNAVAILABLE);
		assert_eq!(status_for("stream_failed"), StatusCode::BAD_GATEWAY);
	}
}
