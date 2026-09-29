/* The geom core's MRDP transport (docs/mrdp.md): command words into MRDP's
 * FIFO over the cmd_data CSR, and the texture staging ring. */
#include <stdint.h>
#include "generated_csr_addrs.h"
#include "geom_mrdp.h"
#include "geom_vpar.h"

#define MRDP_FIFO_DEPTH (1u << (CSR_MRDP_CMD_STATUS_LEVEL_SIZE - 1))

static inline uint32_t mrdp_level(void)
{
    return (*(volatile uint32_t *)CSR_MRDP_CMD_STATUS_ADDR >> CSR_MRDP_CMD_STATUS_LEVEL_OFFSET)
         & ((1u << CSR_MRDP_CMD_STATUS_LEVEL_SIZE) - 1u);
}

#ifdef GEOM_MRDP_CSR_OUT
/* Free FIFO words as last seen. The geom core is the only producer while a
 * display list runs, but the game CPU pushed before it started (frame_init),
 * so the estimate is refreshed per list (geom_mrdp_hw_list_start) and
 * whenever it runs out. A CSR read waits for this core's earlier CSR stores,
 * so the level it returns counts every word written so far. A write to a
 * full FIFO would be lost (and counted in cmd_dropped): never estimate high. */
static uint32_t s_free;

void geom_mrdp_hw_list_start(void) { s_free = 0; }

void geom_mrdp_out(const uint32_t *w, unsigned n)
{
    volatile uint32_t *port = (volatile uint32_t *)CSR_MRDP_CMD_DATA_ADDR;
    for (unsigned i = 0; i < n; i++) {
        while (s_free == 0) s_free = MRDP_FIFO_DEPTH - mrdp_level();
        *port = w[i];
        s_free--;
    }
}
#else
/* The CFU's output buffer and PUSH (GeomSetupUnit 0x63 APPEND2 / 0x61)
 * straight into MRDP's command FIFO (litex/mrdp.py su_*): a CSR store per
 * word crossed the SoC bus, ~34 cycles a word and 10 % of a game frame. PUSH
 * stalls while the FIFO is full, so nothing is dropped and no level is
 * polled. Every word this core sends goes this way, so none can overtake
 * another (a mix of CSR stores and PUSHes could reorder the stream).
 * out[56..61] are PROJECT's: at most 48 words a PUSH. */
#define MRDP_PUSH_MAX 48u
void geom_mrdp_hw_list_start(void) { }

void geom_mrdp_out(const uint32_t *w, unsigned n)
{
    while (n) {
        unsigned k = n < MRDP_PUSH_MAX ? n : MRDP_PUSH_MAX;
        for (unsigned i = 0; i < k; i += 2) geom_setup_append2(w[i], i + 1 < k ? w[i + 1] : 0u);
        geom_setup_push(k);                     /* answers once the last word is taken */
        w += k; n -= k;
    }
}
#endif

/* Stores this core made to SDRAM (a decoded texture) complete before any
 * later command reaches MRDP: a CSR read leaves through the same bus master,
 * behind them. (An SDRAM read-back could be answered by the D-cache.) */
void mrdp_store_barrier(void) { (void)mrdp_level(); }

/* LOAD TILEs: every one this core issues is counted (mrdp_load_issued(),
 * from mrdp_texture_bind()), cached textures' too, since the hardware's
 * load_count counts them all. The staging ring's slot k may be rewritten once
 * the LOAD that read it has run: load_count >= the number it was issued as. */
static uint32_t s_loads_issued, s_slot_load[MRDP_TEX_SLOTS], s_next;
static int s_ring_init;

static inline uint32_t mrdp_load_count(void) { return *(volatile uint32_t *)CSR_MRDP_LOAD_COUNT_ADDR; }

static void mrdp_ring_init(void)
{
    if (s_ring_init) return;
    s_loads_issued = mrdp_load_count();
    for (unsigned k = 0; k < MRDP_TEX_SLOTS; k++) s_slot_load[k] = s_loads_issued;
    s_ring_init = 1;
}

void mrdp_load_issued(void) { mrdp_ring_init(); s_loads_issued++; }

void mrdp_loads_wait(void)
{
    mrdp_ring_init();
    while ((int32_t)(mrdp_load_count() - s_loads_issued) < 0) { }
}

uint32_t mrdp_tex_slot(void)
{
    mrdp_ring_init();
    uint32_t k = s_next++ % MRDP_TEX_SLOTS;
    while ((int32_t)(mrdp_load_count() - s_slot_load[k]) < 0) { }
    s_slot_load[k] = s_loads_issued + 1u;       /* the bind's LOAD, issued next */
    return MRDP_TEX_STAGING + k * (MRDP_TEX_SLOT_TEXELS * 2u);
}
