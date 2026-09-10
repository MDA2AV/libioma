/*
 * unit.c - the handler helpers of include/ioxd.h checked without a server: comparisons, the
 * typed conversions and key/value parsing. `make check` runs it before the HTTP suites.
 */
#include <ioxd.h>

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "io/spsc.h"

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
static ioxd_slice S(const char *cstr)
{
    return (ioxd_slice){ cstr, strlen(cstr) };
}

/* Each conversion: does text parse to want, and does a failure leave the output alone? */
static bool i64_is(const char *text, int64_t want)   { int64_t  v = 7; return ioxd_to_i64(S(text), &v) && v == want; }
static bool i64_fails(const char *text)              { int64_t  v = 7; return !ioxd_to_i64(S(text), &v) && v == 7; }
static bool u64_is(const char *text, uint64_t want)  { uint64_t v = 7; return ioxd_to_u64(S(text), &v) && v == want; }
static bool u64_fails(const char *text)              { uint64_t v = 7; return !ioxd_to_u64(S(text), &v) && v == 7; }
static bool int_is(const char *text, int want)       { int      v = 7; return ioxd_to_int(S(text), &v) && v == want; }
static bool int_fails(const char *text)              { int      v = 7; return !ioxd_to_int(S(text), &v) && v == 7; }
static bool dbl_is(const char *text, double want)    { double   v = 7; return ioxd_to_double(S(text), &v) && v == want; }
static bool dbl_fails(const char *text)              { double   v = 7; return !ioxd_to_double(S(text), &v) && v == 7; }
static bool bool_is(const char *text, bool want)     { bool     v = !want; return ioxd_to_bool(S(text), &v) && v == want; }
static bool bool_fails(const char *text)             { bool     v = true; return !ioxd_to_bool(S(text), &v) && v; }

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
    CHECK(ioxd_slice_eq(S("abc"), "abc"));
    CHECK(!ioxd_slice_eq(S("abc"), "ab"));
    CHECK(!ioxd_slice_eq(S("abc"), "abcd"));
    CHECK(ioxd_slice_eq(S(""), ""));
    CHECK(ioxd_slice_eq((ioxd_slice){ nullptr, 0 }, ""));         /* an absent slice is empty */

    CHECK(ioxd_slice_eq_ci(S("Content-Type"), "content-type"));
    CHECK(ioxd_slice_eq_ci(S("GZIP"), "gzip"));
    CHECK(!ioxd_slice_eq_ci(S("gzip"), "gzi"));
    CHECK(!ioxd_slice_eq_ci(S("gzip"), "gzipx"));

    CHECK(ioxd_slice_starts_with(S("/api/users"), "/api/"));
    CHECK(ioxd_slice_starts_with(S("/api/users"), ""));
    CHECK(!ioxd_slice_starts_with(S("/api"), "/api/"));
    CHECK(ioxd_slice_ends_with(S("data.json"), ".json"));
    CHECK(!ioxd_slice_ends_with(S("json"), ".json"));

    ioxd_slice t = ioxd_slice_trim(S("  a b \t\r\n"));
    CHECK(t.len == 3 && memcmp(t.p, "a b", 3) == 0);
    CHECK(ioxd_slice_trim(S(" \t ")).len == 0);
    CHECK(ioxd_slice_trim(S("")).len == 0);
    CHECK(ioxd_slice_trim(S("x")).len == 1);

    char buf[4];
    CHECK(ioxd_cstr(S("abc"), buf, sizeof buf) && strcmp(buf, "abc") == 0);
    CHECK(!ioxd_cstr(S("abcd"), buf, sizeof buf) && strcmp(buf, "abc") == 0);   /* what fit, terminated */
    CHECK(ioxd_cstr(S(""), buf, sizeof buf) && buf[0] == '\0');
    buf[0] = 'x';
    CHECK(!ioxd_cstr(S("a"), buf, 0) && buf[0] == 'x');                         /* cap 0 writes nothing */
}

