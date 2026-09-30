/* MIRLO's MIPS runtime: what LiteX's libbase gave the RISC-V programs --
 * the UART, delays, and the console under picolibc's stdio. */
#include <stdio.h>
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
#include <uart.h>

/* The UART is JTAG-only, and with no host reading it TXFULL never clears:
 * wait a bounded time (~0.5 s, the relay moves ~24 bytes per round trip),
 * then drop output until there is room again (MIRLO fc98de2). */
#define UART_TX_STALL_SPINS 4000000u

void uart_init(void) {}
void uart_isr(void) {}

void uart_write(char c)
{
    static int tx_stalled;
    if (uart_txfull_read()) {
        if (tx_stalled)
            return;
        for (unsigned int i = 0; i < UART_TX_STALL_SPINS && uart_txfull_read(); i++);
        if (uart_txfull_read()) {
            tx_stalled = 1;
            return;
        }
    }
    tx_stalled = 0;
    uart_rxtx_write((uint8_t)c);
}

int uart_read_nonblock(void) { return !uart_rxempty_read(); }

char uart_read(void)
{
    while (uart_rxempty_read());
    return (char)uart_rxtx_read();
}

void uart_sync(void)
{
    for (unsigned int i = 0; i < UART_TX_STALL_SPINS && uart_txfull_read(); i++);
}

static uint64_t uptime(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }

void busy_wait_us(unsigned int us)
{
    uint64_t end = uptime() + (uint64_t)us * (CONFIG_CLOCK_FREQUENCY / 1000000u);
    while (uptime() < end);
}
void busy_wait(unsigned int ms) { busy_wait_us(ms * 1000u); }

/* picolibc's stdio: stdin/stdout/stderr on the UART (LiteX's libc/stdio.c:
 * '\n' goes out as "\n\r") */
static int mirlo_putc(char c, FILE *f)
{
    (void)f;
    uart_write(c);
    if (c == '\n') uart_write('\r');
    return (unsigned char)c;
}
static int mirlo_getc(FILE *f) { (void)f; return (unsigned char)uart_read(); }
static FILE __stdio = FDEV_SETUP_STREAM(mirlo_putc, mirlo_getc, NULL, _FDEV_SETUP_RW);
FILE *const stdout = &__stdio;
FILE *const stderr = &__stdio;
FILE *const stdin  = &__stdio;
