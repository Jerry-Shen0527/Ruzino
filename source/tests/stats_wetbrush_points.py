"""Statistical analysis of the paint-particle point cloud produced by the
streaming Wetbrush zone (inspect_wetbrush_points.py output).

Pure numbers, no images. Run from Binaries/Release so pxr resolves:

    python ../../source/tests/stats_wetbrush_points.py
"""
import os
import sys
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"
DATA_DIR = Path(__file__).resolve().parent / "data" / "output"
USD_PATH = DATA_DIR / "wetbrush_inspect.usdc"

# --- params used by inspect_wetbrush_points.py (mirror here for checks) ---
RESOLUTION = 256
PAPER_SIZE = 1.0
BRUSH_RADIUS = 0.02
MOCK_N = 30
MOCK_AMP = 0.05
MOCK_LEN = 0.3
FPS = 60.0
NUM_FRAMES = 12
CELL = PAPER_SIZE / RESOLUTION


def load():
    from pxr import Usd
    stage = Usd.Stage.Open(str(USD_PATH))
    prim = stage.GetPrimAtPath("/Brush")
    pts_attr = prim.GetAttribute("points")
    col_attr = prim.GetAttribute("primvars:displayColor")
    width_attr = prim.GetAttribute("widths")
    out = []
    for t in sorted(pts_attr.GetTimeSamples()):
        p = pts_attr.Get(t)
        pts = np.array(p, dtype=float) if p is not None else np.zeros((0, 3))
        cols = None
        if col_attr and col_attr.GetNumTimeSamples():
            cs = col_attr.Get(t)
            if cs is not None:
                cols = np.array(cs, dtype=float).reshape(-1, 3)
        w = None
        if width_attr and width_attr.GetNumTimeSamples():
            ws = width_attr.Get(t)
            if ws is not None:
                w = np.array(ws, dtype=float).reshape(-1)
        out.append((t, pts, cols, w))
    return out


def box(s):
    return f"[{s}]" if s else ""


