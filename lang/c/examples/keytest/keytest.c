/* Prints the Pocket's controller key/trigger words over the UART whenever
 * one changes: which bit a Controls (input.json) entry really reaches the
 * core on, e.g. after the user remaps it. Upload over JTAG
 * (litex/jtag_program_and_run.sh). */
#include <stdio.h>
#include <stdint.h>

#include <generated/csr.h>

int main(void)
{
	uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
	printf("keytest: cont1..4 key words (bit0 up 1 down 2 left 3 right 4 A 5 B 6 X 7 Y 8 L1 9 R1 10 L2 11 R2 12 L3 13 R3 14 select 15 start)\n");
	for (;;) {
		uint32_t k[4] = {apf_input_cont1_key_read(), apf_input_cont2_key_read(),
		                 apf_input_cont3_key_read(), apf_input_cont4_key_read()};
		for (int i = 0; i < 4; i++)
			if (k[i] != last[i]) {
				printf("cont%d key=%08lx\n", i + 1, (unsigned long)k[i]);
				last[i] = k[i];
			}
	}
}
