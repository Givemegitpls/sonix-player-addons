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
//!      written by the first successful login. This is what a later engine
//!      process uses, which is why an engine restart does not sign the user out
//!      (`connect` here, and the addon's resume after it).
//!   2. `<credentials>`, an INI file:
//!
//!          access_token = ...      # the only credential that works, and it may
//!                                  # also stand on a line of its own
//!
//! A username and a password are still *read*, only to answer that Spotify has
//! withdrawn that flow for the clients librespot imitates: going to the network
//! with a pair buys a "Bad credentials" that blames a password the user was
//! never asked for (read_credentials).

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use base64::Engine as _;
use ring::rand::SecureRandom;
use bytes::Bytes;
use http::{header, Request};
use librespot_core::http_client::HttpClient;
use sha2::{Digest, Sha256};

use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::SessionConfig;
use librespot_core::Session;
use serde_json::{json, Value};
use tokio::sync::Mutex;

use crate::{failure, log, PROTOCOL};

/// How long one login attempt may take, from the access-point lookup to the
/// last packet. A device on a bad network gives up here rather than sitting in
/// `authenticating` until the process dies.
const LOGIN_TIMEOUT: Duration = Duration::from_secs(30);

/// The longest reason a `failed` state carries. Long enough to name the
/// failure, short enough for one line of JSON and one line of log.
const REASON_MAX: usize = 120;

/// OAuth, as the one legitimate way in that is left: Spotify's Web API answers
/// 429 to the access tokens users copy out of the web player (their client ids
/// are first-party and have been quota-blocked since 2025-12-15), while a token
/// issued to an application the user registered themselves carries that
/// application's own quota. See docs/ADDONS.md, "Spotify".
///
/// The redirect is the loopback Spotify allows over plain http. It points at
/// whichever machine runs the browser, so the player never sees the redirect:
/// the code travels to the card in `oauth-code.txt` (the whole redirect URL is
/// accepted too) and is exchanged by `oauth_finish`.
const OAUTH_AUTHORIZE: &str = "https://accounts.spotify.com/authorize";
const OAUTH_TOKEN: &str = "https://accounts.spotify.com/api/token";
const OAUTH_REDIRECT: &str = "http://127.0.0.1:5588/login";
const OAUTH_SCOPES: &str = "streaming user-read-private user-read-email playlist-read-private \
playlist-read-collaborative user-library-read";

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

/// What the OAuth exchange left behind. `refresh` is the part that makes this
/// worth doing: an access token lives an hour, a refresh token keeps minting new
/// ones, so the card is signed in until the user revokes the application.
#[derive(Clone, Default)]
struct OAuthTokens {
	access: String,
	refresh: String,
	expires_at: u64,
}

fn now() -> u64 {
	SystemTime::now()
		.duration_since(UNIX_EPOCH)
		.map(|since| since.as_secs())
		.unwrap_or(0)
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
	/// The access token exactly as the user pasted it, for Spotify's Web API.
	///
	/// Not `session.auth_data()`: after an access-point login librespot replaces
	/// that with the *reusable credential* the server handed back, which is not a
	/// bearer token -- sending it to `api.spotify.com` answers 401 ("token
	/// refused") while the same string works fine as an AP credential. Read from
	/// the file, on every login, cache or not: the file is what the user edits.
	catalog_token: std::sync::Mutex<String>,
	/// The application the user registered, from `client_id` in the same file.
	client_id: std::sync::Mutex<String>,
	/// The egress this card goes through for Spotify, from `proxy` in the same
	/// file. Empty is the ordinary case: no proxy.
	proxy: std::sync::Mutex<String>,
	/// The OAuth tokens, when this card has been authorized. They take
	/// precedence over `catalog_token` and over the access-point cache: an
	/// authorization names the account, and the file's pasted token may well be
	/// somebody else's.
	oauth: std::sync::Mutex<OAuthTokens>,
	/// The PKCE verifier and state of the flow in progress: `oauth_start` makes
	/// them, `oauth_finish` needs them, and they are useless after either.
	verifier: std::sync::Mutex<String>,
	pending_state: std::sync::Mutex<String>,
}