def main():
    sys.path.insert(0, str(BINARY_DIR))
    os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BINARY_DIR)
    frames = load()
    L = []
    w = L.append

    w("=" * 64)
    w("WETBRUSH STREAMING ZONE — PAINT POINT CLOUD STATISTICS")
    w("=" * 64)
    w("")
    w(f"params: res={RESOLUTION}  paper={PAPER_SIZE}  brush_radius={BRUSH_RADIUS}")
    w(f"        cell={CELL:.5f}  brush_diameter/cell={2*BRUSH_RADIUS/CELL:.1f} cells")
    w(f"        mock_stroke N={MOCK_N} amp={MOCK_AMP} len={MOCK_LEN}")
    w(f"        {NUM_FRAMES} frames @ {FPS}fps = {NUM_FRAMES/FPS:.3f}s sim")
    w("")

    # ---- per-frame accumulation ----
    w("--- ACCUMULATION (painted cells per frame) ---")
    w(f"{'frame':>5} {'t':>8} {'n':>7} {'delta':>7} {'cumul%':>7}")
    prev = 0
    for i, (t, pts, _c, _wid) in enumerate(frames):
        n = len(pts)
        d = n - prev
        frac = n / (MOCK_N * 20)  # rough ceiling ref
        w(f"{i:>5} {t:>8.4f} {n:>7} {d:>+7} {frac:>6.0%}")
        prev = n
    w("")
    # delta analysis: is growth monotonic & roughly steady?
    deltas = np.diff([len(f[1]) for f in frames])
    w(f"per-frame deltas: {deltas.tolist()}")
    w(f"  mean delta = {deltas.mean():.1f}, std = {deltas.std():.1f}, "
      f"min = {deltas.min()}, max = {deltas.max()}")
    w(f"  monotonic growth: {bool(np.all(deltas[1:] >= 0))}")
    w("")

    # ---- final frame geometry ----
    t, pts, cols, widths = frames[-1]
    w(f"--- FINAL FRAME t={t:.4f}  n={len(pts)} ---")
    if len(pts) == 0:
        w("  empty!"); print("\n".join(L)); return
    xmin, ymin, zmin = pts.min(axis=0)
    xmax, ymax, zmax = pts.max(axis=0)
    cx, cy, cz = pts.mean(axis=0)
    w(f"  X: [{xmin:+.4f}, {xmax:+.4f}]  span={xmax-xmin:.4f}  mean={cx:+.4f}")
    w(f"  Y: [{ymin:+.4f}, {ymax:+.4f}]  span={ymax-ymin:.4f}  mean={cy:+.4f}")
    w(f"  Z: [{zmin:+.4f}, {zmax:+.4f}]  span={zmax-zmin:.4f}  mean={cz:+.4f}")
    finite = np.all(np.isfinite(pts))
    w(f"  all finite (no NaN/Inf): {finite}")
    w("")

    # ---- footprint vs brush / stroke expectations ----
    w("--- FOOTPRINT SANITY ---")
    # only NUM_FRAMES of MOCK_N stroke points consumed; expected X advance
    consumed_frac = NUM_FRAMES / MOCK_N
    expected_x_advance = MOCK_LEN * consumed_frac
    w(f"  stroke consumed: {NUM_FRAMES}/{MOCK_N} = {consumed_frac:.0%} "
      f"-> expected X advance ~ {expected_x_advance:.4f}")
    w(f"  observed X span: {xmax-xmin:.4f}  (advance + brush footprint)")
    w(f"  brush diameter:  {2*BRUSH_RADIUS:.4f}  "
      f"(lateral footprint should be ~ this, +/- cell snapping)")
    yspan = ymax - ymin
    w(f"  observed Y span: {yspan:.4f}  vs brush diameter {2*BRUSH_RADIUS:.4f}  "
      f"-> {yspan/(2*BRUSH_RADIUS):.2f}x brush (lateral)")
    w(f"  Z is paint-layer thickness above paper: span {zmax-zmin:.4f} = "
      f"{(zmax-zmin)/CELL:.1f} cells  (all positive = on top of paper: "
      f"{bool(zmin > 0)})")
    w("")

    # ---- per-cell quantization (these are canvas cells, not free points) ----
    w("--- CELL QUANTIZATION (points are painted canvas cells) ---")
    # round to cell centers; count unique cells vs raw points
    grid = np.round(pts / CELL).astype(int)
    uniq = np.unique(grid, axis=0)
    w(f"  raw points: {len(pts)}   unique grid cells: {len(uniq)}   "
      f"duplicates: {len(pts)-len(uniq)}")
    # cell-center residual should be tiny if these are true cell samples
    resid = pts - grid * CELL
    w(f"  residual to cell grid: max|dx|={np.abs(resid[:,0]).max():.5f}, "
      f"max|dy|={np.abs(resid[:,1]).max():.5f}")
    w("")

    # ---- density of the painted region (points / bounding area) ----
    w("--- DENSITY ---")
    area = max((xmax - xmin) * (ymax - ymin), 1e-9)
    w(f"  points / bbox area = {len(pts)/area:.1f} pts/unit^2")
    w(f"  bbox area = {area:.5f} unit^2 = {area/CELL**2:.0f} cells")
    w(f"  fill fraction = {len(uniq)/(area/CELL**2):.1%} of bbox cells painted")
    w("")

    # ---- colors ----
    if cols is not None and len(cols):
        w("--- DISPLAY COLOR ---")
        w(f"  n colors: {len(cols)}  (range 0..1 per channel)")
        w(f"  R [{cols[:,0].min():.3f}, {cols[:,0].max():.3f}] mean {cols[:,0].mean():.3f}")
        w(f"  G [{cols[:,1].min():.3f}, {cols[:,1].max():.3f}] mean {cols[:,1].mean():.3f}")
        w(f"  B [{cols[:,2].min():.3f}, {cols[:,2].max():.3f}] mean {cols[:,2].mean():.3f}")
        lum = 0.299*cols[:,0] + 0.587*cols[:,1] + 0.114*cols[:,2]
        w(f"  luminosity: min {lum.min():.3f}  max {lum.max():.3f}  mean {lum.mean():.3f}")
        # color spread: std across channels tells if it's a flat ink or varies
        chspread = cols.std(axis=0)
        w(f"  per-channel std: R={chspread[0]:.4f} G={chspread[1]:.4f} B={chspread[2]:.4f}")
        n_unique_colors = len(np.unique(np.round(cols, 3), axis=0))
        w(f"  distinct colors (rounded 1e-3): {n_unique_colors}")
        w("")

    # ---- widths ----
    if widths is not None and len(widths):
        w("--- WIDTHS (point sizes) ---")
        w(f"  n: {len(widths)}  min {widths.min():.4f}  max {widths.max():.4f} "
          f"mean {widths.mean():.4f} std {widths.std():.4f}")
        w("")

    # ---- stroke shape: is it a line / curve? PCA on XY ----
    w("--- STROKE SHAPE (PCA on XY) ---")
    xy = pts[:, :2] - pts[:, :2].mean(axis=0)
    if len(xy) > 1:
        cov = np.cov(xy.T)
        eigval, eigvec = np.linalg.eigh(cov)
        eigval = np.sort(eigval)[::-1]  # largest first
        if eigval[0] > 0:
            ratio = eigval[1] / eigval[0]
            w(f"  eigenvalues: {eigval.tolist()}")
            w(f"  axis ratio (minor/major): {ratio:.3f}")
            w(f"  -> {'elongated line/curve stroke' if ratio < 0.5 else 'more blob-like'}")
        else:
            w("  degenerate (zero variance)")
    w("")

    w("=" * 64)
    print("\n".join(L))
    (DATA_DIR / "stats_report.txt").write_text("\n".join(L), encoding="utf-8")


if __name__ == "__main__":
    main()
