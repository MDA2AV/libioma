/*
 * unit.c - the handler helpers of include/ioma.h checked without a server: comparisons, the
 * typed conversions and key/value parsing. `make check` runs it before the HTTP suites.
 */
#include <ioma.h>

#include <stdio.h>
#include <string.h>

static int checks, failures;

/* Count a check; report a failed one by line. */
static void check(const char *what, bool ok, int line)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(cond) check(#cond, (cond), __LINE__)

/* A slice over a C string. */
static ioma_slice S(const char *cstr)
{
    return (ioma_slice){ cstr, strlen(cstr) };
}

/* Each conversion: does text parse to want, and does a failure leave the output alone? */
static bool i64_is(const char *text, int64_t want)   { int64_t  v = 7; return ioma_to_i64(S(text), &v) && v == want; }
static bool i64_fails(const char *text)              { int64_t  v = 7; return !ioma_to_i64(S(text), &v) && v == 7; }
static bool u64_is(const char *text, uint64_t want)  { uint64_t v = 7; return ioma_to_u64(S(text), &v) && v == want; }
static bool u64_fails(const char *text)              { uint64_t v = 7; return !ioma_to_u64(S(text), &v) && v == 7; }
static bool int_is(const char *text, int want)       { int      v = 7; return ioma_to_int(S(text), &v) && v == want; }
static bool int_fails(const char *text)              { int      v = 7; return !ioma_to_int(S(text), &v) && v == 7; }
static bool dbl_is(const char *text, double want)    { double   v = 7; return ioma_to_double(S(text), &v) && v == want; }
static bool dbl_fails(const char *text)              { double   v = 7; return !ioma_to_double(S(text), &v) && v == 7; }
static bool bool_is(const char *text, bool want)     { bool     v = !want; return ioma_to_bool(S(text), &v) && v == want; }
static bool bool_fails(const char *text)             { bool     v = true; return !ioma_to_bool(S(text), &v) && v; }

static void test_integers(void)
{
    CHECK(i64_is("0", 0));
    CHECK(i64_is("42", 42));
    CHECK(i64_is("-42", -42));
    CHECK(i64_is("007", 7));
    CHECK(i64_is("-0", 0));
    CHECK(i64_is("9223372036854775807", INT64_MAX));
    CHECK(i64_is("-9223372036854775808", INT64_MIN));
    CHECK(i64_fails("9223372036854775808"));
    CHECK(i64_fails("-9223372036854775809"));
    CHECK(i64_fails("99999999999999999999"));
    CHECK(i64_fails(""));
    CHECK(i64_fails("-"));
    CHECK(i64_fails("+1"));
    CHECK(i64_fails(" 1"));
    CHECK(i64_fails("1 "));
    CHECK(i64_fails("1a"));
    CHECK(i64_fails("12.5"));
    CHECK(i64_fails("1e3"));
    CHECK(i64_fails("0x10"));
    CHECK(i64_fails("--1"));

    CHECK(u64_is("0", 0));
    CHECK(u64_is("18446744073709551615", UINT64_MAX));
    CHECK(u64_fails("18446744073709551616"));
    CHECK(u64_fails("-1"));
    CHECK(u64_fails(""));

    CHECK(int_is("2147483647", INT32_MAX));
    CHECK(int_is("-2147483648", INT32_MIN));
    CHECK(int_fails("2147483648"));
    CHECK(int_fails("-2147483649"));
    CHECK(int_fails("x"));
}

static void test_doubles(void)
{
    CHECK(dbl_is("0", 0.0));
    CHECK(dbl_is("2.5", 2.5));
    CHECK(dbl_is("-0.5", -0.5));
    CHECK(dbl_is(".5", 0.5));
    CHECK(dbl_is("1.", 1.0));
    CHECK(dbl_is("1e3", 1000.0));
    CHECK(dbl_is("1E-2", 0.01));
    CHECK(dbl_is("1e+2", 100.0));
    CHECK(dbl_is("0.1", 0.1));                                   /* rounded like the compiler does */
    CHECK(dbl_is("9007199254740993", 9007199254740992.0));       /* halfway case: round to even */
    CHECK(dbl_is("2.2250738585072011e-308", 2.2250738585072011e-308));   /* a subnormal, once a famous hang */
    CHECK(dbl_is("1e-400", 0.0));                                /* underflow rounds to zero */
    CHECK(dbl_is("-1e-400", 0.0));
    CHECK(dbl_is("1.7976931348623157e308", 1.7976931348623157e308));
    CHECK(dbl_fails("1e400"));                                   /* overflow */
    CHECK(dbl_fails(""));
    CHECK(dbl_fails("-"));
    CHECK(dbl_fails("."));
    CHECK(dbl_fails("1e"));
    CHECK(dbl_fails("e5"));
    CHECK(dbl_fails("1.2.3"));
    CHECK(dbl_fails("inf"));
    CHECK(dbl_fails("nan"));
    CHECK(dbl_fails("0x10"));
    CHECK(dbl_fails(" 1.5"));
    CHECK(dbl_fails("1.5 "));
    CHECK(dbl_fails("+1"));
    CHECK(dbl_fails("1,5"));
    CHECK(dbl_fails("1_000"));
    char long_text[200];
    memset(long_text, '1', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';
    CHECK(dbl_fails(long_text));                                 /* longer than any real number */
}

static void test_bools(void)
{
    CHECK(bool_is("true", true));
    CHECK(bool_is("TRUE", true));
    CHECK(bool_is("True", true));
    CHECK(bool_is("1", true));
    CHECK(bool_is("yes", true));
    CHECK(bool_is("on", true));
    CHECK(bool_is("false", false));
    CHECK(bool_is("0", false));
    CHECK(bool_is("no", false));
    CHECK(bool_is("OFF", false));
    CHECK(bool_fails(""));
    CHECK(bool_fails("2"));
    CHECK(bool_fails("t"));
    CHECK(bool_fails("truee"));
}

static void test_strings(void)
{
    CHECK(ioma_slice_eq(S("abc"), "abc"));
    CHECK(!ioma_slice_eq(S("abc"), "ab"));
    CHECK(!ioma_slice_eq(S("abc"), "abcd"));
    CHECK(ioma_slice_eq(S(""), ""));
    CHECK(ioma_slice_eq((ioma_slice){ nullptr, 0 }, ""));         /* an absent slice is empty */

    CHECK(ioma_slice_eq_ci(S("Content-Type"), "content-type"));
    CHECK(ioma_slice_eq_ci(S("GZIP"), "gzip"));
    CHECK(!ioma_slice_eq_ci(S("gzip"), "gzi"));
    CHECK(!ioma_slice_eq_ci(S("gzip"), "gzipx"));

    CHECK(ioma_slice_starts_with(S("/api/users"), "/api/"));
    CHECK(ioma_slice_starts_with(S("/api/users"), ""));
    CHECK(!ioma_slice_starts_with(S("/api"), "/api/"));
    CHECK(ioma_slice_ends_with(S("data.json"), ".json"));
    CHECK(!ioma_slice_ends_with(S("json"), ".json"));

    ioma_slice t = ioma_slice_trim(S("  a b \t\r\n"));
    CHECK(t.len == 3 && memcmp(t.p, "a b", 3) == 0);
    CHECK(ioma_slice_trim(S(" \t ")).len == 0);
    CHECK(ioma_slice_trim(S("")).len == 0);
    CHECK(ioma_slice_trim(S("x")).len == 1);

    char buf[4];
    CHECK(ioma_cstr(S("abc"), buf, sizeof buf) && strcmp(buf, "abc") == 0);
    CHECK(!ioma_cstr(S("abcd"), buf, sizeof buf) && strcmp(buf, "abc") == 0);   /* what fit, terminated */
    CHECK(ioma_cstr(S(""), buf, sizeof buf) && buf[0] == '\0');
    buf[0] = 'x';
    CHECK(!ioma_cstr(S("a"), buf, 0) && buf[0] == 'x');                         /* cap 0 writes nothing */
}

static void test_kv_parse(void)
{
    ioma_kv kv[8];
    char    arena[64];
    size_t  n = ioma_kv_parse("a=1&b=hello+world&c=%41%zz&&d&e=", 32, kv, 8, arena, sizeof arena);
    CHECK(n == 5);
    CHECK(ioma_slice_eq(kv[0].key, "a") && ioma_slice_eq(kv[0].value, "1"));
    CHECK(ioma_slice_eq(kv[1].key, "b") && ioma_slice_eq(kv[1].value, "hello world"));
    CHECK(ioma_slice_eq(kv[2].key, "c") && ioma_slice_eq(kv[2].value, "A%zz"));   /* a bad escape stays */
    CHECK(ioma_slice_eq(kv[3].key, "d") && kv[3].value.len == 0);
    CHECK(ioma_slice_eq(kv[4].key, "e") && kv[4].value.len == 0);
    CHECK(kv[0].value.p != arena && kv[1].value.p >= arena);   /* a view when undecoded, else in the arena */

    n = ioma_kv_parse("k%20ey=v&x=y", 12, kv, 8, arena, sizeof arena);
    CHECK(n == 2 && ioma_slice_eq(kv[0].key, "k ey"));
    n = ioma_kv_parse("a=1&b=x+y&c=3", 13, kv, 8, arena, 0);   /* no arena: the pair needing it is skipped */
    CHECK(n == 2 && ioma_slice_eq(kv[1].key, "c"));
    n = ioma_kv_parse("a=1&b=2&c=3", 11, kv, 1, arena, sizeof arena);
    CHECK(n == 1);

    int v;
    CHECK(ioma_kv_parse("page=12", 7, kv, 8, arena, sizeof arena) == 1 && ioma_to_int(kv[0].value, &v) && v == 12);
}

int main(void)
{
    test_integers();
    test_doubles();
    test_bools();
    test_strings();
    test_kv_parse();
    printf("unit: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
