//! The account: a state machine, the credentials file, the librespot session,
//! and the cache that makes a second start not need the password again.
//!
//! Four states, and nothing else is a state:
//!
//! ```text
//! signed_out ──login──> authenticating ──ok──> signed_in
//!      ^                      │                  │
//!      │                      └──error──> failed │
//!      └────────logout───────────────────────────┘
//! ```
//!
//! `failed` carries one short line of why, with anything that came out of the
//! credentials file scrubbed out of it: a reason is shown to a user and written
//! to a log, and neither may carry a password. The username is not logged
//! either -- the log says `signed in (premium)` and never who.
//!
//! Credentials come from two places, in this order:
//!
//!   1. `<state-dir>/credentials.json`, which is librespot's own cache format,
//!      written by the first successful login. This is what a second start
//!      uses, and it is why the password is needed once and not every time.
//!   2. `<credentials>`, an INI file:
//!
//!          username = ...
//!          password = ...
//!          access_token = ...      # optional, and the only one that still
//!                                  # works with a stock Spotify account
//!
//! A token is tried instead of the pair when it is present: Spotify has
//! withdrawn the username/password flow for the clients librespot imitates, so
//! `Session::connect` with a password answers `LoginFailed` on a real account.
//! The pair is still accepted, and still tried, because that is the documented
//! interface and because a cached credential pair is what librespot stores
//! after either kind of login.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::SessionConfig;
use librespot_core::Session;
use serde_json::{json, Value};
use tokio::sync::Mutex;

use crate::{log, PROTOCOL};

/// How long one login attempt may take, from the access-point lookup to the
/// last packet. A device on a bad network gives up here rather than sitting in
/// `authenticating` until the process dies.
const LOGIN_TIMEOUT: Duration = Duration::from_secs(30);

/// The longest reason a `failed` state carries. Long enough to name the
/// failure, short enough for one line of JSON and one line of log.
const REASON_MAX: usize = 120;

#[derive(Clone, PartialEq, Eq)]
pub enum State {
	SignedOut,
	Authenticating,
	SignedIn,
	Failed(String),
}

impl State {
	fn name(&self) -> &'static str {
		match self {
			State::SignedOut => "signed_out",
			State::Authenticating => "authenticating",
			State::SignedIn => "signed_in",
			State::Failed(_) => "failed",
		}
	}
}

struct Inner {
	state: State,
	session: Option<Session>,
	/// `Some(true)` for a premium account, `Some(false)` for a free one,
	/// `None` when Spotify did not say. Only a premium account can stream at
	/// all, so the addon can tell "log in" from "this account cannot play".
	premium: Option<bool>,
}

impl Inner {
	fn signed_in(&self) -> bool {
		self.state == State::SignedIn && self.session.as_ref().is_some_and(|s| !s.is_invalid())
	}

	fn status(&self) -> Value {
		let mut status = json!({
			"ok": true,
			"protocol": PROTOCOL,
			"state": self.state.name(),
		});
		if let Some(premium) = self.premium {
			status["premium"] = json!(premium);
		}
		if let State::Failed(reason) = &self.state {
			status["error"] = json!(reason);
		}
		status
	}

	fn forget(&mut self) {
		if let Some(session) = self.session.take() {
			session.shutdown();
		}
		self.premium = None;
	}
}

pub struct Manager {
	/// The INI file, when one was named on the command line. Its contents are
	/// read on `login`, never at start-up.
	credentials: Option<PathBuf>,
	state_dir: PathBuf,
	/// Serialises login attempts. Held across the network wait, which is why it
	/// is not the lock that guards the state: `status` has to answer while a
	/// login is in flight.
	auth: Mutex<()>,
	inner: Mutex<Inner>,
	/// The strings that must never reach a log or a response: the username, the
	/// password and the token, as they were read from the file. A plain
	/// `std::sync::Mutex` because scrubbing happens inside synchronous code
	/// (error formatting) and must not need an await.
	secrets: std::sync::Mutex<Vec<String>>,
}

impl Manager {
	pub fn new(credentials: Option<PathBuf>, state_dir: PathBuf) -> Self {
		Self {
			credentials,
			state_dir,
			auth: Mutex::new(()),
			inner: Mutex::new(Inner {
				state: State::SignedOut,
				session: None,
				premium: None,
			}),
			secrets: std::sync::Mutex::new(Vec::new()),
		}
	}

	pub async fn status(&self) -> Value {
		self.inner.lock().await.status()
	}

