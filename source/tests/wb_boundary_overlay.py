#!/usr/bin/env python3
"""Wetbrush moving-boundary overlay + churn-by-boundary buckets (§36 tooling).

Pure post-processing over a rendered frame sequence (no sim, no renderer).
For each frame it composites:

  V1  overlay_fNNNN.png — the rendered frame + adjacent-frame paint diff
      (red = paint lost vs previous frame, green = paint gained) + the two
      MOVING SIM BOUNDARIES that are invisible in the render:
        - the §5.2 Eq.15 drain disc (circle, radius brush_radius + D0)
        - the §4.2 active window (square, WIN_XY cells re-centered on the brush)
      plus dimmer context circles at D0 and brush_radius.
  V2  churn.csv / churn_curves.png — lost/new pixels bucketed by distance to
      the disc edge and to the window wall, so "which boundary manufactures
      the artifact" becomes a curve instead of an eyeball judgment.

The brush trajectory, window origin, and camera projection are recomputed
analytically on the Python side to mirror the sim exactly:

  - trajectory: node_mock_pen_motion.cpp (DESCEND T_d → STROKE Length/Speed
    with y = A·sin(2π·cycles·s) → LIFT). Frame i's state corresponds to pen
    tau = i/60 (frame 0 is rendered after the first tick, whose pen cook used
    tau = 0).
  - window: node_brush_wb_sim.cpp position_window — origin = clamp(
    int((head + paper/2)/cell) - WIN/2, 0, res-WIN).
  - camera: hd_RUZINO renders with the horizontal aperture (36 mm) spanning
    the full image width and an isotropic mm→px scale (calibrated against the
    paper edges on frame_0050: mean edge error 0.8 px; the fit-vertical and
    anisotropic readings were >90 px off). px = 640 + (W/36)·f·x_cam/(−z_cam).

Run from Binaries/Release (or anywhere — paths resolve from __file__):

    python ../../source/tests/wb_boundary_overlay.py
    WB_SEQ_DIR=wetbrush_sequence_s34 WB_FRAMES=0:135 python ...
"""
import csv
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

# --- fixture constants (mirror render_wetbrush.py §34 calibration) ---------
FPS = 60.0
DT = 1.0 / FPS
PAPER = float(os.environ.get("WETBRUSH_PAPER", "10.0"))   # cm
RES = int(os.environ.get("WETBRUSH_RES", "1024"))         # grid cells / side
WIN = int(os.environ.get("WB_WIN_XY", "320"))             # active window cells
CELL = PAPER / RES                                        # cm
BRUSH_R = 0.5                                             # cm (socket)
D0 = 1.0                                                  # cm (sim constant)
DRAIN_R = BRUSH_R + D0                                    # 1.5 cm
T_DESCEND = 0.05                                          # s
T_LIFT = 0.15
AMPLITUDE = 1.0                                           # cm

# bands (cm) for the V2 buckets — ≈13 px at the canvas center depth
EDGE_BAND = 0.1   # |d − drain_r| ≤ band  → "disc edge"
WALL_BAND = 0.1   # signed distance to window wall ≤ band → "window wall"

# diff classification thresholds (hue-based: shadows scale all channels and
# keep the hue, so they don't flip the paint mask)
PAINT_MIN_R = 48
PAINT_DOM = 28     # r − max(g,b) ≥ DOM → paint
BLUR_DOM_R = 15    # b > r + 15 and b > g + 10 → bristle-sprite occlusion

SEQ_DIR = BIN / os.environ.get("WB_SEQ_DIR", "wetbrush_sequence_s34")
OUT_DIR = BIN / "test_output" / os.environ.get(
    "WB_OUT_NAME", "wb_boundary_overlay")
FRAME_RANGE = os.environ.get("WB_FRAMES", "")             # e.g. "0:135"

# pen shape (must mirror the WB_PEN_SHAPE the sequence was rendered with)
PEN_SHAPE = os.environ.get("WB_PEN_SHAPE", "line")
PEN_CYCLES = float(os.environ.get("WB_PEN_CYCLES",
                                  "1" if PEN_SHAPE != "line" else "2"))
