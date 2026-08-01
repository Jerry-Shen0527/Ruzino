#!/usr/bin/env python3
"""
DIAGNOSTIC: run the single-blue-stroke sim WITHOUT rendering, dumping
commit-node outputs per frame (Total Density / Total Color / Particle Count)
to see whether density mass is conserved across the stroke and whether the
particle pool saturates.

Run from Binaries/Release:

    python ../../source/tests/diag_blue_sim.py
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(HERE))

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")

from pxr import Usd, UsdGeom, Sdf  # noqa: E402

import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

from render_wetbrush_blue import build_sim_graph_blue  # noqa: E402

NUM_FRAMES = 60
DT = 1.0 / 60.0


def main():
    sim_usd = BIN / "wetbrush_blue_diag.usdc"
    sim_usd.parent.mkdir(parents=True, exist_ok=True)
    g, stage, prim_path = build_sim_graph_blue(sim_usd)

    commit = g.getNode("Commit")
    print("frame  total_density  tot_b  particles  mean_div")


    def fout(sock):
        r = g.getOutput(commit, sock)
        if isinstance(r, float):
            return r
        try:
            return r.cast_float()
        except Exception:
            return float(r)


    def iout(sock):
        r = g.getOutput(commit, sock)
        try:
            return r.cast_int()
        except Exception:
            return int(r)

    for i in range(NUM_FRAMES):
        t = (i + 1) * DT
        stage.set_render_time(t)
        stage.tick(DT)
        stage.finish_tick()


if __name__ == "__main__":
    main()
