"""USD import audit — round 1 (measurement, 2026-09-15).

Renders the hand-authored torture fixtures in data/scenes/usd_import/ through
the offline HydraRenderer (one SUBPROCESS per fixture: the HD renderer plugin
is a process-global singleton) and checks region-probe expectations.

Fixture semantics are pre-validated against USD's own composition engine
(payload/variant/reference/inherits/timeSamples all compose correctly — see
the audit notes), so any probe failure below is a renderer-side finding.

Fixtures marked gap=True encode a KNOWN GAP and assert the CURRENT broken
behavior (suite stays green while the gap is present; if the gap gets fixed
the assert fails loudly and should be flipped to the regression form).
Round-1 gaps (sphere gprims pruned entirely) were CLOSED 2026-09-16: gprims
are now declared + tessellated; round-2 additions cover capsule tessellation,
the authored/default axis token, and time-sampled gprim radius.
Round-1 surprise: time-sampled values (points/xform/material/light) DO flow
through render(time_code) — the earlier "time-0 ingestion" static reading
was a misdiagnosis of the 0.0f shutter-offset parameter.

Outputs: Binaries/<T>/test_output/usd_import_audit/<name>.json + PNGs +
manifest.md (written by the last test).
"""
import json
import subprocess
import sys
import time
from pathlib import Path

import pytest

from conftest import TEST_OUTPUT_DIR

TESTS_DIR = Path(__file__).resolve().parent
DATA_DIR = TESTS_DIR / "data" / "scenes" / "usd_import"
OUTPUT_DIR = Path(TEST_OUTPUT_DIR) / "usd_import_audit"
CHILD = TESTS_DIR / "usd_import_audit_render.py"
CHILD_TIMEOUT_SEC = 240


def _r(res, frame, region):
    return res["frames"][frame]["regions"][region]


def _c(res, frame, region):
    return res["frames"][frame]["regions"][region]["mean"]


def _lum(res, frame, region):
    return res["frames"][frame]["regions"][region]["lum"]