	/// The session for a catalog or stream op, or the protocol's
	/// `not_signed_in`. A session that Spotify has invalidated is not returned:
	/// librespot cannot reuse one, so the answer is the same as no session.
	pub async fn session(&self) -> Result<Session, &'static str> {
		let inner = self.inner.lock().await;
		match inner.session.as_ref() {
			Some(session) if !session.is_invalid() => Ok(session.clone()),
			_ => Err("not_signed_in"),
		}
	}

	pub async fn signed_in(&self) -> bool {
		self.inner.lock().await.signed_in()
	}

	/// Drops the session without touching the state or the cache: what a
	/// shutdown needs, and nothing more.
	pub async fn close(&self) {
		self.inner.lock().await.forget();
	}

	/// One login attempt, waited out. The answer is the state afterwards, so a
	/// caller that gets `authenticating` is talking to an engine that has no
	/// credentials file and no cache -- which is a `failed`, not a wait.
	pub async fn login(&self) -> Value {
		let _attempt = self.auth.lock().await;

		{
			let mut inner = self.inner.lock().await;
			if inner.signed_in() {
				return inner.status();
			}
			inner.forget();
			inner.state = State::Authenticating;
		}
		log("authenticating");

		let outcome = self.connect().await;

		let mut inner = self.inner.lock().await;
		match outcome {
			Ok((session, premium)) => {
				inner.session = Some(session);
				inner.premium = premium;
				inner.state = State::SignedIn;
				log(match premium {
					Some(true) => "signed in (premium)",
					Some(false) => "signed in (free: streaming will be refused)",
					None => "signed in",
				});
			}
			Err(reason) => {
				log(format!("login failed: {reason}"));
				inner.state = State::Failed(reason);
			}
		}
		inner.status()
	}

	/// Drops the session and the cached credentials. Without removing the file
	/// the next `login` would sign straight back in, which is not what a user
	/// who asked to be signed out means.
	pub async fn logout(&self) -> Value {
		let _attempt = self.auth.lock().await;
		let mut inner = self.inner.lock().await;
		inner.forget();
		inner.state = State::SignedOut;
		self.secrets
			.lock()
			.unwrap_or_else(|error| error.into_inner())
			.clear();
		let _ = fs::remove_file(self.state_dir.join("credentials.json"));
		log("signed out");
		inner.status()
	}

	async fn connect(&self) -> Result<(Session, Option<bool>), String> {
		let cache = self.open_cache()?;
		let (credentials, cached) = match cache.credentials() {
			Some(credentials) => (credentials, true),
			None => {
				let path = self
					.credentials
					.clone()
					.ok_or_else(|| "no credentials file and nothing cached".to_string())?;
				let file = read_credentials(&path)?;
				self.remember_secrets(&file.secrets).await;
				(file.credentials, false)
			}
		};
		log(if cached {
			"using the cached credentials"
		} else {
			"using the credentials file"
		});

		// `store_credentials` is what writes credentials.json back: the second
		// start reads it instead of the file, and a credential pair is upgraded
		// to whatever librespot's login produced.
		let session = Session::new(
			SessionConfig {
				autoplay: Some(false),
				..Default::default()
			},
			Some(cache),
		);

		match tokio::time::timeout(LOGIN_TIMEOUT, session.connect(credentials, true)).await {
			Ok(Ok(())) => {}
			Ok(Err(error)) => {
				session.shutdown();
				return Err(self.scrub(&one_line(&error.to_string())));
			}
			Err(_) => {
				session.shutdown();
				return Err("timeout".to_string());
			}
		}

		// "premium", "free" or "open"; a free account cannot stream, and the
		// addon wants to say so before the user picks a track.
		let premium = match session.get_user_attribute("type").as_deref() {
			Some("premium") => Some(true),
			Some(_) => Some(false),
			None => None,
		};
		Ok((session, premium))
	}

	fn open_cache(&self) -> Result<Cache, String> {
		fs::create_dir_all(&self.state_dir)
			.map_err(|error| format!("state dir: {}", one_line(&error.to_string())))?;
		{
			use std::os::unix::fs::PermissionsExt;
			let _ = fs::set_permissions(&self.state_dir, fs::Permissions::from_mode(0o700));
		}
		Cache::new(
			Some(&self.state_dir),
			None::<&PathBuf>,
			None::<&PathBuf>,
			None,
		)
		.map_err(|error| format!("cache: {}", one_line(&error.to_string())))
	}

	async fn remember_secrets(&self, secrets: &[String]) {
		let mut known = self.secrets.lock().unwrap_or_else(|error| error.into_inner());
		for secret in secrets {
			if secret.len() >= 3 && !known.contains(secret) {
				known.push(secret.clone());
			}
		}
	}

	/// A reason is shown on a screen and written to a log; neither may contain
	/// a username, a password or a token. librespot's error text is not known to
	/// be free of them, so every known secret is struck out of it here.
	fn scrub(&self, text: &str) -> String {
		let known = self.secrets.lock().unwrap_or_else(|error| error.into_inner());
		let mut out = text.to_string();
		for secret in known.iter() {
			if !secret.is_empty() {
				out = out.replace(secret.as_str(), "***");
			}
		}
		out
	}
}

