/* MIRLO's boot ROM on the MIPS SoC, after start.S: the game the Pocket
 * loaded, or LiteX's serial boot (SFL) over the JTAG UART -- the protocol
 * litex/litex_term.py and litex/jtag_run.py speak (LiteX's bios/boot.c). */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>

#define MAIN_RAM 0x40000000u

static void putc_(char c)
{
    /* bounded: with no host reading the JTAG UART, TXFULL never clears */
    for (unsigned i = 0; i < 400000u && uart_txfull_read(); i++);
    if (!uart_txfull_read()) uart_rxtx_write((uint8_t)c);
}
static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }

static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }

static void invalidate_caches(void)
{
    for (uint32_t a = MAIN_RAM; a < MAIN_RAM + 16384u; a += 32) __asm__ volatile(".set push\n.set mips3\ncache 0x00, 0(%0)\n.set pop" :: "r"(a));
    for (uint32_t a = MAIN_RAM; a < MAIN_RAM + 8192u; a += 32) __asm__ volatile(".set push\n.set mips3\ncache 0x01, 0(%0)\n.set pop" :: "r"(a));
}

__attribute__((noreturn)) static void jump(uint32_t addr)
{
    puts_("Executing booted program at 0x"); puthex(addr); puts_("\n");
    invalidate_caches();
    ((void (*)(void))addr)();
    for (;;);
}

void boot_exception(uint32_t cause, uint32_t epc, uint32_t badvaddr)
{
    puts_("\nboot ROM: exception, cause "); puthex(cause); puts_(" epc "); puthex(epc);
    puts_(" badvaddr "); puthex(badvaddr); puts_("\n");
    for (;;);
}

/* ---- SFL (LiteX bios/sfl.h) */
#define SFL_MAGIC_REQ "sL5DdSMmkekro\n"
#define SFL_MAGIC_ACK "z6IHG7cYDID6o\n"
#define SFL_CMD_ABORT 0x00
#define SFL_CMD_LOAD  0x01
#define SFL_CMD_JUMP  0x02
#define SFL_ACK_SUCCESS  'K'
#define SFL_ACK_CRCERROR 'C'
#define SFL_ACK_UNKNOWN  'U'
#define SFL_ACK_ERROR    'E'

static unsigned short crc16(const unsigned char *p, int len)
{
    unsigned short crc = 0;
    while (len-- > 0) {
        crc ^= (unsigned short)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (unsigned short)(crc << 1 ^ 0x1021) : (unsigned short)(crc << 1);
    }
    return crc;
}
static int rx_ready(void) { return !uart_rxempty_read(); }
static unsigned char rx(void) { return (unsigned char)uart_rxtx_read(); }
static void tx_raw(char c) { putc_(c); }

/* the magic answered within `cycles`? */
static int check_ack(uint32_t cycles)
{
    static const char str[] = SFL_MAGIC_ACK;
    int rec = 0;
    uint32_t t0 = now();
    while (now() - t0 < cycles) {
        if (!rx_ready()) continue;
        char c = (char)rx();
        if (c == str[rec]) { if (++rec == 14) return 1; }
        else rec = c == str[0];
    }
    return 0;
}

static uint32_t be32(const unsigned char *d) { return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]; }

static void serialboot(void)
{
    puts_("Booting from serial...\n");
    for (;;) {                                      /* until a host answers */
        for (const char *c = SFL_MAGIC_REQ; *c; c++) tx_raw(*c);
        if (check_ack(CONFIG_CLOCK_FREQUENCY / 2)) break;
    }
    struct { unsigned char len, crc[2], cmd, payload[255]; } f;
    for (;;) {
        int i = 0, timeout = 1;
        uint32_t t0 = 0;
        while (i == 0 || now() - t0 < 10u * CONFIG_CLOCK_FREQUENCY) {
            if (!rx_ready()) continue;
            unsigned char b = rx();
            if (i == 0) { t0 = now(); f.len = b; }
            else if (i == 1) f.crc[0] = b;
            else if (i == 2) f.crc[1] = b;
            else if (i == 3) f.cmd = b;
            else f.payload[i - 4] = b;
            i++;
            if (i >= 4 && i == f.len + 4) { timeout = 0; break; }
        }
        if (timeout) { tx_raw(SFL_ACK_ERROR); continue; }
        if (crc16(&f.cmd, f.len + 1) != (unsigned short)(f.crc[0] << 8 | f.crc[1])) { tx_raw(SFL_ACK_CRCERROR); continue; }
        switch (f.cmd) {
        case SFL_CMD_ABORT:
            tx_raw(SFL_ACK_SUCCESS);
            return;
        case SFL_CMD_LOAD: {
            unsigned char *dst = (unsigned char *)be32(f.payload);
            for (int k = 0; k < f.len - 4; k++) dst[k] = f.payload[4 + k];
            tx_raw(SFL_ACK_SUCCESS);
            break;
        }
        case SFL_CMD_JUMP:
            tx_raw(SFL_ACK_SUCCESS);
            jump(be32(f.payload));
        default:
            tx_raw(SFL_ACK_UNKNOWN);
            break;
        }
    }
}

void boot_main(int loaded)
{
    puts_("\nMIRLO (MIPS) boot ROM\n");
    if (loaded) {
        puts_("Booting slot 0 (loaded by the Pocket)\n");
        jump(MAIN_RAM);
    }
    puts_("No game loaded by the Pocket\n");
    for (;;) serialboot();
}
