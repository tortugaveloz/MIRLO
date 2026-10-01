/* GDL_F3D (geom_gdl.h): the geom core translates an F3DEX2 display list
 * itself, as the N64's RSP did, instead of the game CPU doing it.
 *
 * Linked when the firmware is built with a translator (make F3D_EMIT=<its
 * f3d_emit.c>): the translator (f3d_emit.h's interface) works in fixed point
 * only, so nothing here needs a float. Its translation cache keeps the
 * translations of the static lists (the game's read-only data, a range the
 * task gives) in an arena the task gives too; its big tables live in SDRAM
 * (.sdram_bss, geom_mips.ld), not in the core's 16 KiB of RAM.
 *
 * The task's words (gdl_build.h gdl_f3d_end()):
 *   [0] the list's root  [1] the frame's clear-colour word (0: none)
 *   [2] where the translation goes  [3] its room, in words
 *   [4] [5] the static range [lo, hi)  [6] [7] the cache's arena, its bytes
 * The translation ends in GDL_ENDDL; the walk calls it like a GDL_DL. */
#include <stdint.h>
#include <string.h>
#include "geom_gdl.h"
#include "gdl_build.h"
#include "f3d_emit.h"

static uintptr_t s_static_lo, s_static_hi, s_arena;
static uint32_t  s_arena_bytes;

int f3d_dlc_static(const void *p) { return (uintptr_t)p >= s_static_lo && (uintptr_t)p < s_static_hi; }
void *f3d_dlc_arena(uint32_t *bytes) { *bytes = s_arena_bytes; return (void *)s_arena; }

/* the translator's diagnostics have nowhere to go on this core */
int printf(const char *fmt, ...) { (void)fmt; return 0; }

extern char __sdram_bss_start[], __sdram_bss_end[];

const uint32_t *geom_f3d_task(const uint32_t *a, uint32_t n)
{
    static int s_init;
    if (n < GDL_F3D_WORDS || a[3] < 64u) return 0;
    if (!s_init) {                            /* the SDRAM tables start zeroed, as .bss */
        memset(__sdram_bss_start, 0, (size_t)(__sdram_bss_end - __sdram_bss_start));
        s_init = 1;
    }
    s_static_lo = a[4]; s_static_hi = a[5];
    s_arena = a[6]; s_arena_bytes = a[7];
    uint32_t *out = (uint32_t *)(uintptr_t)a[2];
    gdl_cur_t c;
    gdl_begin(&c, out, a[3] - 1u);            /* (a word kept for the GDL_ENDDL) */
    f3d_emit_reset();
    f3d_emit_display_list((const f3d_word_t *)(uintptr_t)a[0], &c);
    if (c.p > c.end) c.p = c.end;             /* full: cut, but always end */
    *c.p = GDL_HDR(GDL_ENDDL, 0);
    /* the list painted a background colour: this frame's clear uses it */
    uint32_t *cw = (uint32_t *)(uintptr_t)a[1];
    if (cw && f3d_clear_valid) {
        uint32_t rgb = f3d_clear_rgb;
        uint32_t c565 = ((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x07E0u) | ((rgb >> 3) & 0x001Fu);
        *cw = c565 << 16 | c565;
    }
    return out;
}
