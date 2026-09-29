/* Cross-core mailbox / doorbell (geometry-core side). CSR peripheral.
 * See litex/mailbox.py. */
#ifndef GEOM_MAILBOX_H
#define GEOM_MAILBOX_H

#include <stdint.h>
#include "generated_csr_addrs.h"

static inline void     mmio_w(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mmio_r(uint32_t a)             { return *(volatile uint32_t *)a; }

/* Blocking wait for the game CPU's doorbell; returns the 32-bit message
 * (typically the main_ram address of the display list to process). */
static inline uint32_t mbox_wait_kick(void)
{
    while (!((mmio_r(CSR_MAILBOX_STATUS_ADDR) >> CSR_MAILBOX_STATUS_GEOM_PENDING_OFFSET) & 1u))
        ;
    uint32_t msg = mmio_r(CSR_MAILBOX_GAME_MSG_ADDR);
    mmio_w(CSR_MAILBOX_GEOM_ACK_ADDR, 1);   /* clear our pending flag */
    return msg;
}

/* Signal the game CPU that this frame's geometry is done. */
static inline void mbox_signal_done(uint32_t status)
{
    mmio_w(CSR_MAILBOX_GEOM_MSG_ADDR, status);
    mmio_w(CSR_MAILBOX_GEOM_KICK_ADDR, 1);
}

#endif /* GEOM_MAILBOX_H */
