module audio (
    input wire clk_74b,
    input wire clk_sys,

    input wire reset,

    input wire [31:0] audio_bus_in,
    input wire audio_bus_wr,

    input wire audio_playback_en,
    input wire audio_flush,

    output wire [11:0] audio_buffer_fill,

    output wire audio_mclk,  //! Serial Master Clock
    output wire audio_lrck,  //! Left/Right clock
    output wire audio_dac    //! Serialized data
);

  ////////////////////////////////////////////////////////////////////////////////////////
  // Audio PLL

  wire audio_sclk;

  mf_audio_pll audio_pll (
      .refclk  (clk_74b),
      .rst     (0),
      .outclk_0(audio_mclk),
      .outclk_1(audio_sclk)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // FIFO

  wire audio_playback_en_s;

  synch_3 settings_synch (
      audio_playback_en,
      audio_playback_en_s,
      audio_mclk
  );

  wire [15:0] audio_l;
  wire [15:0] audio_r;

  wire empty;

  // The FIFO's aclr is asynchronous, so it must come straight from a
  // register: any combinational logic in front of it clears the FIFO on a
  // glitch. `reset || audio_flush` looked registered (both are), but Quartus
  // optimised LiteX's buffer_flush_re away and fed aclr from a LUT over the
  // CSR bus address and we: every CSR write at offset 0x008 of ANY bank
  // (the geom core's mailbox geom_ack, once per display list) emptied the
  // FIFO -- ~4-9 % silence in game, depending on the build. preserve +
  // dont_merge keep this one (and
  // register retiming is off in the .qsf).
  (* preserve, dont_merge *)
  reg fifo_aclr = 1'b1;
  always @(posedge clk_sys) fifo_aclr <= reset || audio_flush;

  // 1024 deep (was 4096): an audio program keeps a few hundred samples
  // queued, and the 12 M10Ks it frees pay for
  // the game CPU's caches. wrusedw cannot show "full" at 10 bits; the queue
  // never gets there.
  wire [9:0] audio_fill10;
  assign audio_buffer_fill = {2'b00, audio_fill10};
  dcfifo dcfifo_component (
      .wrclk(clk_sys),
      .rdclk(audio_mclk),

      .data (audio_bus_in),
      .wrreq(audio_bus_wr),

      .q({audio_l, audio_r}),
      .rdreq(audio_req && audio_playback_en_s),

      .rdempty(empty),
      .wrusedw(audio_fill10),

      .aclr(fifo_aclr)
      // .eccstatus(),
      // .rdfull(),
      // .rdusedw(),
      // .wrempty(),
      // .wrfull()
  );
  defparam dcfifo_component.intended_device_family = "Cyclone V",
      dcfifo_component.lpm_numwords = 1024, dcfifo_component.lpm_showahead = "OFF",
      dcfifo_component.lpm_type = "dcfifo", dcfifo_component.lpm_width = 32,
      dcfifo_component.lpm_widthu = 10, dcfifo_component.overflow_checking = "ON",
      dcfifo_component.rdsync_delaypipe = 5, dcfifo_component.underflow_checking = "ON",
      dcfifo_component.use_eab = "ON", dcfifo_component.wrsync_delaypipe = 5;

  reg audio_req = 0;

  reg [7:0] mclk_div = 8'hFF;

  always @(posedge audio_mclk) begin
    // MClk is 12.288 MHz, we want 48kHz
    audio_req <= 0;

    if (mclk_div > 0) begin
      mclk_div <= mclk_div - 8'h1;
    end else begin
      mclk_div  <= 8'hFF;

      audio_req <= 1;
    end
  end

  ////////////////////////////////////////////////////////////////////////////////////////
  // i2s Generation

  sound_i2s #(
      .SIGNED_INPUT(1)
  ) sound_i2s (
      .audio_sclk(audio_sclk),

      .audio_l(audio_playback_en_s && ~empty ? audio_l : 16'h0),
      .audio_r(audio_playback_en_s && ~empty ? audio_r : 16'h0),

      .audio_lrck(audio_lrck),
      .audio_dac (audio_dac)
  );


endmodule
