#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <irq.h>
// #include <libbase/uart.h>
// #include <libbase/console.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <generated/mem.h>

int main(void)
{
	// Diagnostic: disable interrupts (clear mstatus.MIE) before anything
	// else, to test whether an early timer/IRQ trap (with a broken or
	// unset handler on the current bitstream) is what halts execution
	// partway through the framebuffer fill below.
	asm volatile ("csrci mstatus, 8");

	// Diagnostic: paint the framebuffer solid green immediately, to check
	// via HDMI whether this minimal deferload boot.bin actually executes
	// on the current bitstream, independent of console/JTAG UART.
	{
		unsigned short *fb = (unsigned short *)VIDEO_FRAMEBUFFER_BASE;
		for (int i = 0; i < MAX_DISPLAY_WIDTH * MAX_DISPLAY_HEIGHT; i++) {
			fb[i] = 0x07E0; // green
		}
	}

	printf("C: Hello, world!\n");

	char *alloced_pointer = (char *)malloc(18 * sizeof(char));

	strcpy(alloced_pointer, "helloworld");
	printf("Pointer value: %p\n", alloced_pointer);
	printf("Pointer address: %p\n", &alloced_pointer);
	printf("String: %s\n", alloced_pointer);

	while (1) {}

	return 0;
}
