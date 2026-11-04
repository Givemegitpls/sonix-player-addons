//! The librespot half of the Sonix Spotify addon, as a separate static process.
//!
//! The device is an Ingenic X1000: MIPS32r2, little-endian, glibc 2.16. This
//! binary is built for `mipsel-unknown-linux-musl` with the standard library
//! compiled from source and linked statically (see README.md), so the device's
//! glibc is not involved at all: there is no dynamic loader in the result and
//! no `libc.so.6` to ask for a symbol version.
//!
//! It has two faces, and they are for two different callers:
//!
//!   * a **unix socket**, spoken by the addon: one JSON request line, one JSON
//!     response line, then the connection closes. The account and the catalog
//!     live here (`status`, `login`, `logout`, `search`, `playlists`,
//!     `playlist_tracks`, `album_tracks`, `track`).
//!   * a **loopback HTTP listener**, spoken by the player itself, which knows
//!     nothing about Spotify and only knows how to play a URL: it opens
//!     `http://127.0.0.1:<http-port>/stream/<track id>` and reads decrypted
//!     audio out of it, the same way it plays an internet radio station.
//!
//! The split is deliberate. Sonix keeps owning decoding, the queue, the DSP and
//! the DAC; the engine owns the account and the bytes.
//!
//! ```text
//! -> {"op":"probe"}
//! <- {"ok":true,"protocol":1,"librespot":"0.8.0","arch":"mipsel","http_port":8730}
//! -> {"op":"search","query":"never gonna","offset":0,"limit":20}
//! <- {"ok":true,"protocol":1,"items":[{"id":"...","kind":"track",...}],"total":20,"more":true}
//! -> {"op":"nonsense"}
//! <- {"ok":false,"protocol":1,"error":"unknown op"}
//! -> not-json
//! <- {"ok":false,"protocol":1,"error":"invalid request"}
//! ```
//!
//! Logs go to stderr, one line each, prefixed `spotify-engine: `. Nothing that
//! comes from `--credentials` is ever logged, and the account's name is not
//! logged either.

mod catalog;
mod http;
mod session;
mod stream;

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use serde::Deserialize;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, UnixListener, UnixStream};
use tokio::sync::Semaphore;

pub const PROTOCOL: u32 = 1;

/// A request is one line of JSON; this is the largest one that is read.
const REQUEST_MAX: usize = 8192;

/// How long a client may take to send its line, and how long the engine may
/// take to hand back the answer. A client that connects and says nothing must
/// not hold a task forever.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(5);
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(10);

/// The furthest into a list an op may ask to start. The addon pages; a client
/// that asks for row 10 001 is not paging, it is walking the catalog.
const OFFSET_MAX: usize = 10_000;

/// Two streams at once: the one that is playing and the one that was just
/// asked for. A third is a caller bug, answered with `busy`.
const STREAM_SLOTS: usize = 2;

pub fn log(message: impl std::fmt::Display) {
	eprintln!("spotify-engine: {message}");
}

/// librespot reports through the `log` crate, and without a backend those lines
/// are dropped: the one that matters is
/// `librespot does not support "free" accounts`, printed immediately before
/// `Session::check_catalogue` ends the process. Warnings and errors are the
/// whole of what is useful there, so that is the level.
struct StderrLogger;

impl log::Log for StderrLogger {
	fn enabled(&self, metadata: &log::Metadata) -> bool {
		metadata.level() <= log::Level::Warn
	}

	fn log(&self, record: &log::Record) {
		if self.enabled(record.metadata()) {
			let level = match record.level() {
				log::Level::Error => "error",
				log::Level::Warn => "warn",
				_ => "info",
			};
			log(format!("librespot {level}: {}", record.args()));
		}
	}

	fn flush(&self) {}
}

static LOGGER: StderrLogger = StderrLogger;

/// The target this was compiled for, in the names the addon and the protocol
/// use. Read from `cfg!`, not from `std::env::consts`, because `mipsel` is a
/// distinction that lives in the target triple: `consts::ARCH` says "mips" for
/// both endiannesses.
fn arch() -> &'static str {
	if cfg!(all(target_arch = "mips", target_endian = "little")) {
		"mipsel"
	} else if cfg!(all(target_arch = "mips", target_endian = "big")) {
		"mips"
	} else {
		std::env::consts::ARCH
	}
}

pub struct App {
	pub session: session::Manager,
	pub streams: Arc<Semaphore>,
	/// The port the HTTP listener was given, so `probe` can say where the
	/// player has to look. Zero in `--self-test`, which binds an ephemeral one.
	pub http_port: u16,
	/// Only in `--self-test --stream-from-file`: what `/stream/selftest`
	/// serves instead of a track.
	pub self_test_file: Option<PathBuf>,
}

