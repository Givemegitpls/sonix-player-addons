//! What the engine is, and what it tells the addon.
//!
//! The engine is the Spotify protocol half of the addon, and the addon is the
//! interface half. They are two libraries because the protocol half is Rust
//! and the interface half is C that talks to LVGL, and because a card that
//! carries the addon may carry an engine that is newer or older than it: the
//! protocol below is a handful of C functions and one line of JSON per event,
//! so the two can disagree about everything except that.
//!
//! What is here today is the protocol and the session state machine. The
//! transport -- the part that actually speaks Spotify -- is not: it is
//! librespot, and this tree has no vendored copy of it, so a build from this
//! source cannot open a session. Rather than pretend, `login` answers
//! `transport_unavailable` and the addon says so on its screen. See README.md
//! for what has to be true before that changes.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::json;
use crate::queue::Queue;

/// The version of the C surface. Bumped when a function changes meaning; the
/// addon compares it with what it was built against and refuses a mismatch
/// with a sentence instead of a call through a changed signature.
pub const ABI: i32 = 1;

/// `CARGO_PKG_VERSION`, reported on the init event so the log says which
/// engine the card is actually running.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The longest token accepted. A Spotify access token is a few hundred bytes;
/// a refresh token is longer. Anything past this is not a token.
pub const TOKEN_MAX: usize = 4096;

/// Between two scripted events of the self-test. Long enough that the addon's
/// 250 ms timer shows them one after another rather than all at once.
const SELFTEST_STEP: Duration = Duration::from_millis(120);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
	/// Initialised, no session, nothing in flight.
	Ready,
	/// A login is being attempted.
	Authenticating,
	/// A session exists. Only the self-test reaches this today.
	Authenticated,
	/// Signed out on request; the account is the addon's business.
	SignedOut,
	/// The last attempt failed; `error` says why.
	Failed,
	/// `spotify_engine_shutdown` ran. Nothing starts again.
	Closed,
}

impl State {
	pub fn name(self) -> &'static str {
		match self {
			State::Ready => "ready",
			State::Authenticating => "authenticating",
			State::Authenticated => "authenticated",
			State::SignedOut => "signed_out",
			State::Failed => "failed",
			State::Closed => "closed",
		}
	}
}

/// What `spotify_engine_init` was told. Only the session directory is really
/// used today; the rest is here because the addon already knows it and the
/// transport will want it (a device name is what Spotify shows in its own
/// "devices" list).
#[derive(Clone, Debug, PartialEq)]
pub struct Config {
	pub session_dir: Option<String>,
	pub device_name: String,
	pub bitrate: u16,
}

