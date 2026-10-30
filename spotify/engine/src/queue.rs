//! The events the engine has to tell the addon about.
//!
//! A queue rather than a callback, because of where the two halves run: the
//! engine's work happens on its own thread, the addon's LVGL timer runs on the
//! UI thread, and a C callback from the former into the latter would be an
//! `lv_*` call from the wrong thread. The addon drains this with
//! `spotify_engine_poll()` from its timer, which is the same shape the rest of
//! the player uses for work that finishes off the UI thread (`gui_post`, a
//! generation counter, and no locks held while drawing).
//!
//! Bounded, and the *oldest* event is the one dropped: a UI that fell behind
//! wants the current state, not the history that led to it.

use std::collections::VecDeque;
use std::sync::Mutex;

/// Enough for a burst of a state change, a log line and a page of results,
/// several times over. The addon polls every 250 ms.
pub const CAPACITY: usize = 64;

pub struct Queue {
	events: Mutex<Inner>,
}

struct Inner {
	events: VecDeque<String>,
	dropped: u64,
}

impl Queue {
	pub fn new() -> Self {
		Self { events: Mutex::new(Inner { events: VecDeque::with_capacity(CAPACITY), dropped: 0 }) }
	}

	/// Appends one event line. Never blocks for longer than a push.
	pub fn push(&self, line: String) {
		let mut inner = self.lock();
		while inner.events.len() >= CAPACITY {
			inner.events.pop_front();
			inner.dropped += 1;
		}
		inner.events.push_back(line);
	}

	/// The oldest event, or None when there is nothing to say.
	pub fn pop(&self) -> Option<String> {
		self.lock().events.pop_front()
	}

	/// How many events have been dropped since the engine started. Reported
	/// once, on the next event after a drop, so a UI that fell far behind is
	/// visible in the log rather than silently thinned out.
	pub fn take_dropped(&self) -> u64 {
		let mut inner = self.lock();
		std::mem::take(&mut inner.dropped)
	}

	#[cfg(test)]
	pub fn len(&self) -> usize {
		self.lock().events.len()
	}

	/// A poisoned lock means a worker panicked while holding it. The queue is
	/// a VecDeque of Strings and cannot be left half-updated by that, so the
	/// inner value is taken as it stands rather than the engine going silent.
	fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
		self.events.lock().unwrap_or_else(|poison| poison.into_inner())
	}
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn events_come_out_in_the_order_they_went_in() {
		let queue = Queue::new();
		queue.push("one".into());
		queue.push("two".into());
		assert_eq!(queue.pop().as_deref(), Some("one"));
		assert_eq!(queue.pop().as_deref(), Some("two"));
		assert_eq!(queue.pop(), None);
	}

	#[test]
	fn a_full_queue_drops_the_oldest_and_says_how_many() {
		let queue = Queue::new();
		for index in 0..CAPACITY + 3 {
			queue.push(format!("{index}"));
		}
		assert_eq!(queue.len(), CAPACITY);
		assert_eq!(queue.take_dropped(), 3);
		assert_eq!(queue.pop().as_deref(), Some("3"));
		// Taken once, not on every call.
		assert_eq!(queue.take_dropped(), 0);
	}
}