static void test_kv_parse(void)
{
    ioxd_kv kv[8];
    char    arena[64];
    size_t  n = ioxd_kv_parse("a=1&b=hello+world&c=%41%zz&&d&e=", 32, kv, 8, arena, sizeof arena, NULL);
    CHECK(n == 5);
    CHECK(ioxd_slice_eq(kv[0].key, "a") && ioxd_slice_eq(kv[0].value, "1"));
    CHECK(ioxd_slice_eq(kv[1].key, "b") && ioxd_slice_eq(kv[1].value, "hello world"));
    CHECK(ioxd_slice_eq(kv[2].key, "c") && ioxd_slice_eq(kv[2].value, "A%zz"));   /* a bad escape stays */
    CHECK(ioxd_slice_eq(kv[3].key, "d") && kv[3].value.len == 0);
    CHECK(ioxd_slice_eq(kv[4].key, "e") && kv[4].value.len == 0);
    CHECK(kv[0].value.p != arena && kv[1].value.p >= arena);   /* a view when undecoded, else in the arena */

    n = ioxd_kv_parse("k%20ey=v&x=y", 12, kv, 8, arena, sizeof arena, NULL);
    CHECK(n == 2 && ioxd_slice_eq(kv[0].key, "k ey"));
    n = ioxd_kv_parse("a=1&b=x+y&c=3", 13, kv, 8, arena, 0, NULL);   /* no arena: the pair needing it is skipped */
    CHECK(n == 2 && ioxd_slice_eq(kv[1].key, "c"));
    n = ioxd_kv_parse("a=1&b=2&c=3", 11, kv, 1, arena, sizeof arena, NULL);
    CHECK(n == 1);

    int v;
    CHECK(ioxd_kv_parse("page=12", 7, kv, 8, arena, sizeof arena, NULL) == 1 && ioxd_to_int(kv[0].value, &v) && v == 12);
}

/* The JSON writer into memory: what a document looks like, byte for byte. */
static bool json_is(const char *want, void (*write)(ioxd_json *))
{
    char   buf[512];
    size_t len;
    ioxd_json j = ioxd_json_mem(buf, sizeof buf, &len);
    write(&j);
    return !j.failed && len == strlen(want) && memcmp(buf, want, len) == 0;
}
static void doc_nested(ioxd_json *j)
{
    ioxd_json_object(j);
    ioxd_json_key(j, "id");    ioxd_json_int(j, 42);
    ioxd_json_key(j, "name");  ioxd_json_cstr(j, "Zo\xc3\xab \"Z\" O'Neil\n\t\x01");
    ioxd_json_key(j, "tags");  ioxd_json_array(j); ioxd_json_cstr(j, "a"); ioxd_json_cstr(j, "b"); ioxd_json_end(j);
    ioxd_json_key(j, "empty"); ioxd_json_object(j); ioxd_json_end(j);
    ioxd_json_key(j, "none");  ioxd_json_null(j);
    ioxd_json_key(j, "ok");    ioxd_json_bool(j, true);
    ioxd_json_key(j, "raw");   ioxd_json_raw(j, S("[1,2]"));
    ioxd_json_end(j);
}
static void doc_numbers(ioxd_json *j)
{
    ioxd_json_array(j);
    ioxd_json_int(j, 0); ioxd_json_int(j, -7); ioxd_json_int(j, INT64_MIN); ioxd_json_int(j, INT64_MAX);
    ioxd_json_uint(j, UINT64_MAX);
    ioxd_json_double(j, 0.1); ioxd_json_double(j, 2.5); ioxd_json_double(j, -0.0); ioxd_json_double(j, 1e21);
    ioxd_json_double(j, 9007199254740993.0); ioxd_json_double(j, 1.0 / 3.0);
    ioxd_json_double(j, INFINITY); ioxd_json_double(j, NAN);
    ioxd_json_end(j);
}
/* A float is not a double: 6 to 9 significant digits, round-tripped against the float itself. */
static void doc_floats(ioxd_json *j)
{
    ioxd_json_array(j);
    ioxd_json_float(j, 0.1F); ioxd_json_float(j, 1.0F / 3.0F); ioxd_json_float(j, 16777217.0F);
    ioxd_json_float(j, -0.0F); ioxd_json_float(j, 1e20F);
    ioxd_json_float(j, INFINITY); ioxd_json_float(j, NAN);
    ioxd_json_end(j);
}
/* The reals whose text the decimal point of a locale would spoil. */
static void doc_reals(ioxd_json *j)
{
    ioxd_json_array(j);
    ioxd_json_double(j, 0.1); ioxd_json_double(j, -2.5e-7); ioxd_json_float(j, 0.5F);
    ioxd_json_end(j);
}
/* Bytes are bytes: what is not valid UTF-8 goes out exactly as it came in. */
static void doc_bad_utf8(ioxd_json *j) { ioxd_json_string(j, S("\xff\xfe\x80 ok")); }
static void doc_top_level(ioxd_json *j) { ioxd_json_cstr(j, "just a string"); }
static void doc_array_of_arrays(ioxd_json *j)
{
    ioxd_json_array(j);
    ioxd_json_array(j); ioxd_json_int(j, 1); ioxd_json_end(j);
    ioxd_json_array(j); ioxd_json_end(j);
    ioxd_json_end(j);
}