impl Manager {
	pub fn new(credentials: Option<PathBuf>, state_dir: PathBuf) -> Self {
		let manager = Self {
			credentials,
			state_dir,
			auth: Mutex::new(()),
			inner: Mutex::new(Inner {
				state: State::SignedOut,
				session: None,
				premium: None,
			}),
			secrets: std::sync::Mutex::new(Vec::new()),
			catalog_token: std::sync::Mutex::new(String::new()),
			client_id: std::sync::Mutex::new(String::new()),
			proxy: std::sync::Mutex::new(String::new()),
			oauth: std::sync::Mutex::new(OAuthTokens::default()),
			verifier: std::sync::Mutex::new(String::new()),
			pending_state: std::sync::Mutex::new(String::new()),
		};
		manager.load_oauth();
		// The client id is what `oauth_start` needs, and a user may tap that
		// before anything has signed in: read it now, not at login.
		if let Some(path) = manager.credentials.clone() {
			if let Ok(file) = read_credentials(&path) {
				manager.remember_client_id(&file.client_id);
				manager.remember_proxy(&file.proxy);
			}
		}
		manager
	}

	/// The bearer token for the Web API, as the user pasted it. Empty when there
	/// is none, and then every catalog op answers `token_expired`.
	pub fn catalog_token(&self) -> String {
		self.catalog_token
			.lock()
			.unwrap_or_else(|error| error.into_inner())
			.clone()
	}

	/// The folder the addon lives in: `credentials.ini` and the two OAuth files
	/// sit there, `state/` below it.
	fn addon_dir(&self) -> Option<PathBuf> {
		self.credentials
			.as_ref()
			.and_then(|path| path.parent().map(Path::to_path_buf))
	}

	fn oauth_file(&self) -> PathBuf {
		self.state_dir.join("oauth.json")
	}

	/// The proxy from the file, or "" -- the card's own egress setting.
	pub fn proxy(&self) -> Option<String> {
		let proxy = self
			.proxy
			.lock()
			.unwrap_or_else(|error| error.into_inner())
			.clone();
		if proxy.is_empty() {
			None
		} else {
			Some(proxy)
		}
	}

	fn client_id(&self) -> String {
		self.client_id
			.lock()
			.unwrap_or_else(|error| error.into_inner())
			.clone()
	}

	/// Reads `state/oauth.json`, if a previous run left one. Called from `new`,
	/// which is not async and must not fail: a card without the file is a card
	/// that has not been authorized yet.
	fn load_oauth(&self) {
		let Ok(text) = fs::read_to_string(self.oauth_file()) else {
			return;
		};
		let Ok(doc) = serde_json::from_str::<Value>(&text) else {
			return;
		};
		let mut tokens = OAuthTokens {
			access: doc.get("access").and_then(Value::as_str).unwrap_or("").to_string(),
			refresh: doc.get("refresh").and_then(Value::as_str).unwrap_or("").to_string(),
			expires_at: doc.get("expires_at").and_then(Value::as_u64).unwrap_or(0),
		};
		// An access token that outlived its hour is worthless but the refresh
		// token is not: drop the first, keep the second.
		if tokens.expires_at != 0 && tokens.expires_at <= now() {
			tokens.access.clear();
		}
		if tokens.refresh.is_empty() && tokens.access.is_empty() {
			return;
		}
		*self.oauth.lock().unwrap_or_else(|error| error.into_inner()) = tokens;
	}

	/// Writes the tokens back to the card (0600) and takes them into memory.
	///
	/// It also deletes `state/credentials.json`: that file is librespot's cache
	/// of whichever account signed in last, and the account an authorization
	/// names is the account this card is for -- a stale cache would sign back in
	/// as the previous one.
	async fn store_oauth(&self, tokens: OAuthTokens) {
		{
			let mut held = self.oauth.lock().unwrap_or_else(|error| error.into_inner());
			*held = tokens.clone();
		}
		let body = serde_json::json!({
			"access": tokens.access,
			"refresh": tokens.refresh,
			"expires_at": tokens.expires_at,
		});
		if fs::create_dir_all(&self.state_dir).is_ok() {
			let path = self.oauth_file();
			if fs::write(&path, body.to_string()).is_ok() {
				use std::os::unix::fs::PermissionsExt;
				let _ = fs::set_permissions(&path, fs::Permissions::from_mode(0o600));
			}
		}
		let _ = fs::remove_file(self.state_dir.join("credentials.json"));
	}

