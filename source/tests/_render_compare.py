"""Convergence-based render test helpers.

Instead of comparing a render against a single hard-coded threshold or a fixed
reference image, we judge whether a render *converges* as sample count grows.

Approach
--------
A high-SPP render (the **anchor**) acts as the convergence target. We then
render a sequence of lower-SPP **probes** (e.g. 16, 32, 64, 128, 256) and
compute each probe's per-channel MSE against the anchor. A correct path tracer
produces a monotonically decreasing MSE curve that drops substantially from the
first probe to the last.

Because we judge the *shape* of the curve (decreasing, converging) rather than
its absolute values, the test is robust to GPU/driver/RNG differences across
machines — only a renderer that genuinely stops converging fails.

The anchor image is committed to ``source/tests/data/reference/`` (the single
allowed location for committed render output) so it is generated once and
reused. Probe renders and the MSE curve are written under
``Binaries/Release/test_output/`` (never the source tree).
"""

import json
import os
from pathlib import Path

import numpy as np


def compute_mse(a, b):
    """Per-channel mean squared error between two HxWxC float images.

    Compares RGB channels only (alpha is renderer bookkeeping, not signal).
    Returns a single float (mean over all RGB pixels).
    """
    a_rgb = a[..., :3].astype(np.float64)
    b_rgb = b[..., :3].astype(np.float64)
    return float(np.mean((a_rgb - b_rgb) ** 2))


def assert_converging(curve, min_drop_ratio=0.5):
    """Assert a probe MSE curve shows genuine convergence.

    ``curve`` is a list of ``(spp, mse)`` tuples ordered by increasing SPP.

    Two conditions:
      1. Monotonically non-increasing (more samples => closer to anchor).
         Small non-monotonic blips are tolerated; we flag only when a later
         probe is meaningfully worse than an earlier one.
      2. The final MSE drops substantially from the first (the renderer is
         actually getting closer to the anchor, not flat-lining).

    Judging the curve shape — not absolute MSE — keeps this robust to
    GPU/driver/RNG differences.
    """
    assert len(curve) >= 2, f"need >=2 probes to judge convergence, got {len(curve)}"

    spps = [s for s, _ in curve]
    mses = [m for _, m in curve]

    # Condition 1: monotonic non-increasing (with small tolerance for noise).
    # A probe is "meaningfully worse" if it exceeds a previous MSE by more than
    # 5% of the first probe's value — that is a real divergence, not jitter.
    noise_tol = max(mses[0] * 0.05, 1e-6)
    running_min = mses[0]
    for spp, mse in curve[1:]:
        if mse > running_min + noise_tol:
            raise AssertionError(
                f"MSE not converging: at {spp} SPP mse={mse:.6f} exceeds earlier "
                f"min {running_min:.6f} by more than noise tol {noise_tol:.6f}. "
                f"Full curve: {curve}")
        running_min = min(running_min, mse)

    # Condition 2: substantial drop from first to last probe.
    drop = mses[0] - mses[-1]
    assert drop > mses[0] * min_drop_ratio * (1 - 1e-9), (
        f"MSE did not drop substantially: first={mses[0]:.6f} last={mses[-1]:.6f} "
        f"(drop {drop:.6f} < required {mses[0]*min_drop_ratio:.6f}). "
        f"Renderer may be flat-lining. Full curve: {curve}")

    print(f"  convergence OK: MSE {' -> '.join(f'{m:.6f}' for _, m in curve)}")


def render_convergence(render_n_fn, anchor_path, probe_spps, save_dir,
                       width=256, height=256, anchor_spp=512,
                       anchor_render_n_fn=None):
    """Run a convergence sweep and return the MSE curve.

    Each probe is rendered on the *same* renderer instance to avoid leaking
    GPU/Hydra resources across multiple HydraRenderer constructions within one
    process (the HD renderer plugin is a global singleton that does not tolerate
    repeated teardown). Callers therefore pass a *single* closure rather than
    constructing a new renderer per probe.

    ``render_n_fn(spp) -> HxWx4 float32 ndarray`` renders exactly ``spp`` samples
    on its renderer (typically: reset_accumulation(); loop render() spp times;
    get_output_texture()). It is invoked once per entry in ``probe_spps``.

    The anchor is loaded from ``anchor_path`` if present (preferred — it is the
    committed reference). If absent and ``anchor_render_n_fn`` is given, the
    anchor is rendered with it at ``anchor_spp``; otherwise the function raises.

    Side effects (all under ``save_dir``, which must be under Binaries):
      - ``anchor.png`` / each ``probe_<spp>.png`` for visual inspection
      - ``mse_curve.json`` recording the curve for post-run diagnosis

    Returns ``curve`` = ``[(spp, mse), ...]`` ordered by ``probe_spps``.
    """
    save_dir = Path(save_dir)
    save_dir.mkdir(parents=True, exist_ok=True)

    anchor = load_anchor(anchor_path)
    if anchor is None:
        if anchor_render_n_fn is None:
            raise FileNotFoundError(
                f"anchor not found at {anchor_path} and no "
                f"anchor_render_n_fn provided to generate it")
        anchor = anchor_render_n_fn(anchor_spp)
        anchor_path.parent.mkdir(parents=True, exist_ok=True)
        np.save(anchor_path.with_suffix(".npy"), anchor)
        _save_png(anchor, anchor_path.with_suffix(".png"))
        print(f"  anchor rendered ({anchor_spp} SPP) -> {anchor_path.name}")
    _save_png(anchor, save_dir / "anchor.png")

    curve = []
    for spp in probe_spps:
        img = render_n_fn(spp)
        _save_png(img, save_dir / f"probe_{spp}.png")
        mse = compute_mse(anchor, img)
        curve.append((spp, mse))
        print(f"  probe {spp:>4d} SPP -> MSE {mse:.6f}")

    (save_dir / "mse_curve.json").write_text(
        json.dumps([{"spp": s, "mse": m} for s, m in curve], indent=2),
        encoding="utf-8")
    return curve


def load_anchor(anchor_path):
    """Load a committed anchor as HxWx4 float32, or None if neither form exists."""
    anchor_npy = anchor_path.with_suffix(".npy")
    anchor_png = anchor_path.with_suffix(".png")

    if anchor_npy.exists():
        return np.load(anchor_npy)
    if anchor_png.exists():
        from PIL import Image
        rgb = np.asarray(Image.open(anchor_png), dtype=np.float32) / 255.0
        if rgb.ndim == 2:
            rgb = rgb[..., None]
        alpha = np.ones(rgb.shape[:2] + (1,), dtype=np.float32)
        return np.concatenate([rgb, alpha], axis=-1)
    return None


def _save_png(img, path):
    """Save an HxWx4 float image as an 8-bit PNG (best-effort, no hard PIL dep)."""
    try:
        from PIL import Image
    except ImportError:
        return
    rgb = np.clip(img[..., :3], 0, 1)
    rgb = (rgb * 255).astype(np.uint8)
    Image.fromarray(rgb).save(path)
