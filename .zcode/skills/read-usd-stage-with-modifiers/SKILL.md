---
name: read-usd-stage-with-modifiers
description: Inspect a Ruzino USD stage combined with its modifier sidecar layer, using the bundled pxr (USD) + python in Binaries/Release. Use whenever the user wants to read, dump, inspect, or compare a .usdc/.usda stage together with its matching _modifiers sidecar file (e.g. "what's in stage.usdc", "show me the modifiers", "dump the USD", "compare stage vs modifier", "why is this prim's value X"). Triggers on any mention of Ruzino stage files, modifier files, pxr, usdcat, or inspecting USD content in this repo.
---

# Read a Ruzino USD stage with its modifier sidecar

Ruzino stores per-stage edits in two cooperating files. To inspect a stage
*as the app sees it*, you must combine them.

There is no canned script — read the mechanism below, copy the minimal snippet,
and adapt it to the actual question (full tree? one prim? animation?).

## What the two files are

- **Stage file** — e.g. `Assets/stage.usdc`. The root layer. Holds geometry,
  cameras, etc.
- **Modifier sidecar** — the persistent copy of the stage's **session layer**.
  Holds per-stage editing opinion (overrides, attribute edits) on top of the
  root. This is what the user-authored modifications look like on disk.

### Naming rule (do not guess it)

For a stage at `dir/<stem>.<ext>`, the modifier sidecar is:

```
dir/<stem>_modifiers.<ext>
```

So `stage.usdc` → `stage_modifiers.usdc`. **Same extension**, underscored
`_modifiers`. Hardcoded in `Stage::get_modifier_layer_path`
(`source/Runtime/stage/source/stage.cpp:583`).

Watch out for look-alikes in the same folder:
- `<stem>.modifiers.<ext>` (dot, not underscore) is a **different** file and is
  **not** auto-loaded. Don't assume it's the modifier.
- `<stem>_modifiers.<random>` / `<stem>.<random>` (random suffix) are USD's
  atomic-write temp files from the last save. Ignore them.

When in doubt, compute the path from the rule above and check it exists.

## How the app combines them (replicate this to read faithfully)

From `Stage::load_modifier_layer` (`stage.cpp:632`):

1. `Sdf.Layer.FindOrOpen(modifier_path)`
2. `layer.ExportToString()` → the layer's USDA text
3. `session_layer.ImportFromString(text)` — the modifier is **folded into the
   stage's session layer**, the **strongest** opinion on the stage.

So to see the composed result: open the stage with a fresh session layer, then
import the modifier into it. The session layer sits **above** the root and wins
on conflicts.

## Environment setup (required, otherwise import fails)

The pxr bindings and `usd_ms.dll` live in `Binaries/Release`:

1. `cwd` / `sys.path[0]` = `Binaries/Release` (so `import pxr` resolves).
2. `PXR_USD_WINDOWS_DLL_PATH` = `Binaries/Release` (so `_usd.pyd` finds
   `usd_ms.dll` on Windows).
3. Run with `Binaries/Release/python.exe`.

Mirrors `source/tests/conftest.py`. If `Binaries/Release/python.exe` is missing,
the python copy step didn't run — fix once:

```
python -c "import os; os.chdir(r'<repo>'); import sys; sys.path.insert(0,'.'); from configure import copy_python_dlls_to_binaries; copy_python_dlls_to_binaries(['Release'])"
```

## Minimal snippet (copy this, then adapt)

Run via `Binaries/Release/python.exe`. The preamble (env setup + combination)
is the part to keep verbatim; the exploration at the bottom is where you pick
the slice the task needs.

```python
import os, sys
from pxr import Usd, Sdf

# --- env setup (verbatim) ---
repo = r"C:\Users\Jerry\WorkSpace\Ruzino"
binaries = os.path.join(repo, "Binaries", "Release")
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = binaries
sys.path.insert(0, binaries)
os.chdir(binaries)

# --- combination (mirrors Stage::load_modifier_layer) ---
stage_path = os.path.join(repo, "Assets", "stage.usdc")
d, stem, ext = os.path.split(stage_path)[0], *os.path.splitext(os.path.basename(stage_path))
mod_path = os.path.join(d, stem + "_modifiers" + ext)  # stage.usdc -> stage_modifiers.usdc

session = Sdf.Layer.CreateAnonymous("session")
stage = Usd.Stage.Open(Sdf.Layer.FindOrOpen(stage_path), session)
if os.path.exists(mod_path):
    file_layer = Sdf.Layer.FindOrOpen(mod_path)
    session.ImportFromString(file_layer.ExportToString())

# --- exploration (adapt to the question) ---
def walk(prim, depth=0):
    print("  " * depth + f"{prim.GetPath()} [{prim.GetTypeName()}] "
          f"({len(prim.GetPrimStack())} layer-op)")
    for c in prim.GetChildren():
        walk(c, depth + 1)

walk(stage.GetPseudoRoot())

# Examples — pick what the task needs, drop the rest:
p = stage.GetPrimAtPath("/mesh_0")
for a in p.GetAuthoredAttributes():
    val = a.Get()
    n = a.GetNumTimeSamples()
    print(f"  {a.GetName()} = {val!r}" + (f"  [{n} timeSamples]" if n else ""))

# Is the modifier overriding this prim? 2 = yes (root + modifier), 1 = no.
print("mesh_0 layer-op:", len(stage.GetPrimAtPath("/mesh_0").GetPrimStack()))
```

**`layer-op` count** = how many layers author that prim: 1 = root only, 2 = root
+ modifier contributes an opinion. The cheapest way to find what the modifier
touches.

## How to decide what to dump (don't dump everything)

Match the task:
- "Is the camera where I expect?" → only `/FreeCamera`'s `xformOp:transform` /
  `third_person:*`.
- "What did the modifier change?" → iterate prims, keep those where
  `len(prim.GetPrimStack())` differs from the no-modifier case; or just run with
  the combination and look for the 2-layer-op prims.
- "What's mesh_0's geometry?" → only that prim's `points`/`faceVertex*`.
- "Is there animation?" → `attr.GetNumTimeSamples()` / `attr.GetTimeSamples()`,
  not the (huge) values.
- Big arrays (`points`, `faceVertexIndices`) bloat output — print `len(...)` and
  a few samples, not the whole array, unless the task needs every value.

## What the modifier actually stores (observed, not assumed)

Dumped directly from `Assets/stage_modifiers.usdc` (via `usdcat.exe`):

- It `over`s **one** prim (`over Mesh "mesh_0"`) — overrides opinions on an
  existing prim, defines no new prims.
- Re-authors mesh_0's geometry: `faceVertexCounts`, `faceVertexIndices`,
  `normals`, `points`, `primvars:UVMap` (interpolation="vertex").
- **Each attribute is written both as a default value *and* as
  `.timeSamples { 0: ... }` — identical.** Animation structure with one frame
  (time code 0). Matches the root-layer `.Animatable (bool)` flag: enabling
  animatable editing routes per-frame opinion into the session layer, which
  persists to the modifier sidecar.

So a 2-layer-op prim in the composed dump means the modifier is holding its
**timeSampled** version. To tell real motion apart from the "animatable but
single-frame" init, use `attr.GetTimeSamples()`.

## Tools without python

`Binaries/Release/usdcat.exe` / `usdtree.exe` link `usd_ms.dll` directly (add
`Binaries/Release` to PATH). They read a **single layer** — they will **not**
show the modifier-composed result, but they're the fastest way to dump one file
to text. To inspect the modifier alone (not composed), point `usdcat.exe` at
`<stem>_modifiers.<ext>`.
