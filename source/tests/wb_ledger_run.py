#!/usr/bin/env python3
"""Sim-only Wetbrush ledger driver: ticks the streaming zone WITHOUT any
renderer, so [wb-mass] per-frame readbacks (grid/swarm/sample mass +
negative-density census) can be collected at max speed for the mass-leak
hunt. Run from Binaries/Release (app configs are CWD-resolved):

    WETBRUSH_RES=1024 WB_DEBUG_DUMP_PTCL=1 \
        python ../../source/tests/wb_ledger_run.py [frames] 2> wb_ledger.log
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(HERE))
os.environ.setdefault("WETBRUSH_RES", "1024")
os.environ.setdefault("WB_DEBUG_DUMP_PTCL", "1")

from render_wetbrush import BIN, DT, build_sim_graph  # noqa: E402

frames = int(sys.argv[1]) if len(sys.argv) > 1 else 135

sim_usd = BIN / "wetbrush_ledger_sim.usdc"
for stale in (sim_usd, sim_usd.with_name("wetbrush_ledger_sim_modifiers.usdc")):
    if stale.exists():
        stale.unlink()

print(f"[ledger] building sim graph ({frames} frames, no renderer)")
graph, stage, prim_path = build_sim_graph(sim_usd)
for i in range(frames):
    t = (i + 1) * DT
    stage.set_render_time(t)
    stage.tick(DT)
    stage.finish_tick()
    if (i + 1) % 15 == 0:
        print(f"[ledger] frame {i + 1}/{frames}")
print("[ledger] done")
