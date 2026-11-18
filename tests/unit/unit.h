/*
 * unit.h - the little the unit tests need: checks that report and carry on, and a
 * runner. A test file includes the source file it tests, so its static functions are
 * in reach, then this. What that file calls of the rest of mwc it stubs; the linker
 * drops the functions no test reaches (--gc-sections), and with them their calls.
 *
 *   TEST(name) { CHECK(cond); CHECK_INT(got, want); }
 *   int main(void) { RUN(name); ...; return unit_done(); }
 *
 * Built with ASan and UBSan always: an overflow is a failure, not a passing test.
 */
#ifndef MWC_UNIT_H
#define MWC_UNIT_H

#include <stdio.h>

static int unit_failed, unit_checks, unit_tests;
static const char *unit_name;

#define TEST(name) static void test_##name(void)

#define RUN(name) do { \
	unit_name = #name; \
	unit_tests++; \
	test_##name(); \
} while (0)

#define CHECK(cond) do { \
	unit_checks++; \
	if (!(cond)) { \
		unit_failed++; \
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, unit_name, #cond); \
	} \
} while (0)

/* Both sides once, as long long: says what each was. */
#define CHECK_INT(got, want) do { \
	long long unit_g = (got), unit_w = (want); \
	unit_checks++; \
	if (unit_g != unit_w) { \
		unit_failed++; \
		fprintf(stderr, "%s:%d: %s: %s is %lld, expected %lld\n", __FILE__, \
			__LINE__, unit_name, #got, unit_g, unit_w); \
	} \
} while (0)

static int unit_done(void)
{
	printf("%s: %d tests, %d checks, %d failed\n", __BASE_FILE__, unit_tests,
		unit_checks, unit_failed);
	return unit_failed ? 1 : 0;
}

#endif
