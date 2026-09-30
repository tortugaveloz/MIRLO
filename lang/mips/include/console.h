/* LiteX's libbase <console.h>: the console is the UART. */
#ifndef __CONSOLE_H
#define __CONSOLE_H
#include <uart.h>
static inline int readchar_nonblock(void) { return uart_read_nonblock(); }
static inline char readchar(void) { return uart_read(); }
#endif
