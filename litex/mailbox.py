"""
Cross-core mailbox / doorbell between the game CPU and the geometry CPU.

Each direction has a 32-bit message word and a set/clear doorbell that drives a
level interrupt on the receiving core:

  game -> geom :  game_msg  + write game_kick  -> geom_irq high until geom
                  writes geom_ack  (typical payload: display-list base address)
  geom -> game :  geom_msg  + write geom_kick  -> game_irq high until game
                  writes game_ack  (typical payload: "frame N geometry done")

`game_irq` is wired into the SoC interrupt controller; `geom_irq` is wired to
the geometry core's externalInterruptArray[0].
"""

from migen import *

from litex.gen.fhdl.module import LiteXModule
from litex.soc.interconnect.csr import CSR, CSRStorage, CSRStatus, CSRField


class Mailbox(LiteXModule):
    def __init__(self):
        # game -> geom
        self.game_msg  = CSRStorage(32, description="Message word from game CPU to geometry CPU.")
        self.game_kick = CSR()  # write: raise geom doorbell
        self.geom_ack  = CSR()  # write (from geom): clear geom doorbell
        # geom -> game
        self.geom_msg  = CSRStorage(32, description="Message word from geometry CPU to game CPU.")
        self.geom_kick = CSR()  # write (from geom): raise game doorbell
        self.game_ack  = CSR()  # write (from game): clear game doorbell

        self.status = CSRStatus(fields=[
            CSRField("geom_pending", size=1, description="Doorbell pending towards the geometry CPU."),
            CSRField("game_pending", size=1, description="Doorbell pending towards the game CPU."),
        ])

        # Level interrupt lines (consumed in analogue_pocket.py).
        self.game_irq = Signal()
        self.geom_irq = Signal()

        geom_pending = Signal()
        game_pending = Signal()
        self.sync += [
            If(self.game_kick.re, geom_pending.eq(1)).Elif(self.geom_ack.re, geom_pending.eq(0)),
            If(self.geom_kick.re, game_pending.eq(1)).Elif(self.game_ack.re, game_pending.eq(0)),
        ]
        self.comb += [
            self.status.fields.geom_pending.eq(geom_pending),
            self.status.fields.game_pending.eq(game_pending),
            self.geom_irq.eq(geom_pending),
            self.game_irq.eq(game_pending),
        ]