# geometry/pacing knobs (must mirror WB_PEN_LENGTH / WB_PEN_SPEED)
STROKE_LEN = float(os.environ.get("WB_PEN_LENGTH", "5.0"))
STROKE_SPEED = float(os.environ.get("WB_PEN_SPEED", "2.5"))
T_STROKE = STROKE_LEN / STROKE_SPEED
T_PENUP = T_DESCEND + T_STROKE                            # stroke → lift

WIDTH, HEIGHT = 1280, 960
# Camera MUST mirror render_wetbrush.py: eye = (F*0.5, -F*1.1, F*1.5) with
# F = WB_CAM_FRAME (default 7.0). The old hardcoded (3.5,-7.7,10.5) silently
# mismatched every big-shape sequence rendered with WB_CAM_FRAME=9.5 — the
# 0.737 view-distance ratio then showed up as a phantom "0.739x shrink" and
# off-center markers in the overlays. (The renderer itself was never wrong;
# buffer dumps in grid space and the dense dot-lattice calibration both
# confirm identity mapping.)
CAM_FRAME = float(os.environ.get("WB_CAM_FRAME", "7.0"))
EYE = np.array([CAM_FRAME * 0.5, -CAM_FRAME * 1.1, CAM_FRAME * 1.5])
TARGET = np.array([0.0, 0.0, 0.0])
UP = np.array([0.0, 0.0, 1.0])
FWD = TARGET - EYE
FWD = FWD / np.linalg.norm(FWD)
RIGHT = np.cross(FWD, UP)
RIGHT = RIGHT / np.linalg.norm(RIGHT)
UP2 = np.cross(RIGHT, FWD)
FOCAL_MM = 50.0
APEX_MM = 36.0
PX_SCALE = (WIDTH / APEX_MM) * FOCAL_MM                    # isotropic, see docstring


def project(world: np.ndarray):
    """World (cm) → pixel (float). Calibrated mode: horizontal aperture spans
    the width, isotropic scale (mean 0.8 px residual on the paper edges)."""
    d = np.atleast_2d(np.asarray(world, float)) - EYE
    x = d @ RIGHT
    y = d @ UP2
    z = d @ (-FWD)
    return (WIDTH * 0.5 + PX_SCALE * x / (-z),
            HEIGHT * 0.5 - PX_SCALE * y / (-z))


def pixel_to_canvas_z0(px, py):
    """Pixel → world xy on the z=0 canvas plane (ray cast). Vectorized."""
    px = np.asarray(px, float)
    py = np.asarray(py, float)
    qx = (px - WIDTH * 0.5) / PX_SCALE
    qy = (HEIGHT * 0.5 - py) / PX_SCALE
    d = (qx[..., None] * RIGHT + qy[..., None] * UP2 + FWD)
    t = (-EYE[2] / d[..., 2])[..., None]
    return EYE[0] + (t * d)[..., 0], EYE[1] + (t * d)[..., 1]


def _unit_path(t):
    """mock_pen_motion unit parametrizations (t in [0,1], UNSCALED) — mirrors
    node_mock_pen_motion.cpp. Line keeps legacy scale (=Length); closed
    shapes are unit-size and scaled so total arc length = Length."""
    t = np.asarray(t, float)
    if PEN_SHAPE == "circle":
        th = -math.pi * 0.5 + 2.0 * math.pi * PEN_CYCLES * t
        return np.cos(th), np.sin(th)
    if PEN_SHAPE == "square":
        # regularized superellipse (mirror reg_pow in the node): f(u) =
        # u·(u²+ε²)^((k−1)/2), k = 1/3 — cbrt-like away from 0, bounded
        # slope near 0.
        th = -math.pi * 0.5 + 2.0 * math.pi * PEN_CYCLES * t
        eps2 = 1e-4
        m = -1.0 / 3.0
        f = lambda u: u * np.power(u * u + eps2, m)
        return f(np.cos(th)), f(np.sin(th))
    if PEN_SHAPE == "figure8":
        return (np.sin(4.0 * math.pi * PEN_CYCLES * t + math.pi * 0.5),
                0.6 * np.sin(2.0 * math.pi * PEN_CYCLES * t))
    if PEN_SHAPE in ("triangle", "star"):
        n = 3.0 if PEN_SHAPE == "triangle" else 5.0
        a = 0.38 if PEN_SHAPE == "triangle" else 0.35
        th = math.pi * 0.5 + 2.0 * math.pi * PEN_CYCLES * t
        r = 1.0 + a * np.cos(n * (th - math.pi * 0.5))
        return r * np.cos(th), r * np.sin(th)
    if PEN_SHAPE == "spiral":
        th = -math.pi * 0.5 + 2.0 * math.pi * PEN_CYCLES * t
        r = 1.0 - 0.76 * t
        return r * np.cos(th), r * np.sin(th)
    return (t - 0.5) * STROKE_LEN, AMPLITUDE * np.sin(
        2.0 * math.pi * PEN_CYCLES * t)


