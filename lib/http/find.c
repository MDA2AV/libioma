#include "http/find.h"
#include "ioxd/slice.h"

#include <string.h>

ptrdiff_t ioxd__find_scalar(const char *hay, size_t len, const char *needle, size_t n)
{
    const char *end  = hay + (len - n + 1);
    const char  last = needle[n - 1];
    for (const char *at = hay; (at = memchr(at, needle[0], (size_t)(end - at))) != nullptr; at++)
        if (at[n - 1] == last && memcmp(at + 1, needle + 1, n - 2) == 0)
            return at - hay;
    return -1;
}

#ifdef __x86_64__

typedef char v16 __attribute__((vector_size(16)));
typedef char v32 __attribute__((vector_size(32)));

static inline v16 load16(const char *p)
{
    v16 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static inline unsigned mask16(v16 v)
{
    return (unsigned)__builtin_ia32_pmovmskb128(v);
}

static inline ptrdiff_t block16(const char *hay, size_t i, const char *needle, size_t n, v16 first, v16 last)
{
    v16      a = load16(hay + i);
    v16      b = load16(hay + i + n - 1);
    unsigned m = mask16((v16)(a == first) & (v16)(b == last));
    for (; m; m &= m - 1) {
        unsigned j = (unsigned)__builtin_ctz(m);
        if (memcmp(hay + i + j + 1, needle + 1, n - 2) == 0)
            return (ptrdiff_t)(i + j);
    }
    return -1;
}

ptrdiff_t ioxd__find_sse2(const char *hay, size_t len, const char *needle, size_t n)
{
    const v16    first = (v16){} + needle[0];
    const size_t span  = len - n + 1;
    if (span >= 16) {
        const v16 last = (v16){} + needle[n - 1];
        for (size_t i = 0;;) {
            ptrdiff_t at = block16(hay, i, needle, n, first, last);
            if (at >= 0)
                return at;
            if (i + 16 == span)
                return -1;
            i += 16;
            if (i + 16 > span)
                i = span - 16;
        }
    }
    if (len >= 16) {
        unsigned m = mask16((v16)(load16(hay) == first)) & ((1U << span) - 1);
        for (; m; m &= m - 1) {
            unsigned j = (unsigned)__builtin_ctz(m);
            if (memcmp(hay + j + 1, needle + 1, n - 1) == 0)
                return (ptrdiff_t)j;
        }
        return -1;
    }
    return ioxd__find_scalar(hay, len, needle, n);
}

__attribute__((target("avx2")))
static inline v32 load32(const char *p)
{
    v32 v;
    memcpy(&v, p, sizeof v);
    return v;
}

__attribute__((target("avx2")))
ptrdiff_t ioxd__find_avx2(const char *hay, size_t len, const char *needle, size_t n)
{
    const size_t span = len - n + 1;
    if (span < 32)
        return ioxd__find_sse2(hay, len, needle, n);
    const v32 first = (v32){} + needle[0];
    const v32 last  = (v32){} + needle[n - 1];
    for (size_t i = 0;;) {
        v32      a = load32(hay + i);
        v32      b = load32(hay + i + n - 1);
        unsigned m = (unsigned)__builtin_ia32_pmovmskb256((v32)(a == first) & (v32)(b == last));
        for (; m; m &= m - 1) {
            unsigned j = (unsigned)__builtin_ctz(m);
            if (memcmp(hay + i + j + 1, needle + 1, n - 2) == 0)
                return (ptrdiff_t)(i + j);
        }
        if (i + 32 == span)
            return -1;
        i += 32;
        if (i + 32 > span)
            i = span - 32;
    }
}

static ptrdiff_t find(const char *hay, size_t len, const char *needle, size_t n)
{
    if (__builtin_cpu_supports("avx2"))
        return ioxd__find_avx2(hay, len, needle, n);
    return ioxd__find_sse2(hay, len, needle, n);
}

#else

static ptrdiff_t find(const char *hay, size_t len, const char *needle, size_t n)
{
    return ioxd__find_scalar(hay, len, needle, n);
}

#endif

ptrdiff_t ioxd_slice_find_bytes(ioxd_slice s, const void *needle, size_t n)
{
    if (n == 0)
        return 0;
    if (n > s.len)
        return -1;
    if (n == 1)
        return ioxd_slice_find_char(s, *(const char *)needle);
    return find(s.p, s.len, needle, n);
}

ptrdiff_t ioxd_slice_find(ioxd_slice s, const char *needle)
{
    return ioxd_slice_find_bytes(s, needle, strlen(needle));
}

ptrdiff_t ioxd_slice_find_char(ioxd_slice s, char c)
{
    if (s.len == 0)
        return -1;
    const char *at = memchr(s.p, c, s.len);
    return at ? at - s.p : -1;
}

ptrdiff_t ioxd_slice_rfind_char(ioxd_slice s, char c)
{
    if (s.len == 0)
        return -1;
    const char *at = memrchr(s.p, c, s.len);
    return at ? at - s.p : -1;
}