/* Structs described once: every kind of field, nested twice, arrays of both scalars and objects. */
#define ADDRESS_FIELDS(X)                    \
    X(VALUE,   const char *, city)           \
    X(VALUE,   const char *, zip)
IOXD_JSON_STRUCT(address, ADDRESS_FIELDS)

#define ORDER_FIELDS(X)                      \
    X(VALUE,   int,          number)         \
    X(VALUE,   double,       total)          \
    X(ARRAY,   int,          items, n_items)
IOXD_JSON_STRUCT(order, ORDER_FIELDS)

#define USER_FIELDS(X)                       \
    X(VALUE,    int64_t,      id)            \
    X(VALUE,    const char *, name)          \
    X(VALUE,    bool,         active)        \
    X(VALUE,    ioxd_slice,   handle)        \
    X(VALUE,    unsigned,     visits)        \
    X(OBJECT,   address,      address)       \
    X(OPTIONAL, address,      billing)       \
    X(ARRAY,    const char *, tags,   n_tags)   \
    X(OBJECTS,  order,        orders, n_orders)
IOXD_JSON_STRUCT(user, USER_FIELDS)

static void doc_struct(ioxd_json *j)
{
    const char *tags[]  = { "new", "vip" };
    int         items[] = { 7, 9 };
    struct order   orders[] = { { 1, 9.5, items, 2 }, { 2, 0.25, items, 0 } };
    struct address billing  = { "Lisboa", "1000-001" };
    struct user u = {
        .id = 42, .name = "Zo\xc3\xab \"Z\"", .active = true, .handle = S("zoe"), .visits = 3,
        .address = { "Porto", NULL }, .billing = &billing,
        .tags = tags, .n_tags = 2, .orders = orders, .n_orders = 2,
    };
    user_to_json(j, &u);
}
static void doc_struct_empty(ioxd_json *j)
{
    struct user u = { .id = 1, .name = NULL, .handle = S(""), .address = { NULL, NULL } };
    user_to_json(j, &u);
}
/* The field macro as a statement, which must not be a discarded value, and as a condition. */
static void doc_field_macro(ioxd_json *j)
{
    ioxd_json_object(j);
    IOXD_JSON_FIELD(j, "n", 5);
    IOXD_JSON_FIELD(j, "x", 2.5);
    IOXD_JSON_FIELD(j, "f", 0.1F);                  /* a float takes the float writer */
    IOXD_JSON_FIELD(j, "s", "str");
    IOXD_JSON_FIELD(j, "b", false);
    if (!IOXD_JSON_FIELD(j, "u", 7U))
        return;
    IOXD_JSON_FIELD(j, "sl", S("slice"));
    ioxd_json_end(j);
}

/* Where a value may go: a key only in an object, one key per value, an end only once the pair it
 * closes is complete. Every refusal is marked failed, so nothing goes unreported. */
static void test_json_levels(void)
{
    char   buf[128];
    size_t len;

    ioxd_json j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(!ioxd_json_key(&j, "k") && j.failed);                  /* a key at the top level */

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_array(&j) && !ioxd_json_key(&j, "k") && j.failed);       /* a key in an array */

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_object(&j) && !ioxd_json_int(&j, 1) && j.failed);        /* a value, no key */

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_object(&j) && ioxd_json_key(&j, "a") && !ioxd_json_key(&j, "b") && j.failed);

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_object(&j) && ioxd_json_key(&j, "a") && !ioxd_json_end(&j) && j.failed);

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(!ioxd_json_end(&j) && j.failed);                       /* nothing open, and it says so */
    CHECK(!ioxd_json_object(&j));                                /* failed stays failed */

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_array(&j) && !ioxd_json_raw(&j, S("")) && j.failed);  /* nothing is no value */

    j = ioxd_json_mem(buf, sizeof buf, &len);
    CHECK(ioxd_json_done(&j));                                   /* nothing written, none wrong */
    CHECK(ioxd_json_object(&j) && !ioxd_json_done(&j));          /* the object is still open */
    CHECK(ioxd_json_key(&j, "a") && ioxd_json_int(&j, 1) && !ioxd_json_done(&j));
    CHECK(ioxd_json_end(&j) && ioxd_json_done(&j));              /* now it is whole */
    CHECK(!ioxd_json_end(&j) && !ioxd_json_done(&j));            /* one end too many */
}

/* The decimal point is '.' whatever LC_NUMERIC says: the numbers are formatted in the writer's
 * own "C" locale and the thread's is handed straight back. A locale with a point of its own is
 * not installed everywhere, so this says so and skips when there is none. */
