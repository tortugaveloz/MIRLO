/* Bounded-latency logging for code that runs inside the frame loop.
 *
 * Why this exists: libbase's putchar() spins on uart_txfull() with no bound,
 * so the CPU stalls for as long as the host takes to drain the UART. Over the
 * JTAG UART that is a very long time -- jtag_uart_relay.py moves ~24 bytes per
 * TCL-RPC round trip, and a single ~150-byte log line was measured stalling
 * the render loop for 358 ms. On screen that is a sub-second freeze every time
 * a periodic log line fires, which is exactly what it looked like: "it freezes
 * every few seconds for no apparent reason". With no host draining the UART at
 * all it would block forever.
 *
 * log_printf() queues the line in a RAM ring instead, and log_pump() -- called
 * once per frame -- pushes out only as many bytes as the UART will take
 * without waiting. Neither ever spins, so logging cannot stall a frame; text
 * is spread over subsequent frames instead of lost. Writing "as much as fits,
 * then give up" does not work here: the UART FIFO holds about 16 bytes, so
 * that truncates every line to ~21 characters, and a truncated line is worse
 * than a missing one because it silently reads as data.
 *
 * A blocking logger also perturbs the very timing it is there to report: an
 * earlier version that printed every frame over 100 ms made every frame take
 * 455-492 ms instead of the real 26 ms.
 *
 * Use plain printf() for anything outside the frame loop (banners, test
 * results), where blocking is fine and losing text is not.
 */
#ifndef GAME_LOG_H
#define GAME_LOG_H

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Drain queued log text into the UART. Call once per frame. Never blocks. */
void log_pump(void);

/* Characters dropped so far because the ring was full. Non-zero means some log
 * output is missing -- report it, do not silently trust the log to be
 * complete. */
extern unsigned long log_dropped;

/* Stall watchdog (diagnostic): the CLINT machine timer interrupts the CPU if
 * watchdog_kick() is not called again within `cycles`, and trap_handler()
 * prints where the CPU was (mepc), the last wd_mark() characters and the bus
 * error count, then halts. Records nothing on the UART while things run, so
 * it does not change the timing it is watching. If the CPU is frozen on a bus
 * access that never completes it cannot take the interrupt, and the watchdog
 * stays silent -- which is the answer to that question too. */
void watchdog_start(unsigned long cycles);
void watchdog_kick(void);
void watchdog_stop(void);

/* Sampling PC profiler (diagnostic, shares the machine timer with the
 * watchdog -- use one or the other). Every `period` cycles the timer
 * interrupt records mepc into a histogram of the program's code in 16-byte
 * buckets; prof_dump() prints the `topn` busiest buckets as
 * "PROF <addr> <samples>" (plain printf: call it outside timed code) for
 * the host to fold per function against the ELF. */
void prof_start(unsigned long period);
void prof_stop(void);
void prof_dump(unsigned topn);
/* sampling on someone else's timer interrupts (log.c) */
void prof_passive(int on);
void prof_passive_reset(void);
unsigned long prof_passive_samples(void);
void prof_dump_log(unsigned topn);
extern volatile char     wd_marks[64];
extern volatile unsigned wd_mark_pos;
static inline void wd_mark(char c) { wd_marks[wd_mark_pos++ & 63u] = c; }

#endif
