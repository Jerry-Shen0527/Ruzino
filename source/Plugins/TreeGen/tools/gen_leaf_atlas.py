"""Procedural leaf atlas generator for TreeGen (Stava 2014 foliage cards).

Draws a 2x2 atlas of species leaf tiles as RGBA PNG (albedo + alpha in one
image). Alpha is the leaf silhouette; the path tracer's stochastic opacity
presence test (path_tracing.slang) treats alpha=0 as pass-through, so no
separate alpha-mask setup is needed beyond binding this texture as the
material's opacity source.

Species tiles (leaf length axis = tile V, tip at V=1):
    tile 0 (lower-left):  ovate serrate   (cherry / elm like)
    tile 1 (lower-right): lanceolate      (willow like)
    tile 2 (upper-left):  palmate lobed   (maple like)
    tile 3 (upper-right): obovate entire  (pear like)

Usage:
    python gen_leaf_atlas.py --out leaf_atlas.png [--size 1024] [--seed 7]

Pure numpy + Pillow; deterministic under --seed. Committed atlas lives in
source/Plugins/TreeGen/assets/leaf_atlas.png; regenerate with this tool.
"""
import argparse
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# Supersampling factor for geometry (serration teeth + smooth silhouette AA)
SS = 4


# ---------------------------------------------------------------------------
# Outline models. Leaf-local frame: t along length in [0,1] (base 0, tip 1),
# u along width in [-1,1]. All outlines are closed polygons in tile pixels.
# ---------------------------------------------------------------------------

def _ovate_outline(n, aspect, peak_bias, serr_depth, serr_freq, rng):
    """Ovate/lanceolate family: width profile + optional marginal teeth."""
    ts = np.linspace(0.0, 1.0, n)
    # Width profile: sin(pi * t^peak_bias)^1 — peak_bias<1 widens the base,
    # >1 widens the tip. Peak sits at t = 0.5 ** (1 / peak_bias).
    w = np.sin(np.pi * np.power(ts, peak_bias))
    if serr_depth > 0.0:
        # Teeth: high-frequency ripple, amplitude fading towards base and tip
        fade = np.clip(4.0 * ts * (1.0 - ts), 0.0, 1.0) ** 0.5
        teeth = np.abs(np.sin(math.pi * serr_freq * ts))
        # Slightly randomized tooth heights
        teeth *= 0.7 + 0.3 * rng.random(n)
        w *= 1.0 + serr_depth * teeth * fade
    w = np.clip(w, 0.0, None)
    half_w = 0.5 / aspect  # true leaf aspect encoded in the tile
    right = np.stack([ts, 0.5 + half_w * w], axis=1)
    left = np.stack([ts, 0.5 - half_w * w], axis=1)
    pts = np.concatenate([right, left[::-1]], axis=0)  # t, u
    return pts


def _palmate_outline(n, lobes, sinus_depth, rng):
    """Palmate (maple-like): polar outline with lobe modulation."""
    theta = np.linspace(0.0, 2.0 * np.pi, n, endpoint=False)
    r = 0.62 + 0.38 * np.power(np.abs(np.cos(lobes / 2.0 * theta)), 0.55)
    r *= 1.0 - sinus_depth * np.power(
        np.abs(np.sin(lobes / 2.0 * theta)), 8.0)
    r *= 0.95 + 0.05 * rng.random(n)
    # x=u (width), y=t (length, petiole at bottom): squash vertically a bit
    u = r * np.sin(theta) * 0.9
    t = 0.5 + r * np.cos(theta) * 0.62
    return np.stack([t, u], axis=1)


def _polygon_pixels(pts, size, margin):
    """Map outline (t,u) to supersampled pixel polygon (x=u, y=1-t flipped
    for the PNG row order — tip must sit at UV v=1 = top of PNG)."""
    m = margin * size * SS
    span = size * SS - 2 * m
    # u in [-1,1] maps to the full tile span; outlines already carry their
    # true width via |u| = half_w <= 0.5
    x = m + (pts[:, 1] + 1.0) * 0.5 * span
    y = m + (1.0 - pts[:, 0]) * span
    return list(zip(x.tolist(), y.tolist()))


# ---------------------------------------------------------------------------
# Albedo painting
# ---------------------------------------------------------------------------

def _base_albedo(size_ss, hue_jitter, rng):
    """Vertical gradient green + low-frequency mottling, leaf-local t axis."""
    t = np.linspace(0.0, 1.0, size_ss)[:, None]  # rows: top of image = tip
    # Row 0 of the image is the leaf TIP (dark tip → bright mid → mid base)
    base = 0.55 + 0.45 * np.sin(np.pi * np.clip(1.0 - t, 0, 1)) ** 0.8
    r = (0.09 + 0.10 * base) * (1.0 + hue_jitter[0])
    g = (0.24 + 0.28 * base) * (1.0 + hue_jitter[1])
    b = (0.05 + 0.08 * base) * (1.0 + hue_jitter[2])
    img = np.repeat(np.stack([r, g, b], axis=2), size_ss, axis=1)

    # Low-frequency mottling: 8x8 noise bicubic-upsampled, ±8%
    small = (rng.random((8, 8, 1)) - 0.5) * 0.16
    mott = np.array(
        Image.fromarray(
            (np.clip(small[:, :, 0], -1, 1) * 127 + 128).astype(np.uint8)
        ).resize((size_ss, size_ss), Image.BICUBIC),
        dtype=np.float32)[:, :, None] / 255.0 - 0.5
    img *= 1.0 + mott
    return img


