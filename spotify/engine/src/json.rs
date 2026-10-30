//! The little JSON this engine needs, without a dependency.
//!
//! Two jobs, both small: read the `config_json` and the `login` request the
//! addon hands over, and write the one-line events it hands back. Neither
//! needs a serialiser: the events are flat objects of strings and numbers, and
//! the input is a handful of fields.
//!
//! The parser is a recursive descent one with a depth limit and a size limit,
//! because the text comes from outside the library. Anything it does not
//! understand is an error and the caller says so; it never guesses.

/// How deep an object or array may nest. The protocol is flat; this only has
/// to be small enough that a hostile string cannot exhaust the stack.
const MAX_DEPTH: usize = 8;

/// The longest input the engine accepts. A config or a login request that is
/// longer than this is not one of ours.
pub const MAX_INPUT: usize = 8192;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
	Null,
	Bool(bool),
	Number(f64),
	Str(String),
	Array(Vec<Value>),
	Object(Vec<(String, Value)>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParseError;

impl Value {
	/// The member of an object, or None for any other kind of value.
	pub fn get(&self, key: &str) -> Option<&Value> {
		match self {
			Value::Object(members) => members.iter().find(|(name, _)| name == key).map(|(_, value)| value),
			_ => None,
		}
	}

	pub fn as_str(&self) -> Option<&str> {
		match self {
			Value::Str(text) => Some(text),
			_ => None,
		}
	}

	pub fn as_f64(&self) -> Option<f64> {
		match self {
			Value::Number(number) => Some(*number),
			_ => None,
		}
	}

	/// A whole number in range, for the fields that are counts and bitrates.
	pub fn as_u64(&self) -> Option<u64> {
		let number = self.as_f64()?;
		if number.is_finite() && number >= 0.0 && number.fract() == 0.0 && number <= u64::MAX as f64 {
			Some(number as u64)
		} else {
			None
		}
	}
}

/// `"..."`, with the escapes JSON requires. Everything below 0x20 is escaped,
/// so an event line never carries a raw newline: the addon reads one event per
/// poll and prints it as one log line.
pub fn string(text: &str) -> String {
	let mut out = String::with_capacity(text.len() + 2);
	out.push('"');
	for character in text.chars() {
		match character {
			'"' => out.push_str("\\\""),
			'\\' => out.push_str("\\\\"),
			'\n' => out.push_str("\\n"),
			'\r' => out.push_str("\\r"),
			'\t' => out.push_str("\\t"),
			'\u{08}' => out.push_str("\\b"),
			'\u{0c}' => out.push_str("\\f"),
			c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
			c => out.push(c),
		}
	}
	out.push('"');
	out
}

pub fn parse(text: &str) -> Result<Value, ParseError> {
	if text.len() > MAX_INPUT {
		return Err(ParseError);
	}
	let mut parser = Parser { bytes: text.as_bytes(), at: 0, depth: 0 };
	parser.space();
	let value = parser.value()?;
	parser.space();
	if parser.at != parser.bytes.len() {
		return Err(ParseError);
	}
	Ok(value)
}

struct Parser<'a> {
	bytes: &'a [u8],
	at: usize,
	depth: usize,
}

impl<'a> Parser<'a> {
	fn peek(&self) -> Option<u8> {
		self.bytes.get(self.at).copied()
	}

