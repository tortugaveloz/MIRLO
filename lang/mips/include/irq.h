/* LiteX's libbase <irq.h> for MIRLO's MIPS game CPU: everything is polled
 * (UART_POLLING); interrupts stay off (Status.IE 0). */
#ifndef __IRQ_H
#define __IRQ_H

static inline unsigned int irq_getie(void) { return 0; }
static inline void irq_setie(unsigned int ie) { (void)ie; }
static inline unsigned int irq_getmask(void) { return 0; }
static inline void irq_setmask(unsigned int mask) { (void)mask; }
static inline unsigned int irq_pending(void) { return 0; }

#endif