pub(crate) fn failure(error: &str) -> Value {
	json!({"ok": false, "protocol": PROTOCOL, "error": error})
}

#[derive(Deserialize)]
struct Request {
	op: String,
	#[serde(default)]
	id: String,
	#[serde(default)]
	query: String,
	#[serde(default)]
	offset: usize,
	#[serde(default)]
	limit: Option<usize>,
}

/// The answer to `probe`. Every field is something the caller can use to tell
/// this engine apart from a stub: the librespot version is read out of the
/// crate that is linked in, the architecture is the target the binary was
/// built for (so an x86-64 build copied to the card cannot pass), and the port
/// is where the player has to open the track URL.
fn probe(app: &App) -> Value {
	json!({
		"ok": true,
		"protocol": PROTOCOL,
		"librespot": librespot_core::version::SEMVER,
		"arch": arch(),
		"http_port": app.http_port,
	})
}

async fn handle(request: Request, app: &Arc<App>) -> Value {
	match request.op.as_str() {
		"probe" => probe(app),
		"status" => app.session.status().await,
		"login" => app.session.login().await,
		"oauth_start" => app.session.oauth_start().await,
		"oauth_finish" => app.session.oauth_finish().await,
		"logout" => app.session.logout().await,
		"search" | "playlists" | "playlist_tracks" | "album_tracks" | "track" => {
			catalog_op(request, app).await
		}
		_ => failure("unknown op"),
	}
}

async fn catalog_op(request: Request, app: &Arc<App>) -> Value {
	let limit = request
		.limit
		.unwrap_or(catalog::PAGE_DEFAULT)
		.clamp(1, catalog::PAGE_MAX);
	if request.offset > OFFSET_MAX {
		return failure("invalid request");
	}
	// No session, no catalog: the token below is the account's, and a list
	// without an account would be a list nobody asked for.
	if let Err(error) = app.session.session().await {
		return failure(error);
	}
	// The catalog is the Web API (catalog.rs says why). Its bearer token is not
	// the session's auth data -- after an access-point login librespot replaces
	// that with the server's reusable credential, which the Web API refuses with
	// a 401. `access_token` answers the account's own token: the authorization's
	// when the card has one (refreshed if it is about to expire), the pasted one
	// otherwise.
	let Some(token) = app.session.access_token().await else {
		return failure("token_expired");
	};
	let catalog = catalog::Catalog::new(token, app.session.proxy().as_deref());

	let page = match request.op.as_str() {
		"search" => catalog.search(&request.query, request.offset, limit).await,
		"playlists" => catalog.playlists(request.offset, limit).await,
		"playlist_tracks" => {
			catalog
				.playlist_tracks(&request.id, request.offset, limit)
				.await
		}
		"album_tracks" => {
			catalog
				.album_tracks(&request.id, request.offset, limit)
				.await
		}
		_ => catalog.track(&request.id).await,
	};

	match page {
		Ok(page) => json!({
			"ok": true,
			"protocol": PROTOCOL,
			"items": page.items,
			"total": page.total,
			"more": page.more,
		}),
		Err(error) => failure(error),
	}
}

/// One request, one response line, then the connection closes. Keeping it to a
/// single exchange means a client that crashes mid-write cannot leave a half
/// request in the engine's state.
async fn serve(mut stream: UnixStream, app: Arc<App>) {
	let mut line = Vec::with_capacity(256);

	let read = async {
		let mut reader = BufReader::new(&mut stream);
		loop {
			let count = reader.read_until(b'\n', &mut line).await?;
			if count == 0 {
				return Err::<(), std::io::Error>(std::io::ErrorKind::UnexpectedEof.into());
			}
			if line.len() > REQUEST_MAX {
				return Err(std::io::ErrorKind::InvalidData.into());
			}
			if line.ends_with(b"\n") {
				return Ok(());
			}
		}
	};

	if !matches!(tokio::time::timeout(REQUEST_TIMEOUT, read).await, Ok(Ok(()))) {
		log("dropped a client that sent no request line");
		return;
	}

	let response = match serde_json::from_slice::<Request>(&line) {
		Ok(request) => {
			let op = request.op.clone();
			match tokio::time::timeout(RESPONSE_TIMEOUT, handle(request, &app)).await {
				Ok(response) => response,
				Err(_) => {
					log(format!("{op}: no answer within {RESPONSE_TIMEOUT:?}"));
					failure("timeout")
				}
			}
		}
		Err(_) => failure("invalid request"),
	};

	let mut bytes = serde_json::to_vec(&response).unwrap_or_default();
	bytes.push(b'\n');
	let _ = tokio::time::timeout(Duration::from_secs(2), stream.write_all(&bytes)).await;
	let _ = stream.shutdown().await;
}