	fn space(&mut self) {
		while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
			self.at += 1;
		}
	}

	fn expect(&mut self, byte: u8) -> Result<(), ParseError> {
		if self.peek() == Some(byte) {
			self.at += 1;
			Ok(())
		} else {
			Err(ParseError)
		}
	}

	fn literal(&mut self, text: &[u8]) -> Result<(), ParseError> {
		if self.bytes.len() >= self.at + text.len() && &self.bytes[self.at..self.at + text.len()] == text {
			self.at += text.len();
			Ok(())
		} else {
			Err(ParseError)
		}
	}

	fn value(&mut self) -> Result<Value, ParseError> {
		match self.peek().ok_or(ParseError)? {
			b'{' => self.object(),
			b'[' => self.array(),
			b'"' => self.string().map(Value::Str),
			b't' => {
				self.literal(b"true")?;
				Ok(Value::Bool(true))
			}
			b'f' => {
				self.literal(b"false")?;
				Ok(Value::Bool(false))
			}
			b'n' => {
				self.literal(b"null")?;
				Ok(Value::Null)
			}
			b'-' | b'0'..=b'9' => self.number(),
			_ => Err(ParseError),
		}
	}

	fn object(&mut self) -> Result<Value, ParseError> {
		self.enter()?;
		self.expect(b'{')?;
		let mut members = Vec::new();
		self.space();
		if self.peek() == Some(b'}') {
			self.at += 1;
			self.depth -= 1;
			return Ok(Value::Object(members));
		}
		loop {
			self.space();
			let name = self.string()?;
			self.space();
			self.expect(b':')?;
			self.space();
			let value = self.value()?;
			members.push((name, value));
			self.space();
			match self.peek() {
				Some(b',') => self.at += 1,
				Some(b'}') => {
					self.at += 1;
					break;
				}
				_ => return Err(ParseError),
			}
		}
		self.depth -= 1;
		Ok(Value::Object(members))
	}

	fn array(&mut self) -> Result<Value, ParseError> {
		self.enter()?;
		self.expect(b'[')?;
		let mut items = Vec::new();
		self.space();
		if self.peek() == Some(b']') {
			self.at += 1;
			self.depth -= 1;
			return Ok(Value::Array(items));
		}
		loop {
			self.space();
			items.push(self.value()?);
			self.space();
			match self.peek() {
				Some(b',') => self.at += 1,
				Some(b']') => {
					self.at += 1;
					break;
				}
				_ => return Err(ParseError),
			}
		}
		self.depth -= 1;
		Ok(Value::Array(items))
	}

	fn enter(&mut self) -> Result<(), ParseError> {
		self.depth += 1;
		if self.depth > MAX_DEPTH {
			return Err(ParseError);
		}
		Ok(())
	}

	fn string(&mut self) -> Result<String, ParseError> {
		self.expect(b'"')?;
		let mut out = String::new();
		loop {
			let byte = self.peek().ok_or(ParseError)?;
			match byte {
				b'"' => {
					self.at += 1;
					return Ok(out);
				}
				b'\\' => {
					self.at += 1;
					let escape = self.peek().ok_or(ParseError)?;
					self.at += 1;
					match escape {
						b'"' => out.push('"'),
						b'\\' => out.push('\\'),
						b'/' => out.push('/'),
						b'b' => out.push('\u{08}'),
						b'f' => out.push('\u{0c}'),
						b'n' => out.push('\n'),
						b'r' => out.push('\r'),
						b't' => out.push('\t'),
						b'u' => out.push(self.codepoint()?),
						_ => return Err(ParseError),
					}
				}
				b if b < 0x20 => return Err(ParseError),
				_ => {
					// A UTF-8 sequence: take as many bytes as the lead byte
					// says and let from_utf8 reject a broken one.
					let width = match byte {
						0x00..=0x7f => 1,
						0xc2..=0xdf => 2,
						0xe0..=0xef => 3,
						0xf0..=0xf4 => 4,
						_ => return Err(ParseError),
					};
					let end = self.at + width;
					if end > self.bytes.len() {
						return Err(ParseError);
					}
					let text = std::str::from_utf8(&self.bytes[self.at..end]).map_err(|_| ParseError)?;
					out.push_str(text);
					self.at = end;
				}
			}
		}
	}

	/// `\uXXXX`, and the surrogate pair that follows when the first half is a
	/// high one. Everything the addon sends is ASCII, so this is only here so
	/// that a legitimate escape is not a parse error.
	fn codepoint(&mut self) -> Result<char, ParseError> {
		let high = self.hex4()?;
		let value = if (0xd800..0xdc00).contains(&high) {
			self.expect(b'\\')?;
			self.expect(b'u')?;
			let low = self.hex4()?;
			if !(0xdc00..0xe000).contains(&low) {
				return Err(ParseError);
			}
			0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00)
		} else {
			high
		};
		char::from_u32(value).ok_or(ParseError)
	}

	fn hex4(&mut self) -> Result<u32, ParseError> {
		let mut value = 0u32;
		for _ in 0..4 {
			let byte = self.peek().ok_or(ParseError)?;
			self.at += 1;
			let digit = match byte {
				b'0'..=b'9' => (byte - b'0') as u32,
				b'a'..=b'f' => (byte - b'a') as u32 + 10,
				b'A'..=b'F' => (byte - b'A') as u32 + 10,
				_ => return Err(ParseError),
			};
			value = value * 16 + digit;
		}
		Ok(value)
	}

	fn number(&mut self) -> Result<Value, ParseError> {
		let start = self.at;
		if self.peek() == Some(b'-') {
			self.at += 1;
		}
		let digits_start = self.at;
		while matches!(self.peek(), Some(b'0'..=b'9')) {
			self.at += 1;
		}
		if self.at == digits_start {
			return Err(ParseError);
		}
		// JSON has no leading zeros: "01" is two tokens, not one number.
		if self.at - digits_start > 1 && self.bytes[digits_start] == b'0' {
			return Err(ParseError);
		}
		if self.peek() == Some(b'.') {
			self.at += 1;
			let fraction_start = self.at;
			while matches!(self.peek(), Some(b'0'..=b'9')) {
				self.at += 1;
			}
			if self.at == fraction_start {
				return Err(ParseError);
			}
		}
		if matches!(self.peek(), Some(b'e' | b'E')) {
			self.at += 1;
			if matches!(self.peek(), Some(b'+' | b'-')) {
				self.at += 1;
			}
			let exponent_start = self.at;
			while matches!(self.peek(), Some(b'0'..=b'9')) {
				self.at += 1;
			}
			if self.at == exponent_start {
				return Err(ParseError);
			}
		}
		let text = std::str::from_utf8(&self.bytes[start..self.at]).map_err(|_| ParseError)?;
		text.parse::<f64>().map(Value::Number).map_err(|_| ParseError)
	}
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn a_flat_object_reads_back() {
		let value = parse(r#"{"kind":"token","bitrate":320,"offline":true,"none":null}"#).unwrap();
		assert_eq!(value.get("kind").and_then(Value::as_str), Some("token"));
		assert_eq!(value.get("bitrate").and_then(Value::as_u64), Some(320));
		assert_eq!(value.get("offline"), Some(&Value::Bool(true)));
		assert_eq!(value.get("none"), Some(&Value::Null));
		assert_eq!(value.get("absent"), None);
	}

	#[test]
	fn broken_input_is_an_error_and_not_a_guess() {
		for text in ["", "{", "{\"a\":}", "{\"a\":1} trailing", "{\"a\":01}", "{'a':1}", "{\"a\":\"\\q\"}"] {
			assert_eq!(parse(text), Err(ParseError), "{text:?}");
		}
		assert_eq!(parse(&"{\"a\":".repeat(20)).map(|_| ()), Err(ParseError));
	}

	#[test]
	fn a_whole_number_is_not_a_fraction() {
		let value = parse(r#"{"a":1.5}"#).unwrap();
		assert_eq!(value.get("a").and_then(Value::as_u64), None);
	}

	#[test]
	fn strings_escape_what_would_break_a_line() {
		assert_eq!(string("a\"b\\c\nd\u{1}"), r#""a\"b\\c\nd\u0001""#);
		// Round trip through the parser: what the engine writes, the addon
		// reads, and a newline in a title can never split an event in two.
		let quoted = string("x\ny");
		assert_eq!(parse(&quoted).unwrap().as_str(), Some("x\ny"));
	}

	#[test]
	fn utf8_survives_the_round_trip() {
		let text = string("Новый трек — 日本語");
		assert_eq!(parse(&text).unwrap().as_str(), Some("Новый трек — 日本語"));
	}

	#[test]
	fn escaped_surrogates_become_one_character() {
		let value = parse(r#""\ud83c\udfb5""#).unwrap();
		assert_eq!(value.as_str(), Some("🎵"));
	}
}