# name -> (file, frames, gap, checks)
# checks: list of (description, fn(result) -> bool)
FIXTURES = {
    "preview_surface": ("preview_surface.usda", ["0"], False, [
        ("finite", lambda r: r["frames"]["0"]["finite"]),
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center blue-dominant", lambda r: _c(r, "0", "center")[2] > 2 * _c(r, "0", "center")[0]),
    ]),
    "xform_multiop": ("xform_multiop.usda", ["0"], False, [
        ("finite", lambda r: r["frames"]["0"]["finite"]),
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.02),
        ("center lit", lambda r: _lum(r, "0", "center") > 0.03),
    ]),
    "variant_authored": ("variant_authored.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center red-dominant (red variant selected)",
         lambda r: _c(r, "0", "center")[0] > 2 * _c(r, "0", "center")[2]
         and _c(r, "0", "center")[0] > 0.04),
    ]),
    "variant_no_selection": ("variant_no_selection.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center neutral gray (fallback binding)",
         lambda r: abs(_c(r, "0", "center")[0] - _c(r, "0", "center")[1]) < 0.1
         and abs(_c(r, "0", "center")[1] - _c(r, "0", "center")[2]) < 0.1
         and _c(r, "0", "center")[1] > 0.03),
    ]),
    "payload_external": ("payload_external.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center green-dominant (payload composed)",
         lambda r: _c(r, "0", "center")[1] > 2 * _c(r, "0", "center")[0]),
    ]),
    "payload_nested": ("payload_nested.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center blue-dominant (nested payload composed)",
         lambda r: _c(r, "0", "center")[2] > 2 * _c(r, "0", "center")[0]),
    ]),
    "reference_chain_a": ("reference_chain_a.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.02),
        ("center orange (root override wins)",
         lambda r: _c(r, "0", "center")[0] > 1.2 * _c(r, "0", "center")[1]
         and _c(r, "0", "center")[0] > 2 * _c(r, "0", "center")[2]),
        ("ball offset right (mid-layer xform)",
         lambda r: (r["frames"]["0"]["bright_centroid_col"] or 48) > 48),
    ]),
    "point_instancer": ("point_instancer.usda", ["0"], False, [
        ("left instance visible", lambda r: _lum(r, "0", "left_mid") > 0.02),
        ("right instance visible", lambda r: _lum(r, "0", "right_mid") > 0.02),
        ("dark gap between instances",
         lambda r: _lum(r, "0", "center_col")
         < 0.35 * max(_lum(r, "0", "left_mid"), _lum(r, "0", "right_mid"))),
    ]),
    "materialx_surface": ("materialx_surface.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center green-dominant (mtlx network)",
         lambda r: _c(r, "0", "center")[1] > 2 * _c(r, "0", "center")[0]),
    ]),
    "materialx_roughness": ("materialx_roughness.usda", ["0"], False, [
        ("object visible (no crash, generation succeeds)",
         lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center green-dominant (legacy roughness aliased)",
         lambda r: _c(r, "0", "center")[1] > 2 * _c(r, "0", "center")[0]),
    ]),
    "inherits_binding": ("inherits_binding.usda", ["0"], False, [
        ("object visible", lambda r: r["frames"]["0"]["coverage"] > 0.03),
        ("center purple (binding from class)",
         lambda r: _c(r, "0", "center")[0] > 1.5 * _c(r, "0", "center")[1]
         and _c(r, "0", "center")[2] > 1.5 * _c(r, "0", "center")[1]),
    ]),
    "purpose_proxy": ("purpose_proxy.usda", ["0"], False, [
        ("render cube visible", lambda r: _c(r, "0", "center")[0] > 0.04),
        ("proxy cube hidden (purpose filtered)",
         lambda r: _lum(r, "0", "right_edge") < 0.35 * _lum(r, "0", "center")),
    ]),
    # --- time-sampled fixtures: REGRESSION asserts (round-1 measurement
    # showed render(time_code) DOES flow time through to geometry, xform,
    # material params and light intensity — the earlier "time-0 ingestion"
    # reading was a misdiagnosis of the 0.0f shutter-offset parameter) ---
    "anim_points": ("anim_points.usda", ["0", "24"], False, [
        ("frames differ", lambda r: max((p["mse"] for p in r["mse_pairs"]), default=0.0) > 0.01),
        ("quad grows (points time samples applied)",
         lambda r: r["frames"]["24"]["coverage"] > 2 * r["frames"]["0"]["coverage"]),
    ]),
    "anim_xform": ("anim_xform.usda", ["0", "24"], False, [
        ("frames differ", lambda r: max((p["mse"] for p in r["mse_pairs"]), default=0.0) > 0.01),
        ("blob moves left -> right (transform time samples applied)",
         lambda r: (r["frames"]["0"]["bright_centroid_col"] or 48) < 48
         < (r["frames"]["24"]["bright_centroid_col"] or -1)),
    ]),
    "anim_material": ("anim_material.usda", ["0", "24"], False, [
        ("red at t0", lambda r: _c(r, "0", "center")[0] > 1.5 * _c(r, "0", "center")[2]),
        ("blue at t24 (material param time samples applied)",
         lambda r: _c(r, "24", "center")[2] > 1.5 * _c(r, "24", "center")[0]),
    ]),
    "anim_light": ("anim_light.usda", ["0", "24"], False, [
        ("brighter at t24 (light intensity time samples applied)",
         lambda r: _lum(r, "24", "center") > 1.5 * _lum(r, "0", "center")),
    ]),
    "sphere_static": ("sphere_static.usda", ["0"], False, [
        ("sphere silhouette occludes dome (gprim tessellated)",
         lambda r: _lum(r, "0", "bg_corner") - _lum(r, "0", "center") > 0.3),
        ("center blue-dominant",
         lambda r: _c(r, "0", "center")[2] > 1.5 * _c(r, "0", "center")[0]),
    ]),
    "capsule_static": ("capsule_static.usda", ["0"], False, [
        ("finite", lambda r: r["frames"]["0"]["finite"]),
        ("capsule occludes dome (hemisphere caps tessellated)",
         lambda r: _lum(r, "0", "bg_corner") - _lum(r, "0", "center") > 0.3),
    ]),
    "gprim_axes": ("gprim_axes.usda", ["0"], False, [
        ("finite", lambda r: r["frames"]["0"]["finite"]),
        ("default-Z cylinder occludes center",
         lambda r: _lum(r, "0", "bg_corner") - _lum(r, "0", "center") > 0.3),
        # A default-axis (Z) cylinder seen from +Z is a short disc; if the
        # tessellation wrongly built it along +Y it would darken the top
        # quarter of the frame.
        ("default-Z axis not rendered tall (top strip stays dome)",
         lambda r: _lum(r, "0", "top_edge") > 0.5 * _lum(r, "0", "bg_corner")),
    ]),
    "anim_gprim": ("anim_gprim.usda", ["0", "24"], False, [
        ("frames differ", lambda r: max((p["mse"] for p in r["mse_pairs"]), default=0.0) > 0.01),
        ("radius time sample applied (bigger sphere occludes right sky)",
         lambda r: _lum(r, "0", "right_mid") - _lum(r, "24", "right_mid") > 0.3),
    ]),
}