struct Args {
	socket: PathBuf,
	http_port: u16,
	/// The INI file with the account. Optional: a state directory that already
	/// holds a cached credential is enough to log in.
	credentials: Option<PathBuf>,
	state_dir: PathBuf,
}

enum Mode {
	Serve(Args),
	/// Prints what the linked librespot says about itself, plus what the HTTP
	/// half answers, and exits.
	SelfTest(Option<PathBuf>),
	Version,
}

fn parse_args() -> Result<Mode, String> {
	let mut socket = None;
	let mut http_port = None;
	let mut credentials = None;
	let mut state_dir = None;
	let mut stream_from_file = None;
	let mut mode = None;
	let mut args = std::env::args().skip(1);

	while let Some(arg) = args.next() {
		match arg.as_str() {
			"--socket" => socket = args.next().map(PathBuf::from),
			"--http-port" => {
				let value = args.next().ok_or("--http-port needs a number")?;
				let port = value.parse::<u16>().map_err(|_| "invalid --http-port")?;
				if port == 0 {
					return Err("--http-port 0 is not a port to listen on".to_string());
				}
				http_port = Some(port);
			}
			"--credentials" => credentials = args.next().map(PathBuf::from),
			"--state-dir" => state_dir = args.next().map(PathBuf::from),
			"--stream-from-file" => stream_from_file = args.next().map(PathBuf::from),
			"--self-test" => mode = Some(Mode::SelfTest(stream_from_file.clone())),
			"--version" => mode = Some(Mode::Version),
			other => return Err(format!("unknown argument {other}")),
		}
	}

	match mode {
		Some(mode) => Ok(mode),
		None => Ok(Mode::Serve(Args {
			socket: socket.ok_or("--socket <path> is required")?,
			http_port: http_port.ok_or("--http-port <n> is required")?,
			credentials,
			state_dir: state_dir.ok_or("--state-dir <dir> is required")?,
		})),
	}
}

async fn run(args: Args) -> std::io::Result<()> {
	let socket = args.socket.clone();

	// A socket left behind by a killed engine is not an error: the client's
	// connect() gets ECONNREFUSED on it, and unlinking it is what makes the
	// next start possible. A live socket, on the other hand, belongs to
	// somebody else.
	match std::fs::symlink_metadata(&socket) {
		Ok(metadata) => {
			use std::os::unix::fs::FileTypeExt;
			if !metadata.file_type().is_socket() {
				return Err(std::io::Error::new(
					std::io::ErrorKind::AddrInUse,
					format!("{} exists and is not a socket", socket.display()),
				));
			}
			match UnixStream::connect(&socket).await {
				Ok(_) => {
					return Err(std::io::Error::new(
						std::io::ErrorKind::AddrInUse,
						format!("{} is already being served", socket.display()),
					))
				}
				Err(_) => {
					std::fs::remove_file(&socket)?;
				}
			}
		}
		Err(_) => {}
	}

	let listener = UnixListener::bind(&socket)?;
	{
		use std::os::unix::fs::PermissionsExt;
		std::fs::set_permissions(&socket, std::fs::Permissions::from_mode(0o600))?;
	}
	log(format!(
		"listening on {} (protocol {PROTOCOL})",
		socket.display()
	));

	// Loopback only, and that is the whole access control of the audio pipe:
	// the card's owner is the only user on the device, and nothing on the
	// network has a way to ask this port for a track.
	if let Err(error) = loopback_up() {
		log(format!("loopback: cannot raise lo: {error}"));
	}
	let http_listener = match TcpListener::bind(("127.0.0.1", args.http_port)).await {
		Ok(listener) => listener,
		Err(error) => {
			return Err(std::io::Error::new(
				error.kind(),
				format!(
					"cannot bind 127.0.0.1:{}: {error} (is the loopback interface up?)",
					args.http_port
				),
			))
		}
	};
	let http_port = http_listener.local_addr()?.port();
	log(format!("http on 127.0.0.1:{http_port}"));

	let app = Arc::new(App {
		session: session::Manager::new(args.credentials.clone(), args.state_dir.clone()),
		streams: Arc::new(Semaphore::new(STREAM_SLOTS)),
		http_port,
		self_test_file: None,
	});

	let http_task = tokio::spawn(http::serve(http_listener, app.clone()));

	let mut terminate = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
	let mut interrupt = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())?;

	let result = loop {
		tokio::select! {
			accepted = listener.accept() => match accepted {
				Ok((stream, _)) => {
					let app = app.clone();
					tokio::spawn(serve(stream, app));
				}
				Err(error) => break Err(error),
			},
			_ = terminate.recv() => {
				log("SIGTERM, exiting");
				break Ok(());
			}
			_ = interrupt.recv() => {
				log("SIGINT, exiting");
				break Ok(());
			}
		}
	};

	http_task.abort();
	app.session.close().await;
	let _ = std::fs::remove_file(&socket);
	result
}

