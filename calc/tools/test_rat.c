// Host golden tests for calc/rat.c (not built into the addon .so).
//
//   cc -O2 -Wall -I.. -o /tmp/test_rat test_rat.c ../rat.c && /tmp/test_rat

#include <stdio.h>
#include <string.h>

#include "rat.h"

static int failures;

static void expect_str(const char *name, const char *got, const char *want) {
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
		failures++;
	}
}

static void expect_ok(const char *name, bool ok) {
	if (!ok) {
		fprintf(stderr, "FAIL %s: expected success\n", name);
		failures++;
	}
}

static void expect_rat(const char *name, rat_t r, int64_t num, int64_t den) {
	rat_normalize(&r);
	if (r.num != num || r.den != den) {
		fprintf(stderr, "FAIL %s: got %lld/%lld, want %lld/%lld\n", name, (long long)r.num,
			(long long)r.den, (long long)num, (long long)den);
		failures++;
	}
}

static void fmt(rat_t r, char *buf, size_t n) {
	expect_ok("format", rat_format(r, buf, n, RAT_DIGITS));
}

int main(void) {
	char buf[64];
	rat_t a, b, c;

	// 1÷3×3 is exactly 1
	expect_ok("div", rat_div((rat_t){1, 1}, (rat_t){3, 1}, &a));
	expect_rat("1/3", a, 1, 3);
	expect_ok("mul", rat_mul(a, (rat_t){3, 1}, &b));
	expect_rat("1/3*3", b, 1, 1);
	fmt(b, buf, sizeof(buf));
	expect_str("fmt 1", buf, "1");

	// 1÷7×7
	expect_ok("1/7", rat_div((rat_t){1, 1}, (rat_t){7, 1}, &a));
	expect_ok("1/7*7", rat_mul(a, (rat_t){7, 1}, &b));
	expect_rat("1/7*7 exact", b, 1, 1);

	// 2/3+1/3
	expect_ok("2/3", rat_parse("2", &a) && rat_div(a, (rat_t){3, 1}, &a));
	expect_ok("1/3", rat_div((rat_t){1, 1}, (rat_t){3, 1}, &b));
	expect_ok("sum", rat_add(a, b, &c));
	expect_rat("2/3+1/3", c, 1, 1);

	// parse + format round trip for simple decimals
	expect_ok("parse 12.34", rat_parse("12.34", &a));
	expect_rat("12.34", a, 617, 50); // 1234/100 reduced
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 12.34", buf, "12.34");

	// 1/3 display
	expect_ok("1/3 again", rat_div((rat_t){1, 1}, (rat_t){3, 1}, &a));
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 1/3", buf, "0.333333333333");

	// 2/3 rounds last digit up
	expect_ok("2/3", rat_div((rat_t){2, 1}, (rat_t){3, 1}, &a));
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 2/3", buf, "0.666666666667");

	// integer
	fmt((rat_t){42, 1}, buf, sizeof(buf));
	expect_str("fmt 42", buf, "42");

	// negative
	fmt((rat_t){-5, 2}, buf, sizeof(buf));
	expect_str("fmt -2.5", buf, "-2.5");

	// zero
	fmt(rat_zero(), buf, sizeof(buf));
	expect_str("fmt 0", buf, "0");

	// divide by zero
	if (rat_div((rat_t){1, 1}, rat_zero(), &a)) {
		fprintf(stderr, "FAIL div0: expected failure\n");
		failures++;
	}

	// 1/6*6
	expect_ok("1/6", rat_div((rat_t){1, 1}, (rat_t){6, 1}, &a));
	expect_ok("1/6*6", rat_mul(a, (rat_t){6, 1}, &b));
	expect_rat("1/6*6", b, 1, 1);

	// small number → scientific ( < 1e-9 )
	expect_ok("1e-10", rat_div((rat_t){1, 1}, (rat_t){10000000000LL, 1}, &a));
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 1e-10", buf, "1e-10");

	// large integer → scientific
	expect_ok("1e12", rat_parse("1000000000000", &a));
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 1e12", buf, "1e12");

	// percent-style: 50/100
	expect_ok("50/100", rat_div((rat_t){50, 1}, (rat_t){100, 1}, &a));
	fmt(a, buf, sizeof(buf));
	expect_str("fmt 0.5", buf, "0.5");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	puts("ok");
	return 0;
}
