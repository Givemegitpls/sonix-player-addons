//! The C surface: five functions and a line of JSON, for `addons/spotify/addon.c`.
//!
//! The addon opens this library with `dlopen()` from the card and takes these
//! by name (`addons/spotify/spotify_engine.h` is the contract, and it is the
//! one file both halves read). Everything else in the crate is private to it.
//!
//! Threads: `init`, `login_start`, `poll` and `shutdown` are called from the
//! addon's LVGL timer and its row callbacks -- the UI thread. The engine does
//! its own work on threads of its own and reports through a queue, because
//! `poll` must return in microseconds: the player's LVGL has no OS layer and a
//! blocking call there is a frozen panel.
//!
//! Nothing here unwinds: `panic = "abort"` in the release profile, and the C
//! side never sees a Rust panic.

mod json;
mod queue;
mod session;

use std::ffi::{c_char, CStr};
use std::sync::{Mutex, MutexGuard};

use session::{Config, ConfigError, Engine, LoginError, Poll};

/// The engine, once `spotify_engine_init` has made one. A `Mutex<Option<_>>`
/// rather than a `static mut`: two calls racing is a lock, not undefined
/// behaviour, and the addon's timer can poll while a row callback starts a
/// login.
static ENGINE: Mutex<Option<Engine>> = Mutex::new(None);

fn engine() -> MutexGuard<'static, Option<Engine>> {
	ENGINE.lock().unwrap_or_else(|poison| poison.into_inner())
}

/// Return codes, mirrored in spotify_engine.h.
const OK: i32 = 0;
const ERR_ARGUMENT: i32 = -2;
const ERR_STATE: i32 = -3;
const ERR_TOO_LONG: i32 = -4;

/// Reads a C string, or None for a null pointer. The text is copied at once:
/// the caller's buffer is not kept past the call.
fn text<'a>(pointer: *const c_char) -> Option<&'a str> {
	if pointer.is_null() {
		return None;
	}
	// Safety: the addon passes a NUL-terminated string that outlives the call,
	// which is what the header asks of it.
	unsafe { CStr::from_ptr(pointer) }.to_str().ok()
}

/// The ABI this library speaks. The addon checks it before calling anything
/// else, so an engine from another build is refused with a sentence.
///
/// # Safety
/// No arguments; callable at any time.
#[no_mangle]
pub extern "C" fn spotify_engine_abi() -> i32 {
	session::ABI
}

/// Starts the engine with the addon's configuration:
///
/// ```json
/// {"session_dir": "<card>/Addons/spotify", "device_name": "Sonix", "bitrate": 160}
/// ```
///
/// Idempotent: a second call with the engine already up returns 0 and changes
/// nothing, which is what an addon whose `init()` runs once and whose timer may
/// re-arm wants.
///
/// # Safety
/// `config_json` must be a NUL-terminated UTF-8 string, or NULL.
#[no_mangle]
pub unsafe extern "C" fn spotify_engine_init(config_json: *const c_char) -> i32 {
	let Some(config_json) = text(config_json) else {
		return ERR_ARGUMENT;
	};
	let mut slot = engine();
	if slot.is_some() {
		return OK;
	}
	match Config::parse(config_json) {
		Ok(config) => {
			*slot = Some(Engine::new(config));
			OK
		}
		Err(ConfigError::Malformed) | Err(ConfigError::Field) => ERR_ARGUMENT,
	}
}

/// One login request:
///
/// ```json
/// {"kind": "token", "token": "..."}
/// {"kind": "signout"}
/// {"kind": "selftest"}
/// ```
///
/// Returns at once; the attempt reports through `spotify_engine_poll`. The
/// token is never logged and never written anywhere: the addon owns it, on the
/// card, in its own `session.ini`.
///
/// # Safety
/// `request_json` must be a NUL-terminated UTF-8 string, or NULL.
#[no_mangle]
pub unsafe extern "C" fn spotify_engine_login_start(request_json: *const c_char) -> i32 {
	let Some(request_json) = text(request_json) else {
		return ERR_ARGUMENT;
	};
	let slot = engine();
	let Some(engine) = slot.as_ref() else {
		return ERR_STATE;
	};
	match engine.login_start(request_json) {
		Ok(()) => OK,
		Err(LoginError::Closed) => ERR_STATE,
		Err(LoginError::Malformed) | Err(LoginError::BadKind) | Err(LoginError::BadToken) => ERR_ARGUMENT,
	}
}

/// Takes one event off the queue into `buffer`, without a newline, and returns
/// its length. 0 means nothing is waiting; a negative value is one of the
/// errors above, and ERR_TOO_LONG says the buffer is too small for one event.
///
/// # Safety
/// `buffer` must point at `capacity` writable bytes, or be NULL with a zero
/// capacity.
#[no_mangle]
pub unsafe extern "C" fn spotify_engine_poll(buffer: *mut c_char, capacity: u32) -> i32 {
	if buffer.is_null() || capacity == 0 {
		return ERR_ARGUMENT;
	}
	let capacity = capacity as usize;
	let slot = engine();
	let Some(engine) = slot.as_ref() else {
		return ERR_STATE;
	};
	match engine.poll(capacity) {
		Poll::Empty => 0,
		Poll::TooLong => ERR_TOO_LONG,
		Poll::Event(event) => {
			// Safety: capacity bytes, and poll() only hands over an event that
			// is shorter than that.
			std::ptr::copy_nonoverlapping(event.as_ptr(), buffer as *mut u8, event.len());
			*buffer.add(event.len()) = 0;
			event.len() as i32
		}
	}
}