/// SHA-256 of the empty string, the value ring has to reproduce.
const EMPTY_SHA256: [u8; 32] = [
	0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
	0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
];

/// What the HTTP half answered, checked over a real socket: the listener is
/// up, `/health` is JSON with `ok`, an unauthenticated `/stream/...` is a 401,
/// and -- with `--stream-from-file` -- a file comes back byte for byte through
/// the chunked body.
async fn http_self_test(
	path: Option<PathBuf>,
) -> (bool, u16, Option<(bool, usize, bool)>) {
	let app = Arc::new(App {
		session: session::Manager::new(None, std::env::temp_dir().join("spotify-engine-selftest")),
		streams: Arc::new(Semaphore::new(STREAM_SLOTS)),
		http_port: 0,
		self_test_file: path.clone(),
	});

	let listener = match TcpListener::bind(("127.0.0.1", 0)).await {
		Ok(listener) => listener,
		Err(error) => {
			log(format!("self-test: cannot bind a loopback port: {error}"));
			return (false, 0, None);
		}
	};
	let port = match listener.local_addr() {
		Ok(address) => address.port(),
		Err(error) => {
			log(format!("self-test: no local address: {error}"));
			return (false, 0, None);
		}
	};
	let server = tokio::spawn(http::serve(listener, app));

	let health = match http::get(port, "/health").await {
		Ok((200, body)) => serde_json::from_slice::<Value>(&body)
			.is_ok_and(|value| value["ok"] == true),
		_ => false,
	};
	let unauthorized = http::get(port, "/stream/x")
		.await
		.map(|(status, _)| status)
		.unwrap_or(0);

	let file = match path {
		Some(path) => {
			let expected = std::fs::read(&path).unwrap_or_default();
			match http::get(port, "/stream/selftest").await {
				Ok((status, body)) => Some((
					status == 200,
					body.len(),
					!expected.is_empty() && body == expected,
				)),
				Err(error) => {
					log(format!("self-test: /stream/selftest: {error}"));
					Some((false, 0, false))
				}
			}
		}
		None => None,
	};

	server.abort();
	(health, unauthorized, file)
}

fn self_test(stream_from_file: Option<PathBuf>) -> i32 {
	use librespot_core::config::SessionConfig;

	// The same interface the engine raises before it serves: the self-test
	// binds a loopback port of its own, and on this device that fails outright
	// while `lo` is down -- which is how it boots.
	if let Err(error) = loopback_up() {
		log(format!("self-test: cannot raise lo: {error}"));
	}

	// `SessionConfig::default()` mints a UUID from the OS random source, so two
	// of them being different is the cheapest proof that `getrandom` works in
	// a static musl binary on a 3.10 kernel (where the syscall does not exist
	// and the fallback is /dev/urandom).
	let first = SessionConfig::default();
	let second = SessionConfig::default();
	let random_ready = !first.device_id.is_empty() && first.device_id != second.device_id;

	// The library half, exercised rather than merely compiled: a default
	// protobuf message, the metadata parser fed with it, and the fetch
	// parameters. `prefetch_threshold_factor` is an f32 that comes out of
	// library code, which is the one float this binary can show without audio
	// -- and the `-soft-float` flag exists precisely so that floats crossing
	// into C code are not silently wrong.
	let fetch = librespot_audio::AudioFetchParams::default();
	let empty_track = librespot_protocol::metadata::Track::default();
	let metadata_parser = match librespot_metadata::Track::try_from(&empty_track) {
		Ok(_) => "accepted a default message",
		// What a message with no uri and no name is supposed to get.
		Err(_) => "rejected a default message",
	};

	// ring's C half: SHA-256 of the empty string is e3b0c442...b855, and this is
	// the code TLS will run on the device.
	let digest = ring::digest::digest(&ring::digest::SHA256, b"");
	let ring_sha256_ok = digest.as_ref() == EMPTY_SHA256.as_slice();

	// The HTTP half, over a real loopback socket.
	let runtime = tokio::runtime::Builder::new_current_thread()
		.enable_all()
		.max_blocking_threads(2)
		.build();
	let (health, unauthorized, file) = match runtime {
		Ok(runtime) => runtime.block_on(http_self_test(stream_from_file)),
		Err(error) => {
			log(format!("self-test: cannot start the runtime: {error}"));
			(false, 0, None)
		}
	};
	let http_ok = health && unauthorized == 401;
	let file_ok = file.as_ref().is_none_or(|(served, _, matches)| *served && *matches);

	let mut report = json!({
		"protocol": PROTOCOL,
		"librespot": librespot_core::version::SEMVER,
		"arch": arch(),
		"os_random_ready": random_ready,
		"audio_fetch": {
			"minimum_download_size": fetch.minimum_download_size,
			"prefetch_threshold_factor": fetch.prefetch_threshold_factor,
		},
		"metadata_parser": metadata_parser,
		"ring_sha256_ok": ring_sha256_ok,
		"http": {
			"health": health,
			"stream_unauthenticated": unauthorized,
		},
	});
	if let Some((served, bytes, matches)) = file {
		report["stream_from_file"] = json!({
			"served": served,
			"bytes": bytes,
			"matches": matches,
		});
	}
	println!("{report}");

	if !random_ready {
		2
	} else if !ring_sha256_ok {
		3
	} else if !http_ok {
		4
	} else if !file_ok {
		5
	} else {
		0
	}
}