GAP_MSE_TOL = 1e-7


def _run_child(fixture, filename, frames):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    stage = DATA_DIR / filename
    if not stage.exists():
        pytest.fail(f"fixture missing: {stage}")
    cmd = [sys.executable, str(CHILD), str(stage), str(OUTPUT_DIR),
           "--frames", ",".join(frames), "--name", fixture]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=CHILD_TIMEOUT_SEC)
    elapsed = round(time.time() - t0, 1)
    json_path = OUTPUT_DIR / f"{fixture}.json"
    if proc.returncode != 0 or not json_path.exists():
        pytest.fail(
            f"render subprocess failed ({elapsed}s)\nstderr:\n{proc.stderr[-2000:]}")
    return json.loads(json_path.read_text()), elapsed


def _apply_checks(result, checks):
    failed = []
    for desc, fn in checks:
        try:
            ok = bool(fn(result))
        except Exception as e:  # probe missing / shape mismatch
            ok = False
            desc = f"{desc} (probe error: {e})"
        if not ok:
            failed.append(desc)
    return failed


@pytest.mark.parametrize("fixture", sorted(FIXTURES))
def test_usd_import_fixture(fixture, request):
    filename, frames, gap, checks = FIXTURES[fixture]
    result, elapsed = _run_child(fixture, filename, frames)

    # stamp audit outcome into the JSON for the manifest
    result["audit"] = {"elapsed_sec": elapsed, "gap": gap, "failed_checks": []}

    if gap:
        # Known-gap tripwire: checks assert the CURRENT broken behavior.
        failed = _apply_checks(result, checks)
        result["audit"]["failed_checks"] = failed
        if failed:
            result["audit"]["status"] = "GAP-CLOSED"
            (OUTPUT_DIR / f"{fixture}.json").write_text(json.dumps(result, indent=2))
            assert False, (
                f"GAP CLOSED for {fixture}: {failed}\n"
                f"Flip these checks to the regression form (assert the now-"
                f"correct behavior). probes: {json.dumps(result['frames'])[:1500]}")
        result["audit"]["status"] = "GAP-CONFIRMED"
    else:
        failed = _apply_checks(result, checks)
        result["audit"]["failed_checks"] = failed
        result["audit"]["status"] = "PASS" if not failed else "FAIL"
        (OUTPUT_DIR / f"{fixture}.json").write_text(json.dumps(result, indent=2))
        assert not failed, (
            f"{fixture}: failed checks: {failed}\n"
            f"probes: {json.dumps(result['frames'], indent=1)[:3000]}\n"
            f"see {OUTPUT_DIR / fixture}.png")

    (OUTPUT_DIR / f"{fixture}.json").write_text(json.dumps(result, indent=2))


def test_zz_manifest():
    """Aggregate per-fixture JSON results into manifest.md (must run last)."""
    rows = []
    for fixture in sorted(FIXTURES):
        p = OUTPUT_DIR / f"{fixture}.json"
        if not p.exists():
            rows.append((fixture, "MISSING", ""))
            continue
        r = json.loads(p.read_text())
        status = r.get("audit", {}).get("status", "NO-AUDIT")
        extra = ""
        if status.startswith("GAP"):
            extra = "; ".join(c for c, _ in FIXTURES[fixture][3])
        elif r["audit"].get("failed_checks"):
            extra = "; ".join(r["audit"]["failed_checks"])
        rows.append((fixture, status, f"{r['audit'].get('elapsed_sec', '?')}s {extra}"))

    lines = ["# USD import audit — round 1 (2026-09-15)", "",
             "| fixture | status | detail |", "|---|---|---|"]
    lines += [f"| {n} | {s} | {d} |" for n, s, d in rows]
    n_gap = sum(1 for _, s, _ in rows if s.startswith("GAP"))
    n_fail = sum(1 for _, s, _ in rows if s == "FAIL" or s == "MISSING")
    lines += ["", f"{len(rows)} fixtures: {len(rows) - n_fail - n_gap} pass, "
              f"{n_gap} known gap confirmed, {n_fail} fail.", "",
              "Static-composition ground truth (USD's own composition engine):",
              "all payloads/variants/references/inherits/timeSamples compose",
              "correctly — every render-side failure above is a renderer finding."]
    (OUTPUT_DIR / "manifest.md").write_text("\n".join(lines))
    print(f"manifest written: {OUTPUT_DIR / 'manifest.md'}")