/* NOLINTBEGIN(concurrency-mt-unsafe): the process locale is switched here, single-threaded, before any worker exists */
static void test_json_locale(void)
{
    static const char *const others[] = {
        "ps_AF.UTF-8", "fa_IR.UTF-8",                            /* a point two bytes long */
        "de_DE.UTF-8", "fr_FR.UTF-8",                            /* a comma                */
    };
    char        saved[64];
    const char *was = setlocale(LC_NUMERIC, NULL);
    snprintf(saved, sizeof saved, "%s", was ? was : "C");

    const char *set = NULL;
    for (size_t i = 0; i < sizeof others / sizeof *others && !set; i++)
        set = setlocale(LC_NUMERIC, others[i]);
    if (!set) {
        printf("note: no locale with a point of its own installed; that check skipped\n");
        return;
    }
    CHECK(strcmp(localeconv()->decimal_point, ".") != 0);        /* the locale really differs */
    CHECK(json_is("[0.1,-2.5e-07,0.5]", doc_reals));             /* and the JSON does not */
    CHECK(strcmp(localeconv()->decimal_point, ".") != 0);        /* the thread's was handed back */
    setlocale(LC_NUMERIC, saved);
}
/* NOLINTEND(concurrency-mt-unsafe) */

static void test_json(void)
{
    CHECK(json_is("{\"id\":42,\"name\":\"Zo\xc3\xab \\\"Z\\\"\",\"active\":true,\"handle\":\"zoe\",\"visits\":3,"
                  "\"address\":{\"city\":\"Porto\",\"zip\":null},\"billing\":{\"city\":\"Lisboa\",\"zip\":\"1000-001\"},"
                  "\"tags\":[\"new\",\"vip\"],\"orders\":[{\"number\":1,\"total\":9.5,\"items\":[7,9]},{\"number\":2,\"total\":0.25,\"items\":[]}]}", doc_struct));
    CHECK(json_is("{\"id\":1,\"name\":null,\"active\":false,\"handle\":\"\",\"visits\":0,\"address\":{\"city\":null,\"zip\":null},\"billing\":null,\"tags\":[],\"orders\":[]}", doc_struct_empty));
    CHECK(json_is("{\"n\":5,\"x\":2.5,\"f\":0.1,\"s\":\"str\",\"b\":false,\"u\":7,\"sl\":\"slice\"}", doc_field_macro));

    CHECK(json_is("{\"id\":42,\"name\":\"Zo\xc3\xab \\\"Z\\\" O'Neil\\n\\t\\u0001\",\"tags\":[\"a\",\"b\"],\"empty\":{},\"none\":null,\"ok\":true,\"raw\":[1,2]}", doc_nested));
    CHECK(json_is("[0,-7,-9223372036854775808,9223372036854775807,18446744073709551615,0.1,2.5,-0,1e+21,9007199254740992,0.3333333333333333,null,null]", doc_numbers));
    CHECK(json_is("[0.1,0.33333334,16777216,-0,1e+20,null,null]", doc_floats));
    CHECK(json_is("[0.1,-2.5e-07,0.5]", doc_reals));
    CHECK(json_is("\"\xff\xfe\x80 ok\"", doc_bad_utf8));
    CHECK(json_is("\"just a string\"", doc_top_level));
    CHECK(json_is("[[1],[]]", doc_array_of_arrays));

    char   small[8];
    size_t len;
    ioxd_json j = ioxd_json_mem(small, sizeof small, &len);
    CHECK(ioxd_json_object(&j) && ioxd_json_key(&j, "k"));      /* {"k": is 5 bytes */
    CHECK(!ioxd_json_cstr(&j, "too long for what is left") && j.failed);
    CHECK(!ioxd_json_int(&j, 1));                                /* failed stays failed */

    /* Every level has a bit of its own, the deepest included: all the way down and out again. */
    char   deep[512];
    j = ioxd_json_mem(deep, sizeof deep, &len);
    bool ok = true;
    for (int i = 0; i < IOXD_JSON_DEPTH; i++)
        ok = ok && ioxd_json_array(&j);
    for (int i = 0; i < IOXD_JSON_DEPTH; i++)
        ok = ok && ioxd_json_end(&j);
    CHECK(ok && ioxd_json_done(&j) && len == 2 * (size_t)IOXD_JSON_DEPTH);

    j = ioxd_json_mem(deep, sizeof deep, &len);
    ok = true;
    for (int i = 0; i < IOXD_JSON_DEPTH; i++)
        ok = ok && ioxd_json_array(&j);
    ok = ok && ioxd_json_int(&j, 1);                        /* the deepest level owes a comma */
    size_t written = len;
    CHECK(ok && !ioxd_json_array(&j) && len == written);     /* too deep: not even that comma */

    /* Longer than one run of the sink, byte for byte through the run loop. */
    char big[4096];
    char longer[2000];
    memset(longer, 'x', sizeof longer);
    j = ioxd_json_mem(big, sizeof big, &len);
    CHECK(ioxd_json_string(&j, (ioxd_slice){ longer, sizeof longer }) && ioxd_json_done(&j));
    CHECK(len == sizeof longer + 2 && big[0] == '"' && big[len - 1] == '"'
          && memcmp(big + 1, longer, sizeof longer) == 0);

    /* A run the sink refuses whole is asked for again halved, down to 64 bytes: what is left of
     * the buffer is filled to within a short run of the end before the writer gives up. */
    char tight[700];
    j = ioxd_json_mem(tight, sizeof tight, &len);
    CHECK(!ioxd_json_string(&j, (ioxd_slice){ longer, sizeof longer }) && j.failed);
    CHECK(len >= sizeof tight - 64);

    test_json_levels();
    test_json_locale();
}