impl Default for Config {
	fn default() -> Self {
		Self { session_dir: None, device_name: "Sonix".to_string(), bitrate: 160 }
	}
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ConfigError {
	/// Not JSON, or not an object.
	Malformed,
	/// A field is there and is not what it has to be.
	Field,
}

impl Config {
	pub fn parse(text: &str) -> Result<Self, ConfigError> {
		let value = json::parse(text).map_err(|_| ConfigError::Malformed)?;
		if !matches!(value, json::Value::Object(_)) {
			return Err(ConfigError::Malformed);
		}
		let mut config = Config::default();
		if let Some(dir) = value.get("session_dir") {
			let dir = dir.as_str().ok_or(ConfigError::Field)?;
			if dir.is_empty() || dir.len() > 400 || dir.chars().any(char::is_control) {
				return Err(ConfigError::Field);
			}
			config.session_dir = Some(dir.to_string());
		}
		if let Some(name) = value.get("device_name") {
			let name = name.as_str().ok_or(ConfigError::Field)?;
			if name.is_empty() || name.chars().count() > 64 || name.chars().any(char::is_control) {
				return Err(ConfigError::Field);
			}
			config.device_name = name.to_string();
		}
		if let Some(bitrate) = value.get("bitrate") {
			let bitrate = bitrate.as_u64().ok_or(ConfigError::Field)?;
			if !matches!(bitrate, 96 | 160 | 320) {
				return Err(ConfigError::Field);
			}
			config.bitrate = bitrate as u16;
		}
		Ok(config)
	}
}

/// A login request, as the addon sends it.
/// What one `poll` found. `TooLong` carries no text: the caller's buffer is
/// the only place an event may be written, and it did not fit.
#[derive(Debug, PartialEq, Eq)]
pub enum Poll {
	Event(String),
	Empty,
	TooLong,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LoginKind {
	/// `{"kind":"token","token":"..."}`: a token the user pasted.
	Token,
	/// `{"kind":"selftest"}`: the scripted sequence, no account and no network.
	SelfTest,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LoginError {
	Closed,
	Malformed,
	BadKind,
	BadToken,
}

/// One flat JSON object, built field by field. Events are one line each, so
/// the object is never nested and never formatted for reading.
pub struct Event {
	text: String,
}

impl Event {
	pub fn new(kind: &str) -> Self {
		let mut text = String::with_capacity(96);
		text.push_str("{\"v\":");
		text.push_str(&ABI.to_string());
		text.push_str(",\"event\":");
		text.push_str(&json::string(kind));
		Self { text }
	}

	pub fn text(mut self, key: &str, value: &str) -> Self {
		self.text.push(',');
		self.text.push_str(&json::string(key));
		self.text.push(':');
		self.text.push_str(&json::string(value));
		self
	}

	pub fn number(mut self, key: &str, value: u64) -> Self {
		self.text.push(',');
		self.text.push_str(&json::string(key));
		self.text.push(':');
		self.text.push_str(&value.to_string());
		self
	}

	pub fn boolean(mut self, key: &str, value: bool) -> Self {
		self.text.push(',');
		self.text.push_str(&json::string(key));
		self.text.push(':');
		self.text.push_str(if value { "true" } else { "false" });
		self
	}

	pub fn finish(mut self) -> String {
		self.text.push('}');
		self.text
	}
}

pub struct Engine {
	config: Config,
	/// Shared with the worker, which sets the state when an attempt ends. One
	/// cell, not two: the addon polls an event and reads the state behind it,
	/// and the two must not be able to disagree.
	state: Arc<Mutex<State>>,
	queue: Arc<Queue>,
	/// The flag the running worker watches. Replaced, not reset, when a new
	/// login starts: a worker that is still winding down must see its own flag
	/// go up and not the new one go down.
	worker_stop: Mutex<Option<Arc<AtomicBool>>>,
	worker: Mutex<Option<JoinHandle<()>>>,
}

impl Engine {
	pub fn new(config: Config) -> Self {
		let engine = Self {
			config,
			state: Arc::new(Mutex::new(State::Ready)),
			queue: Arc::new(Queue::new()),
			worker_stop: Mutex::new(None),
			worker: Mutex::new(None),
		};
		engine.emit(
			Event::new("init")
				.text("engine", "spotify")
				.text("version", VERSION)
				.number("abi", ABI as u64)
				// Not a secret, and the one thing that explains every failed
				// login in this build: the protocol half is here, the Spotify
				// transport is not.
				.text("transport", "unavailable")
				.text("state", engine.state().name()),
		);
		engine.emit(Event::new("log").text("level", "info").text(
			"message",
			"engine initialised; the Spotify transport is not built into this engine (see README.md)",
		));
		engine
	}

	pub fn state(&self) -> State {
		*self.state.lock().unwrap_or_else(|poison| poison.into_inner())
	}

	fn set_state(&self, state: State) {
		*self.state.lock().unwrap_or_else(|poison| poison.into_inner()) = state;
	}

	/// One line onto the queue, with a note first if the addon fell so far
	/// behind that events had to be dropped.
	pub fn emit(&self, event: Event) {
		let dropped = self.queue.take_dropped();
		if dropped > 0 {
			self.queue.push(
				Event::new("log")
					.text("level", "warn")
					.text("message", &format!("{dropped} events dropped: the addon is not polling"))
					.finish(),
			);
		}
		self.queue.push(event.finish());
	}

	pub fn login_start(&self, request: &str) -> Result<(), LoginError> {
		let state = self.state();
		if state == State::Closed {
			return Err(LoginError::Closed);
		}
		let value = json::parse(request).map_err(|_| LoginError::Malformed)?;
		let kind = value.get("kind").and_then(json::Value::as_str).ok_or(LoginError::Malformed)?;
		match kind {
			"token" => {
				let token = value.get("token").and_then(json::Value::as_str).ok_or(LoginError::BadToken)?;
				if !valid_token(token) {
					// A request the engine will not act on is not a failed
					// session: the state stays where it was and the log says
					// what was wrong with the text.
					self.emit(Event::new("log").text("level", "warn").text(
						"message",
						"login refused: the token is empty, too long, or has characters a token cannot have",
					));
					return Err(LoginError::BadToken);
				}
				// The length is all the log gets: the token is a credential and
				// a log on a card is a file somebody else may read.
				self.emit(
					Event::new("log")
						.text("level", "info")
						.text("message", &format!("login: token accepted for the attempt, {} bytes", token.len())),
				);
				self.begin(LoginKind::Token, Some(token.to_string()))
			}
			"signout" => {
				self.stop_worker();
				self.set_state(State::SignedOut);
				self.emit(Event::new("login").text("state", State::SignedOut.name()));
				Ok(())
			}
			"selftest" => self.begin(LoginKind::SelfTest, None),
			_ => Err(LoginError::BadKind),
		}
	}

	/// Starts the worker for one attempt. The previous one, if any, is told to
	/// stop and left to finish on its own: joining it here would put a sleep of
	/// its own between the tap and the screen.
	fn begin(&self, kind: LoginKind, token: Option<String>) -> Result<(), LoginError> {
		self.stop_worker();
		let stop = Arc::new(AtomicBool::new(false));
		*self.worker_stop.lock().unwrap_or_else(|poison| poison.into_inner()) = Some(stop.clone());
		self.set_state(State::Authenticating);
		self.emit(Event::new("login").text("state", State::Authenticating.name()));

		let queue = self.queue.clone();
		let shared = self.state.clone();
		let config = self.config.clone();
		let handle = thread::Builder::new()
			.name("spotify-login".to_string())
			.spawn(move || match kind {
				LoginKind::SelfTest => selftest(&queue, &shared, &stop),
				LoginKind::Token => token_attempt(&queue, &shared, &stop, &config, token),
			})
			.map_err(|_| LoginError::Malformed)?;
		*self.worker.lock().unwrap_or_else(|poison| poison.into_inner()) = Some(handle);
		Ok(())
	}

	fn stop_worker(&self) {
		if let Some(stop) = self.worker_stop.lock().unwrap_or_else(|poison| poison.into_inner()).take() {
			stop.store(true, Ordering::Relaxed);
		}
		// Detached: see begin(). The handle is dropped, not joined.
		let _ = self.worker.lock().unwrap_or_else(|poison| poison.into_inner()).take();
	}

	pub fn shutdown(&self) -> bool {
		if self.state() == State::Closed {
			return false;
		}
		if let Some(stop) = self.worker_stop.lock().unwrap_or_else(|poison| poison.into_inner()).take() {
			stop.store(true, Ordering::Relaxed);
		}
		let worker = self.worker.lock().unwrap_or_else(|poison| poison.into_inner()).take();
		if let Some(worker) = worker {
			let _ = worker.join();
		}
		self.set_state(State::Closed);
		self.emit(Event::new("shutdown").text("state", State::Closed.name()));
		true
	}

	/// One event, at most `capacity - 1` bytes, without the newline the addon
	/// puts in its log line itself.
	pub fn poll(&self, capacity: usize) -> Poll {
		match self.queue.pop() {
			None => Poll::Empty,
			// Dropped, not truncated: half an event is not JSON, and the addon
			// logs the error code so the reason is not silent.
			Some(event) if event.len() >= capacity => Poll::TooLong,
			Some(event) => Poll::Event(event),
		}
	}
}

/// A token is pasted by hand, so the check is only "this could be a token":
/// the transport is what decides whether Spotify accepts it.
pub fn valid_token(token: &str) -> bool {
	!token.is_empty()
		&& token.len() <= TOKEN_MAX
		&& token.bytes().all(|byte| (0x21..=0x7e).contains(&byte))
}

/// Pushes one event and records the state it implies.
fn report(queue: &Arc<Queue>, shared: &Arc<Mutex<State>>, state: State, event: Event) {
	*shared.lock().unwrap_or_else(|poison| poison.into_inner()) = state;
	queue.push(event.finish());
}

/// The scripted sequence: what the protocol looks like end to end with no
/// account and no network. Everything it says is marked `offline`, so a
/// screenshot cannot be mistaken for a working session.
fn selftest(queue: &Arc<Queue>, shared: &Arc<Mutex<State>>, stop: &Arc<AtomicBool>) {
	let steps: [(State, Event); 2] = [
		(
			State::Authenticated,
			Event::new("login")
				.text("state", "authenticated")
				.boolean("offline", true)
				.text("account", "self-test"),
		),
		(
			State::Authenticated,
			Event::new("log")
				.text("level", "info")
				.boolean("offline", true)
				.text("message", "self-test finished: the protocol works, no Spotify session was opened"),
		),
	];
	for (state, event) in steps {
		if stop.load(Ordering::Relaxed) {
			return;
		}
		report(queue, shared, state, event);
		thread::sleep(SELFTEST_STEP);
	}
}

/// The attempt a pasted token starts. This is where the transport would go:
/// librespot's session, its credentials store, the "premium" check the rekkod
/// tree did, and the first catalogue request. It is not here, so the answer is
/// the honest one.
fn token_attempt(
	queue: &Arc<Queue>,
	shared: &Arc<Mutex<State>>,
	stop: &Arc<AtomicBool>,
	_config: &Config,
	token: Option<String>,
) {
	if stop.load(Ordering::Relaxed) {
		return;
	}
	let bytes = token.as_ref().map(|token| token.len()).unwrap_or(0);
	// The token itself is dropped here and never written anywhere: the addon
	// keeps it on the card, the engine keeps it only for the length of an
	// attempt, and there is no attempt to make.
	drop(token);
	report(
		queue,
		shared,
		State::Failed,
		Event::new("login")
			.text("state", "failed")
			.text("error", "transport_unavailable")
			.text("message", "the engine was built without the Spotify transport; a token of this length cannot be used")
			.number("token_bytes", bytes as u64),
	);
}

#[cfg(test)]
mod tests {
	use super::*;

	/// Everything waiting, in order: what the addon's timer does in a loop.
	fn drain(engine: &Engine) -> Vec<String> {
		let mut events = Vec::new();
		loop {
			match engine.poll(1024) {
				Poll::Event(event) => events.push(event),
				Poll::Empty | Poll::TooLong => return events,
			}
		}
	}

	#[test]
	fn config_defaults_are_the_documented_ones() {
		let config = Config::parse("{}").unwrap();
		assert_eq!(config, Config::default());
		assert_eq!(config.bitrate, 160);
		assert_eq!(config.device_name, "Sonix");
		assert_eq!(config.session_dir, None);
	}

	#[test]
	fn config_reads_the_fields_the_addon_sends() {
		let config = Config::parse(r#"{"session_dir":"/mnt/sd_0/Addons/spotify","device_name":"V3","bitrate":320}"#).unwrap();
		assert_eq!(config.session_dir.as_deref(), Some("/mnt/sd_0/Addons/spotify"));
		assert_eq!(config.device_name, "V3");
		assert_eq!(config.bitrate, 320);
	}

	#[test]
	fn config_refuses_what_it_cannot_use() {
		assert_eq!(Config::parse("not json"), Err(ConfigError::Malformed));
		assert_eq!(Config::parse("[]"), Err(ConfigError::Malformed));
		assert_eq!(Config::parse(r#"{"bitrate":128}"#), Err(ConfigError::Field));
		assert_eq!(Config::parse(r#"{"bitrate":"320"}"#), Err(ConfigError::Field));
		assert_eq!(Config::parse(r#"{"session_dir":""}"#), Err(ConfigError::Field));
		assert_eq!(Config::parse(r#"{"session_dir":"a\nb"}"#), Err(ConfigError::Field));
	}

	#[test]
	fn init_says_what_is_missing_before_anything_is_asked() {
		let engine = Engine::new(Config::default());
		let Poll::Event(init) = engine.poll(1024) else { panic!("an init event") };
		assert!(init.contains(r#""event":"init""#), "{init}");
		assert!(init.contains(r#""transport":"unavailable""#), "{init}");
		assert!(init.contains(r#""state":"ready""#), "{init}");
		let Poll::Event(log) = engine.poll(1024) else { panic!("a log line") };
		assert!(log.contains(r#""event":"log""#), "{log}");
		assert_eq!(engine.state(), State::Ready);
	}

	#[test]
	fn a_token_attempt_ends_with_the_truth_about_the_transport() {
		let engine = Engine::new(Config::default());
		drain(&engine);
		engine.login_start(r#"{"kind":"token","token":"BQDexample"}"#).unwrap();
		assert_eq!(engine.state(), State::Authenticating);
		// The worker runs on its own thread: wait for it the way the addon
		// would, by polling.
		let mut seen = Vec::new();
		for _ in 0..200 {
			seen.extend(drain(&engine));
			if engine.state() == State::Failed {
				break;
			}
			thread::sleep(Duration::from_millis(5));
		}
		assert_eq!(engine.state(), State::Failed);
		assert!(seen.iter().any(|event| event.contains("transport_unavailable")), "{seen:?}");
		assert!(seen.iter().all(|event| !event.contains("BQDexample")), "the token reached the log: {seen:?}");
	}

	#[test]
	fn a_broken_request_is_refused_and_the_state_stays() {
		let engine = Engine::new(Config::default());
		drain(&engine);
		assert_eq!(engine.login_start(""), Err(LoginError::Malformed));
		assert_eq!(engine.login_start(r#"{"kind":"password"}"#), Err(LoginError::BadKind));
		assert_eq!(engine.login_start(r#"{"kind":"token"}"#), Err(LoginError::BadToken));
		assert_eq!(engine.login_start(r#"{"kind":"token","token":"has space"}"#), Err(LoginError::BadToken));
		assert_eq!(engine.login_start(r#"{"kind":"token","token":""}"#), Err(LoginError::BadToken));
		assert_eq!(engine.state(), State::Ready);
	}

	#[test]
	fn the_self_test_reaches_authenticated_and_says_it_is_offline() {
		let engine = Engine::new(Config::default());
		drain(&engine);
		engine.login_start(r#"{"kind":"selftest"}"#).unwrap();
		let mut seen = Vec::new();
		for _ in 0..200 {
			seen.extend(drain(&engine));
			if seen.iter().any(|event| event.contains("self-test finished")) {
				break;
			}
			thread::sleep(Duration::from_millis(5));
		}
		assert!(seen.iter().any(|event| event.contains(r#""state":"authenticated""#)), "{seen:?}");
		// Everything that claims a session is marked as the self-test's, so a
		// screenshot of it cannot be read as a working account.
		assert!(
			seen.iter().filter(|event| event.contains("authenticated")).all(|event| event.contains(r#""offline":true"#)),
			"{seen:?}"
		);
	}

	#[test]
	fn sign_out_is_immediate_and_closes_the_session_state() {
		let engine = Engine::new(Config::default());
		drain(&engine);
		engine.login_start(r#"{"kind":"signout"}"#).unwrap();
		assert_eq!(engine.state(), State::SignedOut);
		let Poll::Event(event) = engine.poll(1024) else { panic!("a signed_out event") };
		assert!(event.contains(r#""state":"signed_out""#), "{event}");
	}

	#[test]
	fn shutdown_is_once_and_nothing_starts_again() {
		let engine = Engine::new(Config::default());
		drain(&engine);
		assert!(engine.shutdown());
		assert_eq!(engine.state(), State::Closed);
		assert_eq!(engine.login_start(r#"{"kind":"signout"}"#), Err(LoginError::Closed));
		assert!(!engine.shutdown(), "a second shutdown reports that it did nothing");
	}

	#[test]
	fn an_event_that_does_not_fit_is_reported_and_dropped() {
		let engine = Engine::new(Config::default());
		assert_eq!(engine.poll(16), Poll::TooLong);
		// Dropped, not left at the head of the queue: the next poll is the
		// event behind it.
		assert!(matches!(engine.poll(1024), Poll::Event(event) if event.contains(r#""event":"log""#)));
	}

	#[test]
	fn an_event_is_one_line_with_the_abi_in_it() {
		let event = Event::new("login").text("state", "ready\nsecond line").number("abi", ABI as u64).finish();
		assert!(!event.contains('\n'), "{event}");
		assert!(event.contains(r#""abi":1"#), "{event}");
		assert_eq!(json::parse(&event).unwrap().get("state").and_then(json::Value::as_str), Some("ready\nsecond line"));
	}
}
