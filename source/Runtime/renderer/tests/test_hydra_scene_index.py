#!/usr/bin/env python3
"""Hydra 2.0 scene index chain liveness tests.

hd_RUZINO_SceneIndexPlugin appends Hd_RUZINO_SimSceneIndex to the
renderer-specific part of the scene index chain (keyed on the display name
"RUZINO Renderer"). These tests prove the chain link is actually inserted
and receives prim notices when the offline HydraRenderer runs, by running a
one-frame render in a subprocess with HD_RUZINO_SIM_SCENE_INDEX_DEBUG=1 and
checking the pass-through observer's log output. The render subprocess
itself lives in hydra2_ab_common (shared with the ingest smoke tests).
"""

import os
from pathlib import Path

import pytest

import hydra2_ab_common as ab


def test_sim_scene_index_on_chain():
    """The RUZINO renderer-specific scene index is appended and observes prims."""
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    scene = Path(__file__).parent / "test_scene.usda"
    if not scene.exists():
        pytest.skip("No test USD scene found")

    env = os.environ.copy()
    env["HD_RUZINO_SIM_SCENE_INDEX_DEBUG"] = "1"
    result = ab.run_render_subprocess(binary_dir, scene, env=env)

    combined = result.stdout + result.stderr
    assert "RENDER_DONE" in combined, (
        f"render did not complete. stdout/stderr tail:\n{combined[-2000:]}")

    # The scene index announces itself when the engine assembles the chain
    # for the "RUZINO Renderer" display name.
    assert "[SimSceneIndex] created" in combined, (
        f"SimSceneIndex was not appended to the chain. "
        f"stdout/stderr tail:\n{combined[-2000:]}"
    )

    # And prim notices actually flow through it during population/render.
    assert "[SimSceneIndex] +" in combined, (
        f"No PrimsAdded notices observed. stdout/stderr tail:\n{combined[-2000:]}"
    )


def test_sim_scene_index_silent_by_default():
    """Without the debug env var the pass-through scene index logs nothing."""
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    scene = Path(__file__).parent / "test_scene.usda"
    if not scene.exists():
        pytest.skip("No test USD scene found")

    # env=None: os.environ copy with the debug var removed.
    result = ab.run_render_subprocess(binary_dir, scene)

    combined = result.stdout + result.stderr
    assert "RENDER_DONE" in combined, (
        f"render did not complete. tail:\n{combined[-1000:]}")
    assert "[SimSceneIndex]" not in combined, (
        f"Debug logging should be off by default. tail:\n{combined[-1000:]}"
    )