def _smootherstep(u):
    u = np.clip(u, 0.0, 1.0)
    return u * u * u * (u * (6.0 * u - 15.0) + 10.0)


_ARC_N = 2048
_ARC_C, _ARC_SCALE = None, 1.0


def _arc_init():
    """Cumulative arc-length fractions of the unit shape + Length scale."""
    global _ARC_C, _ARC_SCALE
    if _ARC_C is not None:
        return
    ts = np.arange(_ARC_N + 1) / _ARC_N
    xs, ys = _unit_path(ts)
    seg = np.hypot(np.diff(xs), np.diff(ys))
    total = float(np.sum(seg))
    _ARC_C = np.concatenate([[0.0], np.cumsum(seg)]) / max(total, 1e-9)
    _ARC_SCALE = STROKE_LEN / max(total, 1e-9)


def head_xy(tau: float):
    """mock_pen_motion pen-tip XY at elapsed time tau (mirrors the node's
    smootherstep phase easing and arc-length reparametrization)."""
    scale = 1.0
    if PEN_SHAPE != "line":
        _arc_init()
        scale = _ARC_SCALE
    if tau < T_DESCEND:
        x, y = _unit_path(0.0)
        return float(x) * scale, float(y) * scale
    u = min((tau - T_DESCEND) / T_STROKE, 1.0)
    s = _smootherstep(u)
    if PEN_SHAPE == "line":
        t = float(s)
    else:
        k = int(np.clip(np.searchsorted(_ARC_C, s, side="right") - 1,
                        0, _ARC_N - 1))
        span = max(_ARC_C[k + 1] - _ARC_C[k], 1e-9)
        wgt = float(np.clip((s - _ARC_C[k]) / span, 0.0, 1.0))
        t = (k + wgt) / _ARC_N
    x, y = _unit_path(np.asarray(t))
    return float(x) * scale, float(y) * scale


def pen_down(tau: float) -> bool:
    return T_DESCEND <= tau < T_PENUP