/* ioxd_configure: a zero keeps a default, a bad value is refused and changes nothing. */
/* io/spsc.h: the receive queue keeps every item through a burst past its inline slots, in order,
 * and reset hands a grown ring back. */
static void test_spsc(void)
{
    struct spsc q = { 0 };
    ioxd__spsc_reset(&q);
    CHECK(ioxd__spsc_empty(&q) && q.items == q.slots);
    for (unsigned i = 0; i < 3 * RX_QUEUE + 1; i++) {
        if (ioxd__spsc_full(&q))
            CHECK(ioxd__spsc_grow(&q));
        ioxd__spsc_push(&q)->len = i;
    }
    CHECK(ioxd__spsc_count(&q) == 3 * RX_QUEUE + 1 && q.items != q.slots && q.mask + 1 == 4 * RX_QUEUE);
    bool in_order = true;
    for (unsigned i = 0; i < 3 * RX_QUEUE + 1; i++)
        in_order = in_order && ioxd__spsc_pop(&q).len == i;
    CHECK(in_order && ioxd__spsc_empty(&q));
    for (unsigned i = 0; i < RX_QUEUE; i++)                    /* wraps within the grown ring */
        ioxd__spsc_push(&q)->len = 100 + i;
    CHECK(ioxd__spsc_count(&q) == RX_QUEUE && ioxd__spsc_pop(&q).len == 100);
    ioxd__spsc_reset(&q);
    CHECK(ioxd__spsc_empty(&q) && q.items == q.slots && q.mask == RX_QUEUE - 1);
}

static void test_config(void)
{
    CHECK(ioxd_configure(&(ioxd_config){ 0 }) == 0);
    CHECK(ioxd_configure(&(ioxd_config){ .ring_entries = 1024, .recv_buffers = 8192, .recv_buffer_size = 4096,
                                         .stack_size = 256UL * 1024, .idle_stacks = 1, .idle_connections = 1 }) == 0);
    CHECK(ioxd_configure(&(ioxd_config){ .ring_entries = 3000 }) == -1);            /* not a power of two */
    CHECK(ioxd_configure(&(ioxd_config){ .ring_entries = 65536 }) == -1);
    CHECK(ioxd_configure(&(ioxd_config){ .recv_buffers = 65536 }) == -1);           /* the kernel refuses it */
    CHECK(ioxd_configure(&(ioxd_config){ .recv_buffers = 12 }) == -1);
    CHECK(ioxd_configure(&(ioxd_config){ .recv_buffer_size = 16 }) == -1);
    CHECK(ioxd_configure(&(ioxd_config){ .stack_size = 4096 }) == -1);              /* the engine's frames alone are 44 KB */
    CHECK(ioxd_configure(&(ioxd_config){ .recv_buffers = 8, .recv_buffer_size = 64 }) == 0);   /* the starved test build */
    CHECK(ioxd_configure(&(ioxd_config){ 0 }) == 0);                                /* back to the defaults */
}

/* ioxd_delay off a worker: nothing to wait on, and no sleeping either. */
static void test_delay_off_worker(void)
{
    CHECK(ioxd_delay(1) == -1);
    CHECK(ioxd_delay_ns(1) == -1);
}

int main(void)
{
    test_integers();
    test_spsc();
    test_config();
    test_delay_off_worker();
    test_json();
    test_doubles();
    test_bools();
    test_strings();
    test_kv_parse();
    printf("unit: %d checks, %d failed\n", checks, failures);   /* test_json ran first, above */
    return failures ? 1 : 0;
}
