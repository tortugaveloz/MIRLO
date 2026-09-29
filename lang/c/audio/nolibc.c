/* The few libc routines GCC may call in this -nostdlib firmware. Word-wise
 * where both ends are aligned: most calls move DMEM work buffers. */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) u32a;

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3u) == 0) {
        u32a *dw = (u32a *)dp; const u32a *sw = (const u32a *)sp;
        for (; n >= 4; n -= 4) *dw++ = *sw++;
        dp = (uint8_t *)dw; sp = (const uint8_t *)sw;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    if (((uintptr_t)dp & 3u) == 0) {
        uint32_t v = (uint8_t)c * 0x01010101u;
        u32a *dw = (u32a *)dp;
        for (; n >= 4; n -= 4) *dw++ = v;
        dp = (uint8_t *)dw;
    }
    while (n--) *dp++ = (uint8_t)c;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}
