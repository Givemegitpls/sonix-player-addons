#include "rat.h"

#include <stdio.h>
#include <string.h>

static int64_t i64_abs(int64_t x) {
	return x < 0 ? -x : x;
}

static int64_t igcd(int64_t a, int64_t b) {
	a = i64_abs(a);
	b = i64_abs(b);
	while (b) {
		int64_t t = a % b;
		a = b;
		b = t;
	}
	return a ? a : 1;
}

rat_t rat_zero(void) {
	return (rat_t){0, 1};
}

void rat_normalize(rat_t *r) {
	if (!r->den) {
		r->num = 0;
		r->den = 1;
		return;
	}
	if (r->den < 0) {
		r->num = -r->num;
		r->den = -r->den;
	}
	if (r->num == 0) {
		r->den = 1;
		return;
	}
	int64_t g = igcd(r->num, r->den);
	r->num /= g;
	r->den /= g;
}

static bool mul_i64(int64_t a, int64_t b, int64_t *out) {
	if (a == 0 || b == 0) {
		*out = 0;
		return true;
	}
	if (a == INT64_MIN && b == -1) {
		return false;
	}
	if (b == INT64_MIN && a == -1) {
		return false;
	}
	int64_t aa = i64_abs(a);
	int64_t bb = i64_abs(b);
	if (aa > INT64_MAX / bb) {
		return false;
	}
	*out = a * b;
	return true;
}

bool rat_add(rat_t a, rat_t b, rat_t *out) {
	rat_normalize(&a);
	rat_normalize(&b);
	int64_t g = igcd(a.den, b.den);
	int64_t a_den = a.den / g;
	int64_t b_den = b.den / g;
	int64_t t1, t2, num, den;
	if (!mul_i64(a.num, b_den, &t1) || !mul_i64(b.num, a_den, &t2)) {
		return false;
	}
	if ((t2 > 0 && t1 > INT64_MAX - t2) || (t2 < 0 && t1 < INT64_MIN - t2)) {
		return false;
	}
	num = t1 + t2;
	if (!mul_i64(a_den, b.den, &den)) {
		return false;
	}
	out->num = num;
	out->den = den;
	rat_normalize(out);
	return true;
}

bool rat_sub(rat_t a, rat_t b, rat_t *out) {
	b.num = -b.num;
	return rat_add(a, b, out);
}

bool rat_mul(rat_t a, rat_t b, rat_t *out) {
	rat_normalize(&a);
	rat_normalize(&b);
	int64_t g1 = igcd(a.num, b.den);
	a.num /= g1;
	b.den /= g1;
	int64_t g2 = igcd(b.num, a.den);
	b.num /= g2;
	a.den /= g2;
	int64_t num, den;
	if (!mul_i64(a.num, b.num, &num) || !mul_i64(a.den, b.den, &den)) {
		return false;
	}
	out->num = num;
	out->den = den;
	rat_normalize(out);
	return true;
}

bool rat_div(rat_t a, rat_t b, rat_t *out) {
	rat_normalize(&b);
	if (b.num == 0) {
		return false;
	}
	rat_t inv = {b.den, b.num};
	if (inv.den < 0) {
		inv.num = -inv.num;
		inv.den = -inv.den;
	}
	return rat_mul(a, inv, out);
}

bool rat_parse(const char *s, rat_t *out) {
	if (!s || !s[0]) {
		*out = rat_zero();
		return true;
	}
	int sign = 1;
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	int64_t num = 0;
	int64_t den = 1;
	bool seen_dot = false;
	bool any = false;
	for (; *s; s++) {
		if (*s == '.') {
			if (seen_dot) {
				return false;
			}
			seen_dot = true;
			continue;
		}
		if (*s < '0' || *s > '9') {
			return false;
		}
		any = true;
		int dig = *s - '0';
		if (num > (INT64_MAX - dig) / 10) {
			return false;
		}
		num = num * 10 + dig;
		if (seen_dot) {
			if (den > INT64_MAX / 10) {
				return false;
			}
			den *= 10;
		}
	}
	if (!any) {
		*out = rat_zero();
		return true;
	}
	out->num = sign < 0 ? -num : num;
	out->den = den;
	rat_normalize(out);
	return true;
}

static int count_digits_u64(uint64_t v) {
	int n = 1;
	while (v >= 10) {
		v /= 10;
		n++;
	}
	return n;
}

static void strip_trailing_zeros(char *s) {
	char *dot = strchr(s, '.');
	if (!dot) {
		return;
	}
	char *end = s + strlen(s) - 1;
	while (end > dot && *end == '0') {
		*end-- = '\0';
	}
	if (end == dot) {
		*end = '\0';
	}
}

