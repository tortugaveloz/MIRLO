/* Video scanout diagnostic -- no rasterizer, no geom core, no rendering.
 *
 * Fills the three framebuffers with three DISTINCT solid colors once, then
 * cycles video_framebuffer_dma_base between them exactly the way frame.c's
 * frame_submit() does (wait for vblank, then write the new base). Nothing
 * ever clears or re-renders a buffer after the initial fill.
 *
 * Purpose: decide where intermittent black frames come from.
 *   - If the display only ever shows the three colors -> the flip/scanout
 *     path is sound, and black frames must come from the render/clear side
 *     (a clear of a buffer that is still being scanned out).
 *   - If black frames appear here -> the flip/scanout path itself is broken
 *     (DMA base latching, buffer sync, or FIFO underflow), independent of
 *     any rendering.
 */
#include <stdint.h>
#include <stdio.h>
#include <generated/csr.h>
#include <generated/mem.h>
#include <generated/soc.h>

#define NUM_FB 3
static const uint32_t s_fb[NUM_FB] = {
    VIDEO_FRAMEBUFFER_BASE + 0u * 0x00040000u,
    VIDEO_FRAMEBUFFER_BASE + 1u * 0x00040000u,
    VIDEO_FRAMEBUFFER_BASE + 2u * 0x00040000u,
};

/* RGB565: red, green, blue -- all far from black so any black pixel on
 * screen is unambiguously a fault, not content. */
static const uint16_t s_col[NUM_FB] = { 0xF800, 0x07E0, 0x001F };

static int wait_vblank_bounded(void) {
    uint32_t n = 0;
    while (!apf_video_video_vblank_triggered_read()) {
        if (++n == 4000000u) return 0;
    }
    return 1;
}

int main(void)
{
    printf("=== videotest: 3-buffer flip cycling, no rendering ===\n");

    uint32_t npix = (uint32_t)VIDEO_FRAMEBUFFER_HRES * (uint32_t)VIDEO_FRAMEBUFFER_VRES;
    for (int b = 0; b < NUM_FB; b++) {
        volatile uint16_t *fb = (volatile uint16_t *)s_fb[b];
        for (uint32_t i = 0; i < npix; i++) fb[i] = s_col[b];
        printf("buf %d @0x%08x filled with 0x%04x\n",
               b, (unsigned)s_fb[b], (unsigned)s_col[b]);
    }

    video_framebuffer_dma_base_write(s_fb[0]);
    printf("cycling flips now; screen must ALWAYS be red, green or blue\n");

    uint32_t i = 0, vto = 0, n = 0;
    for (;;) {
        int vok = wait_vblank_bounded();
        if (!vok) vto++;
        video_framebuffer_dma_base_write(s_fb[i]);
        i = (i + 1) % NUM_FB;
        if ((++n & 0xFF) == 0)
            printf("flips=%lu vTO=%lu frame_cnt=%lu dma_off=%lu\n",
                   (unsigned long)n, (unsigned long)vto,
                   (unsigned long)apf_video_video_frame_counter_read(),
                   (unsigned long)video_framebuffer_dma_offset_read());
    }
    return 0;
}