	/// Reads `client_id` out of `credentials.ini` again: the file is edited by
	/// hand and the engine can be running while it happens.
	fn reload_client_id(&self) {
		if let Some(path) = self.credentials.clone() {
			if let Ok(file) = read_credentials(&path) {
				self.remember_client_id(&file.client_id);
				self.remember_proxy(&file.proxy);
			}
		}
	}

	fn remember_client_id(&self, client_id: &str) {
		if !client_id.is_empty() {
			*self.client_id.lock().unwrap_or_else(|error| error.into_inner()) = client_id.to_string();
		}
	}

	fn remember_proxy(&self, proxy: &str) {
		*self.proxy.lock().unwrap_or_else(|error| error.into_inner()) = proxy.to_string();
	}

	fn remember_pasted_token(&self, token: Option<String>) {
		if let Some(token) = token {
			*self
				.catalog_token
				.lock()
				.unwrap_or_else(|error| error.into_inner()) = token;
		}
	}

	/// The bearer to use for the Web API and for the access-point login.
	///
	/// The order is the account's: an authorization (which names an account the
	/// user chose) first, then the pasted token (a legacy path, and the account
	/// it belongs to is whatever the web player was signed in as). A stored
	/// access token is refreshed a minute before it expires; a refresh that the
	/// service refuses leaves nothing and the caller falls back.
	pub async fn access_token(&self) -> Option<String> {
		let (access, refresh, expires_at) = {
			let tokens = self.oauth.lock().unwrap_or_else(|error| error.into_inner());
			(tokens.access.clone(), tokens.refresh.clone(), tokens.expires_at)
		};
		let authorized = !access.is_empty() || !refresh.is_empty();
		if !access.is_empty() && (expires_at == 0 || now() + 60 < expires_at) {
			return Some(access);
		}
		if !refresh.is_empty() {
			match self.refresh_access(&refresh).await {
				Ok(tokens) => {
					let fresh = tokens.access.clone();
					self.store_oauth(tokens).await;
					log("oauth: access token refreshed");
					return Some(fresh);
				}
				Err(error) => {
					log(format!("oauth: refresh failed: {error}"));
					// The refresh token is dead (revoked, or the application was
					// deleted). The card is signed out of the authorization; the
					// page says to sign in again rather than lying about it.
					*self.oauth.lock().unwrap_or_else(|error| error.into_inner()) = OAuthTokens::default();
					let _ = fs::remove_file(self.oauth_file());
				}
			}
		}
		if authorized {
			// Nothing left of the authorization: do not quietly fall back to a
			// pasted token that belongs to another account.
			return None;
		}
		let pasted = self.catalog_token();
		if pasted.is_empty() {
			None
		} else {
			Some(pasted)
		}
	}

	/// True when this card has been authorized: the state the page shows.
	pub fn authorized(&self) -> bool {
		let tokens = self.oauth.lock().unwrap_or_else(|error| error.into_inner());
		!tokens.refresh.is_empty() || !tokens.access.is_empty()
	}