// The device boots with `lo` administratively down -- the kernel is 3.10, there
// is no init script, and nothing else on this system wants loopback -- so a bind
// to 127.0.0.1 fails with EADDRNOTAVAIL and the audio pipe never exists. The
// engine raises the interface itself rather than asking the player to: the pipe
// is loopback-only by design, and this is the only process that needs it.
fn loopback_up() -> std::io::Result<()> {
	unsafe {
		let fd = libc::socket(libc::AF_INET, libc::SOCK_DGRAM, 0);
		if fd < 0 {
			return Err(std::io::Error::last_os_error());
		}
		let mut request: libc::ifreq = std::mem::zeroed();
		for (slot, byte) in request.ifr_name.iter_mut().zip(b"lo\0".iter()) {
			*slot = *byte as libc::c_char;
		}
		if libc::ioctl(fd, libc::SIOCGIFFLAGS as libc::Ioctl, &mut request) < 0 {
			let error = std::io::Error::last_os_error();
			libc::close(fd);
			return Err(error);
		}
		let want = (libc::IFF_UP | libc::IFF_RUNNING) as libc::c_short;
		request.ifr_ifru.ifru_flags |= want;
		let result = libc::ioctl(fd, libc::SIOCSIFFLAGS as libc::Ioctl, &request);
		let error = std::io::Error::last_os_error();
		libc::close(fd);
		if result < 0 {
			Err(error)
		} else {
			Ok(())
		}
	}
}

fn main() {
	// Before anything librespot does: the line that explains a process ending
	// itself is written through `log` (StderrLogger).
	let _ = log::set_logger(&LOGGER);
	log::set_max_level(log::LevelFilter::Warn);

	let mode = match parse_args() {
		Ok(mode) => mode,
		Err(error) => {
			log(error);
			std::process::exit(1);
		}
	};

	match mode {
		Mode::Version => {
			println!(
				"{}",
				json!({"protocol": PROTOCOL, "librespot": librespot_core::version::SEMVER, "arch": arch()})
			);
		}
		Mode::SelfTest(stream_from_file) => std::process::exit(self_test(stream_from_file)),
		Mode::Serve(args) => {
			log(format!(
				"starting: librespot {}, {}",
				librespot_core::version::SEMVER,
				arch()
			));
			match &args.credentials {
				Some(path) => log(format!("credentials {}", describe(path))),
				None => log("no --credentials given; only a cached login can be used"),
			}
			log(format!("state {}", describe(&args.state_dir)));

			let runtime = match tokio::runtime::Builder::new_current_thread()
				.enable_all()
				.max_blocking_threads(4)
				.build()
			{
				Ok(runtime) => runtime,
				Err(error) => {
					log(format!("cannot start the runtime: {error}"));
					std::process::exit(1);
				}
			};

			let result = runtime.block_on(run(args));
			runtime.shutdown_timeout(Duration::from_secs(2));

			if let Err(error) = result {
				log(format!("fatal: {error}"));
				std::process::exit(1);
			}
		}
	}
}

/// Paths are logged, contents never are; this only marks an empty value so a
/// misquoted command line is visible.
fn describe(path: &std::path::Path) -> String {
	if path.as_os_str().is_empty() {
		"(empty)".to_string()
	} else {
		path.display().to_string()
	}
}
