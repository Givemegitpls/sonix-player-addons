// Exact rationals for the calculator (+ − × ÷), Windows/AOSP model on int64 limbs.
// Display rounds to a fixed number of significant digits; the value stays exact.

#ifndef CALC_RAT_H
#define CALC_RAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAT_DIGITS 12

typedef struct {
	int64_t num;
	int64_t den; // > 0 after normalize
} rat_t;

rat_t rat_zero(void);
void rat_normalize(rat_t *r);

bool rat_add(rat_t a, rat_t b, rat_t *out);
bool rat_sub(rat_t a, rat_t b, rat_t *out);
bool rat_mul(rat_t a, rat_t b, rat_t *out);
bool rat_div(rat_t a, rat_t b, rat_t *out);

bool rat_parse(const char *s, rat_t *out);

// Round |r| to `digits` significant digits (typically RAT_DIGITS). Fixed form
// when 1e-9 <= |r| < 1e12, otherwise scientific. Returns false on bad den.
bool rat_format(rat_t r, char *out, size_t size, int digits);

#endif /* CALC_RAT_H */
