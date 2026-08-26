#!/usr/bin/env python3
"""
Sim-only probe for the blob test — NO renderer, NO PNGs. Ticks the press
phase (~30 frames) with WB_DEBUG_DUMP_PTCL=1 so the commit node prints
per-frame swarm kinematics (count/centroid/bbox/vmean/vmax) + window grid
velocity stats. Used to locate the frame-11/12 upward "explosion" source.

Light by design: run at WETBRUSH_RES=512 (few hundred MB VRAM).

    WB_DEBUG_DUMP_PTCL=1 WETBRUSH_RES=512 python ../../source/tests/blob_sim_probe.py
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))

import render_wetbrush_blob as blob  # noqa: E402


def main():
    sim_usd = BIN / "wetbrush_blob_probe_sim.usdc"
    for stale in (sim_usd,
                  BIN / "wetbrush_blob_probe_sim_modifiers.usdc"):
        if stale.exists():
            stale.unlink()
    # 30 frames covers the press phase (0-17) + the explosion onset (11-16).
    blob.NUM_FRAMES = 30
    sim_graph, stage, prim_path = blob.build_sim_graph(sim_usd)
    dt = 1.0 / 60.0
    for i in range(blob.NUM_FRAMES):
        t = (i + 1) * dt
        stage.set_render_time(t)
        stage.tick(dt)
        stage.finish_tick()
        print(f"[probe] frame {i} (t={t:.4f}) ticked", flush=True)
    print("[probe] done")


if __name__ == "__main__":
    main()