def _draw_veins(draw, pts_px, size_ss, rng, species):
    """Midrib + pinnate laterals as light strokes; palmate gets radial veins."""
    tip = min(pts_px, key=lambda p: p[1])
    base = max(pts_px, key=lambda p: p[1])
    if species == "palmate":
        cx = (tip[0] + base[0]) * 0.5
        cy = base[1] - (base[1] - tip[1]) * 0.42
        for k in range(7):
            ang = -math.pi / 2 + (k - 3) * 0.42
            L = size_ss * (0.30 if k % 3 else 0.40)
            ex, ey = cx + math.sin(ang) * L, cy - math.cos(ang) * L
            draw.line([(cx, cy), (ex, ey)],
                      fill=(150, 190, 110, 200), width=SS * 2)
        return
    draw.line([base, tip], fill=(150, 190, 110, 210), width=SS * 3)
    n_veins = rng.integers(9, 13)
    bx, by = base
    for k in range(1, n_veins + 1):
        f = k / (n_veins + 1)
        sx = bx + (tip[0] - bx) * f
        sy = by + (tip[1] - by) * f
        side = 1 if k % 2 else -1
        ln = size_ss * 0.16 * (1.0 - 0.55 * abs(f - 0.45))
        ang = side * (0.7 + 0.25 * rng.random())
        ex = sx + math.sin(ang) * ln
        ey = sy - math.cos(ang) * ln * 0.8
        draw.line([(sx, sy), (ex, ey)],
                  fill=(150, 190, 110, 170), width=max(1, SS))


# ---------------------------------------------------------------------------
# Tile assembly
# ---------------------------------------------------------------------------

def _species_params():
    return [
        dict(name="ovate_serrate", kind="ovate", aspect=1.8, peak_bias=1.35,
             serr_depth=0.045, serr_freq=19.0,
             hue=(0.00, 0.00, 0.00), margin_frac=0.06),
        dict(name="lanceolate", kind="ovate", aspect=3.6, peak_bias=0.80,
             serr_depth=0.02, serr_freq=14.0,
             hue=(-0.05, 0.05, -0.02), margin_frac=0.04),
        dict(name="palmate", kind="palmate", aspect=1.0,
             lobes=5, sinus_depth=0.35,
             hue=(-0.08, -0.05, 0.03), margin_frac=0.08),
        dict(name="obovate", kind="ovate", aspect=1.45, peak_bias=0.70,
             serr_depth=0.0, serr_freq=0.0,
             hue=(0.14, 0.10, -0.01), margin_frac=0.07),
    ]


def draw_tile(spec, size_tile, rng):
    """Draw one species into a (size_tile*SS) RGBA supersampled canvas."""
    size_ss = size_tile * SS
    rng_local = rng
    if spec["kind"] == "palmate":
        pts = _palmate_outline(720, spec["lobes"], spec["sinus_depth"],
                               rng_local)
    else:
        pts = _ovate_outline(1440, spec["aspect"], spec["peak_bias"],
                             spec["serr_depth"], spec["serr_freq"], rng_local)
    poly = _polygon_pixels(pts, size_tile, spec["margin_frac"])

    canvas = Image.new("RGBA", (size_ss, size_ss), (0, 0, 0, 0))

    # Alpha mask from the filled polygon
    mask = Image.new("L", (size_ss, size_ss), 0)
    ImageDraw.Draw(mask).polygon(poly, fill=255)

    # Albedo: gradient + mottling, masked to the leaf
    albedo = _base_albedo(size_ss, spec["hue"], rng_local)
    albedo_img = Image.fromarray(
        (np.clip(albedo, 0, 1) * 255).astype(np.uint8), "RGB").convert("RGBA")
    _draw_veins(ImageDraw.Draw(albedo_img), poly, size_ss, rng_local,
                spec["kind"])

    # Margin darkening: eroded inner ring slightly darker
    inner = mask.filter(ImageFilter.MinFilter(2 * SS + 1))
    shade = Image.new("RGBA", (size_ss, size_ss), (0, 0, 0, 70))
    albedo_img = Image.alpha_composite(albedo_img, Image.composite(
        shade, Image.new("RGBA", (size_ss, size_ss), (0, 0, 0, 0)), mask))

    out = Image.composite(albedo_img,
                          Image.new("RGBA", (size_ss, size_ss), (0, 0, 0, 0)),
                          mask)
    # Alpha: leaf = opaque; AA comes free from the SS downsample later
    out.putalpha(mask)
    return out


def gen_atlas(size=1024, seed=7):
    rng = np.random.default_rng(seed)
    tile = size // 2
    atlas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    for idx, spec in enumerate(_species_params()):
        t = draw_tile(spec, tile, rng)
        x = (idx % 2) * tile
        y = (1 - idx // 2) * tile  # tiles 0,1 = bottom row in UV space
        atlas.paste(t.resize((tile, tile), Image.LANCZOS), (x, y))
    return atlas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="leaf_atlas.png")
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    atlas = gen_atlas(args.size, args.seed)
    atlas.save(args.out)
    print(f"[leaf_atlas] wrote {args.out} ({args.size}x{args.size}, 4 species)")


if __name__ == "__main__":
    main()