// Single-pass display (Ratpack NumberToString idea on int64 rationals):
//   1) find order of leading digit
//   2) long-divide into digits+1 significand digits
//   3) round half-up
//   4) fixed or scientific by order window
bool rat_format(rat_t r, char *out, size_t size, int digits) {
	if (!out || size == 0 || digits < 1 || digits > 18) {
		return false;
	}
	rat_normalize(&r);
	if (r.den == 0) {
		return false;
	}
	if (r.num == 0) {
		snprintf(out, size, "0");
		return true;
	}

	int neg = r.num < 0;
	uint64_t a = (uint64_t)(neg ? -r.num : r.num);
	uint64_t b = (uint64_t)r.den;

	// order = floor(log10(|r|)): leading digit sits at 10^order.
	int order;
	if (a >= b) {
		order = count_digits_u64(a / b) - 1;
	} else {
		order = -1;
		uint64_t t = a;
		for (;;) {
			if (t > UINT64_MAX / 10) {
				return false;
			}
			t *= 10;
			if (t >= b) {
				break;
			}
			order--;
		}
	}

	// Scale (a, b) so a/b is in [1, 10): multiply b by 10^order when order>0,
	// or multiply a by 10^(-order) when order<0.
	uint64_t rem = a;
	uint64_t div = b;
	if (order > 0) {
		for (int i = 0; i < order; i++) {
			if (div > UINT64_MAX / 10) {
				// Shrink rem instead; same ratio to first order.
				rem /= 10;
			} else {
				div *= 10;
			}
		}
	} else if (order < 0) {
		for (int i = 0; i < -order; i++) {
			if (rem > UINT64_MAX / 10) {
				return false;
			}
			rem *= 10;
		}
	}
	// rem/div in [1, 10) aside from rounding noise of the shrink path.

	char digs[20];
	if (digits + 1 > (int)sizeof(digs)) {
		return false;
	}
	for (int i = 0; i < digits + 1; i++) {
		uint64_t q = rem / div;
		if (q > 9) {
			// Shrink path left rem/div >= 10; peel extra into order.
			while (rem / div >= 10) {
				rem /= 10;
				order++;
			}
			q = rem / div;
		}
		digs[i] = (char)('0' + q);
		rem %= div;
		if (rem > UINT64_MAX / 10) {
			rem = 0;
		} else {
			rem *= 10;
		}
	}

	// Round half-up on the guard digit.
	if (digs[digits] >= '5') {
		for (int i = digits - 1; i >= 0; i--) {
			if (digs[i] < '9') {
				digs[i]++;
				break;
			}
			digs[i] = '0';
			if (i == 0) {
				// 999... → 1000...; bump order, keep `digits` significants.
				memmove(digs + 1, digs, (size_t)digits);
				digs[0] = '1';
				order++;
			}
		}
	}
	digs[digits] = '\0';
	{
		size_t n = strlen(digs);
		while (n > 1 && digs[n - 1] == '0') {
			digs[--n] = '\0';
		}
	}

	// Window matches the old phone-calc display: fixed in [1e-9, 1e12).
	bool scientific = (order >= 12) || (order <= -10);

	char tmp[64];
	size_t pos = 0;
	if (neg) {
		tmp[pos++] = '-';
	}

	if (scientific) {
		tmp[pos++] = digs[0];
		if (digs[1]) {
			tmp[pos++] = '.';
			for (size_t i = 1; digs[i]; i++) {
				tmp[pos++] = digs[i];
			}
		}
		tmp[pos] = '\0';
		char expbuf[16];
		snprintf(expbuf, sizeof(expbuf), "e%d", order);
		if (pos + strlen(expbuf) + 1 > sizeof(tmp)) {
			return false;
		}
		memcpy(tmp + pos, expbuf, strlen(expbuf) + 1);
	} else if (order >= 0) {
		int int_digits = order + 1;
		int dig_len = (int)strlen(digs);
		if (int_digits >= dig_len) {
			for (int i = 0; i < dig_len; i++) {
				tmp[pos++] = digs[i];
			}
			for (int i = dig_len; i < int_digits; i++) {
				tmp[pos++] = '0';
			}
			tmp[pos] = '\0';
		} else {
			for (int i = 0; i < int_digits; i++) {
				tmp[pos++] = digs[i];
			}
			tmp[pos++] = '.';
			for (int i = int_digits; i < dig_len; i++) {
				tmp[pos++] = digs[i];
			}
			tmp[pos] = '\0';
		}
	} else {
		tmp[pos++] = '0';
		tmp[pos++] = '.';
		for (int i = 0; i < -order - 1; i++) {
			tmp[pos++] = '0';
		}
		for (size_t i = 0; digs[i]; i++) {
			tmp[pos++] = digs[i];
		}
		tmp[pos] = '\0';
	}

	strip_trailing_zeros(tmp);
	if (tmp[0] == '-' && strcmp(tmp + 1, "0") == 0) {
		snprintf(out, size, "0");
		return true;
	}
	if (strlen(tmp) >= size) {
		return false;
	}
	memcpy(out, tmp, strlen(tmp) + 1);
	return true;
}
