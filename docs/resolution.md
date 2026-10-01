# Resolution

## Two modes, chosen by the game file (Mirlo)

The core runs 268 x 240 (the default: fills the Pocket's screen, the N64's
320-wide frame cropped 26 px a side, 2D moved in) or the N64's own 320 x 240
(4:3, nothing cropped or moved). The game file's header picks: byte 26 of the
MIRLODATA footer, 0 = 268, 1 = 320 (`tools/make_mirlo_game.py --video 320`).
Older files read 0. There is no menu override: a game runs in the mode its
file asks for.

How it switches, at boot (lang/c/game/frame.c `frame_set_video`, called by
the program with the game file header's choice before `frame_init()`):
the scan-out's `vtg_mode` register (rtl/soc/n64_video.sv: two constant
timing sets, 340 x 280 and 360 x 264 at the same 5.712 MHz pixel clock,
~60 Hz both, which also set the frame's length), the Pocket's scaler slot
(sent on the RGB bus during blanking; video.json slot 0 = 268, slot 1 = 320),
the clears' width and the geom core's screen edges (GDL_FBSIZE).

The older, manual procedure below (a new pixel clock and timings) still
applies for any other resolution.

## Changing the resolution by hand

If you don't like the default resolution selected by the core, you can change it to anything supported by the Analogue Pocket. Remember openFPGA is limited to 800x720, so you can't choose a resolution outside of those bounds.

1. Find your desired vertical and horizontal counts. [See agg23's wiki](https://github.com/agg23/analogue-pocket-utils/wiki/Video) for additional information about how to select counts, but briefly, visit https://tomverbeure.github.io/video_timings_calculator and enter your desired resolution. It will show you the "conforms to protocol" options that you can select between, but you are not required to use one of these for the Pocket; it is just an example. If you choose an unusual resolution (like the default 266x240), there won't be a standard to base the calculations off of, so the tool will estimate what it would be.  
Note that you want all of your system clocks (CPU, and 2x for SDRAM) to be integer multiples of this clock, and to pass timing you probably need the CPU clock to be < 60MHz. For example, you could choose counts that require a 10MHz clock, which would either be a 5x or 6x multiple (50MHz or 60MHz) for the CPU clock.

2. In Quartus, open the `mf_pllbase` megafunction on the left side of the screen. This will open the tool where you can manipulate the clocks for the core. The last two clocks should be the video pixel clock that you chose, the first is your CPU clock (remember to keep it an integer multiple of the video clock), and the second and third clocks are your SDRAM clock (remember to keep it 2x the CPU clock). Enter those values and click finish.

3. Edit the clock constants: `CLOCK_HZ` in `tools/mirlo_regs.py` (then run it) and `SYS_HZ` in `target/pocket/core_top.sv`. Put your counts into `rtl/soc/n64_video.sv` (its two timing sets and the frame's word counts).

4. Update the Pocket `video.json` to your new resolution. Without it, the Pocket won't know what resolution you want to display.

5. Rebuild the firmware that has the clock compiled in (see the top-level README's build steps).

6. Build the Quartus project by pressing "Build".

7. Reverse the produced bitstream (`*.rbf`). See https://www.analogue.co/developer/docs/packaging-a-core#fpga-bitstream

8. Update your clock speed constants in your software code, if any.

9. The core should now be ready to use at the new resolution.