/// Stops the worker, joins it and puts the engine in `closed`. Returns 1 when
/// there was something to stop and 0 when the engine was already closed or
/// never started; the addon logs whichever it got and calls this from no
/// thread but the UI one.
///
/// # Safety
/// No arguments; safe to call more than once.
#[no_mangle]
pub unsafe extern "C" fn spotify_engine_shutdown() -> i32 {
	let slot = engine();
	match slot.as_ref() {
		None => 0,
		Some(engine) => i32::from(engine.shutdown()),
	}
}

#[cfg(test)]
mod tests {
	use super::*;
	use std::ffi::CString;

	fn init_default() {
		let config = CString::new(r#"{"session_dir":"/tmp","device_name":"test"}"#).unwrap();
		assert_eq!(unsafe { spotify_engine_init(config.as_ptr()) }, OK);
	}

	fn drain() -> Vec<String> {
		let mut seen = Vec::new();
		let mut buffer = vec![0u8; 1024];
		loop {
			let count = unsafe { spotify_engine_poll(buffer.as_mut_ptr() as *mut c_char, buffer.len() as u32) };
			if count <= 0 {
				return seen;
			}
			let text = std::str::from_utf8(&buffer[..count as usize]).unwrap().to_string();
			seen.push(text);
		}
	}

	// The tests share one process-wide engine, so they run one at a time and
	// each leaves it closed.
	fn with_engine(test: impl FnOnce()) {
		static LOCK: Mutex<()> = Mutex::new(());
		let _guard = LOCK.lock().unwrap_or_else(|poison| poison.into_inner());
		unsafe { spotify_engine_shutdown() };
		*engine() = None;
		test();
		unsafe { spotify_engine_shutdown() };
		*engine() = None;
	}

	#[test]
	fn the_abi_is_what_the_header_says() {
		assert_eq!(spotify_engine_abi(), session::ABI);
	}

	#[test]
	fn null_arguments_are_refused_rather_than_dereferenced() {
		with_engine(|| {
			assert_eq!(unsafe { spotify_engine_init(std::ptr::null()) }, ERR_ARGUMENT);
			assert_eq!(unsafe { spotify_engine_poll(std::ptr::null_mut(), 0) }, ERR_ARGUMENT);
			assert_eq!(unsafe { spotify_engine_login_start(std::ptr::null()) }, ERR_ARGUMENT);
		});
	}

	#[test]
	fn a_bad_config_leaves_no_engine_behind() {
		with_engine(|| {
			let config = CString::new("{\"bitrate\":128}").unwrap();
			assert_eq!(unsafe { spotify_engine_init(config.as_ptr()) }, ERR_ARGUMENT);
			let mut buffer = vec![0u8; 64];
			assert_eq!(unsafe { spotify_engine_poll(buffer.as_mut_ptr() as *mut c_char, 64) }, ERR_STATE);
		});
	}

	#[test]
	fn init_polls_out_its_events_and_is_idempotent() {
		with_engine(|| {
			init_default();
			// The second call must not re-initialise: one init event, not two.
			let config = CString::new("{}").unwrap();
			assert_eq!(unsafe { spotify_engine_init(config.as_ptr()) }, OK);
			let events = drain();
			assert_eq!(events.iter().filter(|event| event.contains("\"event\":\"init\"")).count(), 1, "{events:?}");
		});
	}

	#[test]
	fn a_login_request_without_an_engine_is_a_state_error() {
		with_engine(|| {
			let request = CString::new(r#"{"kind":"signout"}"#).unwrap();
			assert_eq!(unsafe { spotify_engine_login_start(request.as_ptr()) }, ERR_STATE);
		});
	}

	#[test]
	fn the_self_test_travels_through_the_c_surface() {
		with_engine(|| {
			init_default();
			let request = CString::new(r#"{"kind":"selftest"}"#).unwrap();
			assert_eq!(unsafe { spotify_engine_login_start(request.as_ptr()) }, OK);
			let mut seen = Vec::new();
			for _ in 0..200 {
				seen.extend(drain());
				if seen.iter().any(|event| event.contains("self-test finished")) {
					break;
				}
				std::thread::sleep(std::time::Duration::from_millis(5));
			}
			assert!(seen.iter().any(|event| event.contains("\"state\":\"authenticated\"")), "{seen:?}");
		});
	}

	#[test]
	fn a_buffer_too_small_is_reported_and_nothing_is_written_past_it() {
		with_engine(|| {
			init_default();
			let mut small = [b'#' as c_char; 8];
			let count = unsafe { spotify_engine_poll(small.as_mut_ptr(), 8) };
			assert_eq!(count, ERR_TOO_LONG);
			assert!(small.iter().all(|byte| *byte == b'#' as c_char), "the buffer was touched");
		});
	}

	#[test]
	fn shutdown_says_whether_it_did_anything() {
		with_engine(|| {
			assert_eq!(unsafe { spotify_engine_shutdown() }, 0, "no engine yet");
			init_default();
			assert_eq!(unsafe { spotify_engine_shutdown() }, 1);
			assert_eq!(unsafe { spotify_engine_shutdown() }, 0, "already closed");
		});
	}
}