struct CredentialsFile {
	credentials: Credentials,
	secrets: Vec<String>,
}

/// The INI file: `key = value`, `#` and `;` start a comment, blanks around the
/// value do not count, and a value may be quoted. Anything else is ignored
/// rather than fatal, so a file with a comment line in a language the engine
/// does not know still works.
fn read_credentials(path: &Path) -> Result<CredentialsFile, String> {
	let text = fs::read_to_string(path).map_err(|error| {
		format!(
			"credentials {}: {}",
			path.display(),
			one_line(&error.to_string())
		)
	})?;

	let mut username = String::new();
	let mut password = String::new();
	let mut token = String::new();
	for line in text.lines() {
		let line = line.trim();
		if line.is_empty() || line.starts_with('#') || line.starts_with(';') || line.starts_with('[') {
			continue;
		}
		let Some((key, value)) = line.split_once('=') else {
			continue;
		};
		let value = unquote(value.trim());
		match key.trim().to_ascii_lowercase().as_str() {
			"username" | "user" => username = value,
			"password" | "pass" => password = value,
			"access_token" | "token" => token = value,
			_ => {}
		}
	}

	if !token.is_empty() {
		return Ok(CredentialsFile {
			credentials: Credentials::with_access_token(token.clone()),
			secrets: vec![token],
		});
	}
	if username.is_empty() || password.is_empty() {
		return Err("credentials file has no username and password".to_string());
	}
	Ok(CredentialsFile {
		credentials: Credentials::with_password(username.clone(), password.clone()),
		secrets: vec![username, password],
	})
}

fn unquote(value: &str) -> String {
	let bytes = value.as_bytes();
	if bytes.len() >= 2 {
		let first = bytes[0];
		let last = bytes[bytes.len() - 1];
		if (first == b'"' && last == b'"') || (first == b'\'' && last == b'\'') {
			return value[1..value.len() - 1].to_string();
		}
	}
	value.to_string()
}

/// One line, control characters gone, cut at `REASON_MAX`. Error text from a
/// transport can carry a newline and a full HTTP request in it.
fn one_line(text: &str) -> String {
	let mut out: String = text
		.chars()
		.map(|c| if c.is_control() { ' ' } else { c })
		.collect();
	while out.contains("  ") {
		out = out.replace("  ", " ");
	}
	let out = out.trim().to_string();
	if out.chars().count() > REASON_MAX {
		let mut cut: String = out.chars().take(REASON_MAX).collect();
		cut.push('…');
		cut
	} else {
		out
	}
}

#[cfg(test)]
mod tests {
	use super::*;

	fn write(name: &str, text: &str) -> PathBuf {
		let path = std::env::temp_dir().join(format!("engine-mips-{}-{name}", std::process::id()));
		fs::write(&path, text).unwrap();
		path
	}

	#[test]
	fn ini_takes_a_pair_with_comments_quotes_and_blanks() {
		let path = write(
			"pair.ini",
			"# spotify account\n; another comment\n\nusername = listener\npassword = \"hunter 2\"\n",
		);
		let file = read_credentials(&path).unwrap();
		assert_eq!(file.credentials.username.as_deref(), Some("listener"));
		assert_eq!(file.credentials.auth_data, b"hunter 2".to_vec());
		assert_eq!(file.secrets, vec!["listener", "hunter 2"]);
		fs::remove_file(path).unwrap();
	}

	#[test]
	fn a_token_wins_over_a_pair_and_is_the_only_secret() {
		let path = write(
			"token.ini",
			"username = listener\npassword = hunter\naccess_token = BQ-abc\n",
		);
		let file = read_credentials(&path).unwrap();
		assert_eq!(file.credentials.auth_data, b"BQ-abc".to_vec());
		assert_eq!(file.secrets, vec!["BQ-abc"]);
		fs::remove_file(path).unwrap();
	}

	#[test]
	fn a_file_without_a_pair_is_an_error_and_not_a_login() {
		let path = write("empty.ini", "# nothing here\n");
		assert!(read_credentials(&path).is_err());
		fs::remove_file(path).unwrap();
		assert!(read_credentials(Path::new("/nonexistent/spotify.ini")).is_err());
	}

	#[test]
	fn reasons_are_one_short_line() {
		assert_eq!(one_line("a\nb\tc"), "a b c");
		let long = "x".repeat(500);
		assert_eq!(one_line(&long).chars().count(), REASON_MAX + 1);
	}
}