def window_origin(headx: float, heady: float):
    """position_window: clamp(int(cell coord) − WIN/2, 0, res−WIN)."""
    bgx = (headx + PAPER * 0.5) / CELL
    bgy = (heady + PAPER * 0.5) / CELL
    ox = max(0, min(int(bgx) - WIN // 2, RES - WIN))
    oy = max(0, min(int(bgy) - WIN // 2, RES - WIN))
    return ox, oy


def window_bounds(ox: int, oy: int):
    """World-space window square (cell boundary → cell boundary)."""
    x0 = ox * CELL - PAPER * 0.5
    y0 = oy * CELL - PAPER * 0.5
    return x0, y0, x0 + WIN * CELL, y0 + WIN * CELL


def circle_points(cx: float, cy: float, r: float, z: float = 0.0, n: int = 96):
    t = np.linspace(0.0, 2.0 * math.pi, n)
    return np.stack([cx + r * np.cos(t), cy + r * np.sin(t),
                     np.full(n, z)], axis=1)


def paint_mask(rgb: np.ndarray):
    r = rgb[..., 0].astype(np.int32)
    g = rgb[..., 1].astype(np.int32)
    b = rgb[..., 2].astype(np.int32)
    mx = np.maximum(g, b)
    return (r >= PAINT_MIN_R) & ((r - mx) >= PAINT_DOM)


def bluish_mask(rgb: np.ndarray):
    r = rgb[..., 0].astype(np.int32)
    g = rgb[..., 1].astype(np.int32)
    b = rgb[..., 2].astype(np.int32)
    return (b > r + BLUR_DOM_R) & (b > g + 10)


def _erode(m: np.ndarray, it: int) -> np.ndarray:
    for _ in range(it):
        m = (m & np.roll(m, 1, 0) & np.roll(m, -1, 0)
             & np.roll(m, 1, 1) & np.roll(m, -1, 1))
    return m


def _dilate(m: np.ndarray, it: int) -> np.ndarray:
    for _ in range(it):
        m = (m | np.roll(m, 1, 0) | np.roll(m, -1, 0)
             | np.roll(m, 1, 1) | np.roll(m, -1, 1))
    return m


def hole_mask(prev: np.ndarray, cur: np.ndarray) -> np.ndarray:
    """Real interior paint loss (the §34 nb_count≥7 idea, morphological
    form): a pixel that was DEEP inside the paint and is now paint-free even
    after dilating the new mask — kills the sub-threshold outline flicker
    that dominates the naive frame diff."""
    return _erode(prev, 2) & ~_dilate(cur, 2)


def draw_polyline(dr, pts2d, color, width):
    xy = [(float(x), float(y)) for x, y in zip(*project(pts2d))]
    dr.line(xy, fill=color, width=width, joint="curve")


def load_font(size=16):
    for cand in ("C:/Windows/Fonts/arial.ttf",
                 "C:/Windows/Fonts/segoeui.ttf"):
        if Path(cand).exists():
            return ImageFont.truetype(cand, size)
    return ImageFont.load_default()


def bucket(d_disc, w_signed):
    """(disc bucket, window bucket) from arrays of distances in cm."""
    d = np.asarray(d_disc)
    w = np.asarray(w_signed)
    bd = np.where(np.abs(d - DRAIN_R) <= EDGE_BAND, "disc_edge",
                  np.where(d < DRAIN_R, "disc_int", "disc_ext"))
    bw = np.where(w < 0, "win_out",
                  np.where(w <= WALL_BAND, "win_wall", "win_int"))
    return bd, bw


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    frames = sorted(SEQ_DIR.glob("frame_*.png"))
    if FRAME_RANGE:
        lo, hi = (int(v) for v in FRAME_RANGE.split(":"))
        frames = [f for f in frames
                  if lo <= int(f.stem.split("_")[1]) < hi]
    if not frames:
        sys.exit(f"no frame_*.png under {SEQ_DIR}")
    print(f"[overlay] {len(frames)} frames from {SEQ_DIR.name} -> {OUT_DIR}")

    font = load_font(18)
    rows = []
    prev_mask = None
    charts_n = []

    for f in frames:
        idx = int(f.stem.split("_")[1])
        tau = idx * DT
        hx, hy = head_xy(tau)
        pdown = pen_down(tau)
        ox, oy = window_origin(hx, hy)
        wx0, wy0, wx1, wy1 = window_bounds(ox, oy)

        img = Image.open(f).convert("RGB")
        rgb = np.asarray(img, float)
        cur_mask = paint_mask(rgb)

        lost = new = holes = None
        if prev_mask is not None:
            lost = prev_mask & ~cur_mask & ~bluish_mask(rgb)
            new = cur_mask & ~prev_mask
            holes = hole_mask(prev_mask, cur_mask)

        # ---- V1 composite -------------------------------------------------
        base = np.asarray(img, np.uint8).copy()
        if lost is not None:
            base[lost] = (base[lost] * 0.25 + np.array([230, 0, 70]) * 0.75
                          ).astype(np.uint8)
        if new is not None:
            base[new] = (base[new] * 0.25 + np.array([0, 235, 90]) * 0.75
                         ).astype(np.uint8)
        if holes is not None:
            # real interior holes pop in yellow on top of the pink diff
            base[holes] = (base[holes] * 0.15 + np.array([255, 230, 0]) * 0.85
                           ).astype(np.uint8)
        ov = Image.fromarray(base)
        dr = ImageDraw.Draw(ov)

        # window square (magenta) — the moving sim boundary
        corners = np.array([[wx0, wy0, 0], [wx1, wy0, 0],
                            [wx1, wy1, 0], [wx0, wy1, 0], [wx0, wy0, 0]])
        draw_polyline(dr, corners, (255, 0, 255), 3)
        # brush center vs contact patch: the sim's brush_pos (= the pen
        # sample point, marked by the cross) is the ROOT-DISK center; under
        # the 30° drag tilt the bristle TIPS — and the paint they deposit —
        # trail BEHIND it by rest_len·sin(tilt) opposite the motion. Draw
        # both: the sim's disc stays root-centered (grid_to_particle uses
        # brush_pos), the white bristle-footprint circle marks the patch.
        tilt_deg = float(os.environ.get("WB_PEN_TILT", "30"))
        h2 = head_xy(tau + DT)
        vx = (h2[0] - hx) / DT
        vy = (h2[1] - hy) / DT
        vv = math.hypot(vx, vy)
        tx, ty = hx, hy
        if pdown and tilt_deg > 0.0 and vv > 1e-6:
            trail = 1.5 * BRUSH_R * math.sin(math.radians(tilt_deg))
            tx, ty = hx - trail * vx / vv, hy - trail * vy / vv

        # drain disc R+D0 (cyan; dim when pen-up: Eq.15 suspended) — ROOT
        # centered, exactly what grid_to_particle tests
        col = (0, 255, 255) if pdown else (0, 130, 130)
        draw_polyline(dr, circle_points(hx, hy, DRAIN_R), col, 3)
        # context circle: D0 adhesion bulb (yellow, root-centered)
        draw_polyline(dr, circle_points(hx, hy, D0), (200, 170, 0) if pdown
                      else (100, 90, 0), 1)
        # bristle contact footprint (white) — TIP-PATCH centered
        draw_polyline(dr, circle_points(tx, ty, BRUSH_R),
                      (240, 240, 240) if pdown else (110, 110, 110), 1)
        # brush center cross (root) + tip-patch marker + connecting line
        bx, by = (float(v[0]) for v in project(np.array([[hx, hy, 0.0]])))
        dr.line([(bx - 12, by), (bx + 12, by)], fill=(255, 255, 255), width=2)
        dr.line([(bx, by - 12), (bx, by + 12)], fill=(255, 255, 255), width=2)
        txp, typ = (float(v[0]) for v in project(np.array([[tx, ty, 0.0]])))
        dr.line([(bx, by), (txp, typ)], fill=(200, 200, 200), width=1)
        dr.ellipse([txp - 4, typ - 4, txp + 4, typ + 4],
                   outline=(255, 255, 255), width=2)
        # stroke start/end anchors (the shape's touchdown/liftoff points)
        sx0, sy0 = head_xy(T_DESCEND + 1e-9)
        sx, sy = (float(v[0]) for v in project(np.array([[sx0, sy0, 0.0]])))
        dr.ellipse([sx - 5, sy - 5, sx + 5, sy + 5],
                   outline=(255, 255, 255), width=2)
        ex0, ey0 = head_xy(T_PENUP + 1e-9)
        ex, ey = (float(v[0]) for v in project(np.array([[ex0, ey0, 0.0]])))
        dr.rectangle([ex - 5, ey - 5, ex + 5, ey + 5],
                     outline=(180, 180, 180), width=2)

        # ---- V2 buckets ---------------------------------------------------
        lost_total = int(lost.sum()) if lost is not None else 0
        new_total = int(new.sum()) if new is not None else 0
        paint_px = int(cur_mask.sum())
        bcounts = {k: 0 for k in ("disc_int", "disc_edge", "disc_ext",
                                  "win_out", "win_wall", "win_int",
                                  "edge_x_wout")}
        nb = {k: dict(bcounts) for k in ("lost", "new", "holes")}
        for m, tag in ((lost, "lost"), (new, "new"), (holes, "holes")):
            if m is None or not m.any():
                continue
            ys, xs = np.nonzero(m)
            wxs, wys = pixel_to_canvas_z0(xs + 0.5, ys + 0.5)
            d_disc = np.hypot(wxs - hx, wys - hy)
            w_signed = np.minimum(np.minimum(wxs - wx0, wx1 - wxs),
                                  np.minimum(wys - wy0, wy1 - wys))
            bd, bw = bucket(d_disc, w_signed)
            for bdv, bwv in zip(bd, bw):
                nb[tag][bdv] += 1
                nb[tag][bwv] += 1
                if bdv == "disc_edge" and bwv == "win_out":
                    nb[tag]["edge_x_wout"] += 1

        churn = 100.0 * (lost_total + new_total) / max(paint_px, 1)
        holes_total = sum(nb["holes"][k] for k in
                          ("disc_int", "disc_edge", "disc_ext"))
        row = {
            "frame": idx, "tau": round(tau, 4), "pen_down": int(pdown),
            "head_x": round(hx, 4), "head_y": round(hy, 4),
            "win_ox": ox, "win_oy": oy,
            "paint_px": paint_px, "lost": lost_total, "new": new_total,
            "churn_pct": round(churn, 3), "holes": holes_total,
        }
        for tag in ("lost", "new", "holes"):
            for k in bcounts:
                row[f"{tag}_{k}"] = nb[tag][k]
        rows.append(row)
        charts_n.append(row)

        dr.text((14, 12),
                f"f{idx:03d} tau={tau:5.2f}s pen={'DOWN' if pdown else 'up  '}"
                f" root=({hx:+.2f},{hy:+.2f})cm tip=({tx:+.2f},{ty:+.2f})cm"
                f"  win=({ox},{oy})",
                fill=(0, 0, 0), font=font)
        dr.text((14, 34),
                f"churn {churn:5.2f}%   lost {lost_total:6d}   new "
                f"{new_total:6d}   HOLES {nb['holes']['disc_int']+nb['holes']['disc_edge']+nb['holes']['disc_ext']:5d}"
                f" (edge {nb['holes']['disc_edge']:4d} wall {nb['holes']['win_wall']:4d})",
                fill=(0, 0, 0), font=font)
        ov.save(OUT_DIR / f"overlay_{idx:04d}.png")
        prev_mask = cur_mask

    # ---- CSV ---------------------------------------------------------------
    csv_path = OUT_DIR / "churn.csv"
    with open(csv_path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"[overlay] churn table -> {csv_path}")

    # ---- curves (PIL, no matplotlib) ---------------------------------------
    curves_path = OUT_DIR / "churn_curves.png"
    draw_curves(charts_n, curves_path)
    print(f"[overlay] churn curves -> {curves_path}")

    # ---- montage of key frames ---------------------------------------------
    keys = [14, 16, 30, 50, 70, 90, 110, 130]
    tiles = []
    for k in keys:
        p = OUT_DIR / f"overlay_{k:04d}.png"
        if p.exists():
            tiles.append(np.asarray(Image.open(p).resize((640, 480))))
    if tiles:
        while len(tiles) % 2:
            tiles.append(np.zeros_like(tiles[0]))
        rowsimg = [np.hstack(tiles[i:i + 2]) for i in range(0, len(tiles), 2)]
        Image.fromarray(np.vstack(rowsimg)).save(OUT_DIR / "montage.png")
        print(f"[overlay] montage -> {OUT_DIR / 'montage.png'}")

    # ---- mp4 ----------------------------------------------------------------
    mp4 = OUT_DIR / "overlay.mp4"
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-framerate", "12",
             "-i", str(OUT_DIR / "overlay_%04d.png"),
             "-pix_fmt", "yuv420p", "-crf", "23", str(mp4)],
            check=True)
        print(f"[overlay] mp4 -> {mp4}")
    except Exception as e:  # ffmpeg missing is fine
        print(f"[overlay] mp4 skipped: {e}")

    peaks = sorted(charts_n, key=lambda r: -r["churn_pct"])[:8]
    print("[overlay] worst churn frames:",
          ", ".join(f"f{r['frame']}({r['churn_pct']:.2f}%)" for r in peaks))


def draw_curves(rows, path):
    """Two stacked PIL charts: (1) churn% + lost/new totals, (2) the same
    churn split by boundary bucket (disc edge vs window wall vs elsewhere)."""
    CW, CH = 1280, 720
    M = 70
    img = Image.new("RGB", (CW, CH * 2 + 40), (250, 250, 248))
    dr = ImageDraw.Draw(img)
    font = load_font(16)
    n = len(rows)
    f0 = rows[0]["frame"]
    f1 = rows[-1]["frame"]

    def xf(i):
        return M + (CW - 2 * M) * (i / max(n - 1, 1))

    def yf(chart, v, vmax):
        top = chart * (CH + 40) + M
        h = CH - 2 * M
        return top + h * (1.0 - min(v / vmax, 1.0))

    def series(chart, vals, vmax, color, label, width=3):
        pts = [(xf(i), yf(chart, v, vmax)) for i, v in enumerate(vals)]
        dr.line(pts, fill=color, width=width)
        dr.text((CW - M + 8, yf(chart, vals[-1], vmax) - 8), label,
                fill=color, font=font)

    # chart 1: totals
    maxlost = max(max(r["lost"] for r in rows),
                  max(r["new"] for r in rows), 1)
    dr.text((M, 16), "paint churn per frame  (px)", fill=(0, 0, 0), font=font)
    series(0, [r["lost"] for r in rows], maxlost, (220, 0, 70), "lost")
    series(0, [r["new"] for r in rows], maxlost, (0, 160, 60), "new")
    series(0, [r["churn_pct"] * maxlost / 5.0 for r in rows], maxlost,
           (0, 0, 0), f"churn%  (max {max(r['churn_pct'] for r in rows):.2f})",
           width=1)

    # chart 2: REAL interior holes by boundary bucket (own scale — the naive
    # lost/new totals are dominated by head-advance and outline flicker)
    dr.text((M, CH + 56),
            "interior HOLES (morphological) by boundary bucket  "
            f"(edge band ±{EDGE_BAND}cm of r={DRAIN_R}cm disc; "
            f"wall band 0..{WALL_BAND}cm inside window)",
            fill=(0, 0, 0), font=font)
    hedge = [r["holes_disc_edge"] for r in rows]
    hint = [r["holes_disc_int"] for r in rows]
    hwall = [r["holes_win_wall"] for r in rows]
    helse = [r["holes"] - e - i - w
             for r, e, i, w in zip(rows, hedge, hint, hwall)]
    mx = max(max(hedge), max(hint), max(hwall), max(helse), 1)
    series(1, helse, mx, (120, 120, 120), "elsewhere (incl. frozen zone)")
    series(1, hint, mx, (0, 120, 255), "disc interior")
    series(1, hedge, mx, (255, 140, 0), "disc edge")
    series(1, hwall, mx, (200, 0, 200), "window wall")

    # axes + frame gridlines every 15
    for chart in (0, 1):
        top = chart * (CH + 40) + M
        dr.line([(M, top), (M, top + CH - 2 * M), (CW - M, top + CH - 2 * M)],
                fill=(0, 0, 0), width=2)
        for r in rows:
            if r["frame"] % 15 == 0:
                x = xf(rows.index(r))
                dr.line([(x, top), (x, top + CH - 2 * M)],
                        fill=(220, 220, 220), width=1)
                dr.text((x - 14, top + CH - 2 * M + 6), str(r["frame"]),
                        fill=(0, 0, 0), font=font)
        for frac in (0.0, 0.5, 1.0):
            v = frac * (maxlost if chart == 0 else mx)
            y = yf(chart, v, maxlost if chart == 0 else mx)
            dr.text((8, y - 8), f"{v:.0f}", fill=(0, 0, 0), font=font)

    img.save(path)


if __name__ == "__main__":
    main()