	/// Step one: the URL the user opens. Writes it to `oauth-url.txt` beside
	/// `credentials.ini`, because a URL that long cannot be shown on a 320-pixel
	/// page and cannot be typed into it either.
	pub async fn oauth_start(&self) -> Value {
		// The card may have been edited since this engine started (the client id
		// is typed into the file by hand): read it again here.
		self.reload_client_id();
		let client_id = self.client_id();
		if client_id.is_empty() {
			return failure("no_client_id");
		}
		let verifier = random_text(64);
		let challenge = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()));
		let state = random_text(16);
		*self.verifier.lock().unwrap_or_else(|error| error.into_inner()) = verifier;
		*self.pending_state.lock().unwrap_or_else(|error| error.into_inner()) = state.clone();

		let url = format!(
			"{OAUTH_AUTHORIZE}?client_id={}&response_type=code&redirect_uri={}&scope={}&code_challenge_method=S256&code_challenge={}&state={}",
			encode(&client_id),
			encode(OAUTH_REDIRECT),
			encode(OAUTH_SCOPES),
			encode(&challenge),
			encode(&state)
		);
		if let Some(dir) = self.addon_dir() {
			let _ = fs::write(dir.join("oauth-url.txt"), &url);
			log(format!("oauth: authorization url in {}", dir.join("oauth-url.txt").display()));
		}
		json!({"ok": true, "protocol": PROTOCOL, "state": "waiting_for_authorization"})
	}

	/// Step two: the code the browser was redirected with, as left in
	/// `oauth-code.txt` -- either the code alone or the whole redirect URL.
	pub async fn oauth_finish(&self) -> Value {
		let Some(dir) = self.addon_dir() else {
			return failure("no_card");
		};
		let path = dir.join("oauth-code.txt");
		let Ok(text) = fs::read_to_string(&path) else {
			return failure("no_code_file");
		};
		let Some(code) = code_from(&text) else {
			return failure("no_code_in_file");
		};
		let verifier = self
			.verifier
			.lock()
			.unwrap_or_else(|error| error.into_inner())
			.clone();
		if verifier.is_empty() {
			return failure("no_login_in_progress");
		}
		self.reload_client_id();
		let client_id = self.client_id();
		if client_id.is_empty() {
			return failure("no_client_id");
		}

		let form = format!(
			"grant_type=authorization_code&code={}&redirect_uri={}&client_id={}&code_verifier={}",
			encode(&code),
			encode(OAUTH_REDIRECT),
			encode(&client_id),
			encode(&verifier)
		);
		match self.post_token(&form).await {
			Ok(tokens) => {
				let _ = fs::remove_file(&path);
				*self.verifier.lock().unwrap_or_else(|error| error.into_inner()) = String::new();
				self.store_oauth(tokens).await;
				log("oauth: authorized; the account is this card's now");
				self.login().await
			}
			Err(error) => {
				log(format!("oauth: code exchange failed: {error}"));
				failure("oauth_refused")
			}
		}
	}

	async fn refresh_access(&self, refresh: &str) -> Result<OAuthTokens, String> {
		let form = format!(
			"grant_type=refresh_token&refresh_token={}&client_id={}",
			encode(refresh),
			encode(&self.client_id())
		);
		// A refresh keeps the refresh token Spotify already gave us when the
		// answer does not carry a new one.
		let mut tokens = self.post_token(&form).await?;
		if tokens.refresh.is_empty() {
			tokens.refresh = refresh.to_string();
		}
		Ok(tokens)
	}

	/// The one POST this file makes: Spotify's token endpoint, form encoded.
	async fn post_token(&self, form: &str) -> Result<OAuthTokens, String> {
		let client = HttpClient::new(None);
		let request = Request::builder()
			.method("POST")
			.uri(OAUTH_TOKEN)
			.header(header::CONTENT_TYPE, "application/x-www-form-urlencoded")
			.header(header::ACCEPT, "application/json")
			.body(Bytes::from(form.to_string()))
			.map_err(|error| format!("request: {error}"))?;
		let body = client
			.request_body(request)
			.await
			.map_err(|error| one_line(&error.to_string()))?;
		let doc: Value = serde_json::from_slice(&body).map_err(|error| format!("answer: {error}"))?;
		if let Some(error) = doc.get("error").and_then(Value::as_str) {
			let description = doc.get("error_description").and_then(Value::as_str).unwrap_or("");
			return Err(one_line(&format!("{error} {description}")));
		}
		let access = doc.get("access_token").and_then(Value::as_str).unwrap_or("").to_string();
		if access.is_empty() {
			return Err("no access_token in the answer".to_string());
		}
		let refresh = doc.get("refresh_token").and_then(Value::as_str).unwrap_or("").to_string();
		let lifetime = doc.get("expires_in").and_then(Value::as_u64).unwrap_or(3600);
		Ok(OAuthTokens {
			access,
			refresh,
			expires_at: now() + lifetime,
		})
	}

	/// The account's state, and the account type with it.
	///
	/// `premium` is read here rather than only in `login`: the ProductInfo packet
	/// that carries the account type is handled by librespot's dispatch task
	/// *after* `connect()` returns, so a login almost always sees nothing and the
	/// page would say "Premium: unknown" for an account whose type is known a
	/// second later. The first status that follows fills it in, and the value is
	/// kept (`forget()` clears it with the session).
	pub async fn status(&self) -> Value {
		let mut inner = self.inner.lock().await;
		if inner.premium.is_none() {
			if let Some(kind) = inner
				.session
				.as_ref()
				.filter(|session| !session.is_invalid())
				.and_then(|session| session.get_user_attribute("type"))
			{
				inner.premium = match kind.as_str() {
					"premium" => Some(true),
					_ => Some(false),
				};
			}
		}
		let mut status = inner.status();
		// Two facts the page draws rows for: whether this card carries the
		// application id OAuth needs, and whether it has been authorized. Neither
		// is a secret; the tokens are.
		status["client_id"] = json!(!self.client_id().is_empty());
		status["authorized"] = json!(self.authorized());
		status
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
		// A sign-out ends the authorization as well: the refresh token on the
		// card would otherwise sign the same account straight back in.
		*self.oauth.lock().unwrap_or_else(|error| error.into_inner()) = OAuthTokens::default();
		let _ = fs::remove_file(self.oauth_file());
		log("signed out");
		inner.status()
	}

	async fn connect(&self) -> Result<(Session, Option<bool>), String> {
		let cache = self.open_cache()?;
		// An authorization names the account: it decides who signs in, and the
		// cache (whoever signed in last) and the pasted token (whoever the web
		// player was) have nothing to say about it.
		let authorized = self.authorized();
		let token = self.access_token().await;
		let (credentials, source) = if authorized {
			let token =
				token.ok_or_else(|| "the authorization has expired: sign in with Spotify again".to_string())?;
			(Credentials::with_access_token(token), "the authorization")
		} else if let Some(credentials) = cache.credentials() {
			// The Web API needs the pasted token even when the session came from
			// the cache: the cache holds the AP's reusable credential, which
			// `api.spotify.com` refuses (see `catalog_token`).
			if let Some(path) = self.credentials.clone() {
				if let Ok(file) = read_credentials(&path) {
					self.remember_pasted_token(file.token);
				}
			}
			(credentials, "the cached credentials")
		} else {
			let path = self
				.credentials
				.clone()
				.ok_or_else(|| "no credentials file and nothing cached".to_string())?;
			let file = read_credentials(&path)?;
			if file.token.is_none() && !file.client_id.is_empty() {
				return Err("this card signs in with Spotify: tap \"Sign in with Spotify\"".to_string());
			}
			self.remember_secrets(&file.secrets).await;
			self.remember_pasted_token(file.token);
			(file.credentials, "the credentials file")
		};
		log(format!("using {source}"));

		// `store_credentials` is what writes credentials.json back: the second
		// start reads it instead of the file, and a credential pair is upgraded
		// to whatever librespot's login produced.
		// The access point, the audio keys and the CDN are as region-bound as the
		// Web API: a track that answers `track_unavailable` on one network plays on
		// another. So the card's proxy, when it has one, is the session's proxy
		// too -- the same egress for everything Spotify.
		let proxy = self
			.proxy()
			.and_then(|text| url::Url::parse(&text).ok());
		if proxy.is_some() {
			log("using the proxy from credentials.ini for the session");
		}
		let session = Session::new(
			SessionConfig {
				autoplay: Some(false),
				proxy,
				..Default::default()
			},
			Some(cache),
		);

		match tokio::time::timeout(LOGIN_TIMEOUT, session.connect(credentials, true)).await {
			Ok(Ok(())) => {}
			Ok(Err(error)) => {
				session.shutdown();
				return Err(self.scrub(&login_failure(&error)));
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

#[derive(Debug)]
struct CredentialsFile {
	credentials: Credentials,
	secrets: Vec<String>,
	/// The access token, when the file gave one: what the Web API can use, and
	/// the legacy way in.
	token: Option<String>,
	/// The application the user registered, for OAuth. Not a secret: it is in
	/// every authorization URL the browser opens.
	client_id: String,
	/// An HTTP proxy for Spotify's Web API, for a network the service does not
	/// serve directly. Empty is no proxy.
	proxy: String,
}

/// The INI file: `key = value`, `#` and `;` start a comment, blanks around the
/// value do not count, and a value may be quoted. Anything else is ignored
/// rather than fatal, so a file with a comment line in a language the engine
/// does not know still works.
///
/// The one exception is a line with nothing to split: a bare token, which is
/// what a value copied out of a browser's inspector looks like. It is taken as
/// the token, because the alternative -- ignoring it -- turns a pasted token
/// into "there is no token here", which sends the user looking for the wrong
/// mistake.
///
/// The failures say what is missing rather than quoting librespot: a user
/// reading "Bad credentials" next to a token they never typed as a password
/// has no way to know which of the two files or which line to look at.
fn read_credentials(path: &Path) -> Result<CredentialsFile, String> {
	let text = fs::read_to_string(path).map_err(|error| {
		format!(
			"credentials {}: {}",
			path.display(),
			one_line(&error.to_string())
		)
	})?;

	let name = path
		.file_name()
		.map(|name| name.to_string_lossy().into_owned())
		.unwrap_or_else(|| path.display().to_string());

	let mut username = String::new();
	let mut password = String::new();
	let mut token = String::new();
	let mut bare = String::new();
	let mut client_id = String::new();
	let mut proxy = String::new();
	for line in text.lines() {
		let line = line.trim();
		if line.is_empty() || line.starts_with('#') || line.starts_with(';') || line.starts_with('[') {
			continue;
		}
		let Some((key, value)) = line.split_once('=') else {
			if bare.is_empty() {
				bare = unquote(line);
			}
			continue;
		};
		let value = unquote(value.trim());
		match key.trim().to_ascii_lowercase().as_str() {
			"username" | "user" => username = value,
			"password" | "pass" => password = value,
			"access_token" | "token" => token = value,
			"client_id" | "client" => client_id = value,
			"proxy" | "http_proxy" => proxy = value,
			_ => {}
		}
	}

	if token.is_empty() {
		token = bare;
	}
	if !token.is_empty() {
		return Ok(CredentialsFile {
			credentials: Credentials::with_access_token(token.clone()),
			secrets: vec![token.clone()],
			token: Some(token),
			client_id,
			proxy,
		});
	}
	if !username.is_empty() && !password.is_empty() {
		// The pair is read and no longer works for anyone: Spotify withdrew that
		// flow for the client librespot imitates, so going to the network with it
		// only buys a "Bad credentials" that blames the user's password.
		return Err(format!(
			"Spotify no longer accepts a username and password: put an access_token in {name}"
		));
	}
	if !client_id.is_empty() {
		// A card set up for OAuth: the client id is the whole of the file until
		// the authorization has been done, and then `state/oauth.json` carries
		// the account. Not an error -- this is what the file is meant to look
		// like before the first sign-in.
		return Ok(CredentialsFile {
			credentials: Credentials::with_access_token(String::new()),
			secrets: Vec::new(),
			token: None,
			client_id,
			proxy,
		});
	}
	Err(format!("no access_token in {name}"))
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

/// librespot's sentence for a token it will not take is
/// `Permission denied { Login failed with reason: Bad credentials }`, which
/// reads like a mistyped password to somebody who never typed one: the pair flow
/// is withdrawn, and the only credential here is a token. What the user can act
/// on is that the token is no good, so that is what is said; everything else
/// (a DNS failure, a timeout inside the transport) is passed on as it came.
fn login_failure(error: &librespot_core::Error) -> String {
	let text = one_line(&error.to_string());
	let lowered = text.to_ascii_lowercase();
	if lowered.contains("bad credentials") || lowered.contains("login failed") {
		return "the access_token was refused (it may have expired)".to_string();
	}
	text
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

/// The characters OAuth allows in a PKCE verifier, and nothing else: 64 of them
/// is 384 bits of entropy, and no escaping is needed anywhere they are used.
fn random_text(length: usize) -> String {
	const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
	let mut bytes = vec![0u8; length];
	if ring::rand::SystemRandom::new().fill(&mut bytes).is_err() {
		// A machine without a random source cannot do PKCE at all; falling back
		// to a fixed string would be worse than a short one.
		return "sonix".to_string();
	}
	bytes
		.iter()
		.map(|byte| ALPHABET[*byte as usize % ALPHABET.len()] as char)
		.collect()
}

/// Percent-encoding for a query or form value: everything outside the
/// unreserved set is escaped, so a redirect URI keeps its `:` and `/`.
fn encode(text: &str) -> String {
	text.bytes()
		.map(|byte| {
			if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
				(byte as char).to_string()
			} else {
				format!("%{byte:02X}")
			}
		})
		.collect()
}

/// The authorization code, out of either the code alone or the whole redirect
/// URL the browser failed to open: `http://127.0.0.1:5588/login?code=...&state=...`.
fn code_from(text: &str) -> Option<String> {
	let text = text.trim();
	if text.is_empty() {
		return None;
	}
	let code = match text.find("code=") {
		Some(at) => text[at + "code=".len()..].split('&').next().unwrap_or(""),
		None => text,
	};
	let code = code.trim();
	if code.is_empty() {
		None
	} else {
		Some(code.to_string())
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
		// The pair is no longer a way in, and saying so is the whole point of
		// reading it: no network call, no "Bad credentials" about a password
		// nobody typed.
		let error = read_credentials(&path).unwrap_err();
		assert!(error.contains("no longer accepts a username and password"), "{error}");
		assert!(error.contains("access_token"), "{error}");
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
	fn the_file_keeps_the_raw_token_for_the_web_api() {
		let path = write("wapi.ini", "access_token = BQ-abc\n");
		let file = read_credentials(&path).unwrap();
		assert_eq!(file.token.as_deref(), Some("BQ-abc"));
		fs::remove_file(path).unwrap();
	}

	#[test]
	fn a_token_on_a_line_of_its_own_is_a_token() {
		let path = write("bare.ini", "# paste it below\nBQ-abc-def-123\n");
		let file = read_credentials(&path).unwrap();
		assert_eq!(file.credentials.auth_data, b"BQ-abc-def-123".to_vec());
		assert_eq!(file.secrets, vec!["BQ-abc-def-123"]);
		fs::remove_file(path).unwrap();
	}

	#[test]
	fn a_file_without_a_token_says_so() {
		let path = write("empty.ini", "# nothing here\n");
		let error = read_credentials(&path).unwrap_err();
		assert!(error.starts_with("no access_token in "), "{error}");
		fs::remove_file(path).unwrap();
		assert!(read_credentials(Path::new("/nonexistent/spotify.ini")).is_err());
	}

	#[test]
	fn a_code_is_taken_out_of_a_redirect_url_or_a_bare_line() {
		assert_eq!(code_from("AQDxyz\n").as_deref(), Some("AQDxyz"));
		assert_eq!(
			code_from("http://127.0.0.1:5588/login?code=AQDxyz&state=abc").as_deref(),
			Some("AQDxyz")
		);
		assert_eq!(code_from("   "), None);
	}

	#[test]
	fn a_redirect_uri_survives_encoding() {
		assert_eq!(
			encode("http://127.0.0.1:5588/login"),
			"http%3A%2F%2F127.0.0.1%3A5588%2Flogin"
		);
	}

	#[test]
	fn reasons_are_one_short_line() {
		assert_eq!(one_line("a\nb\tc"), "a b c");
		let long = "x".repeat(500);
		assert_eq!(one_line(&long).chars().count(), REASON_MAX + 1);
	}
}
