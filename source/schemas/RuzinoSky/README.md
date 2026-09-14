# RuzinoSky — HosekWilkieSky codeless USD schema

`HosekWilkieSky` is this engine's sky-sun rig prim: a `UsdLuxDomeLight`
derived type (unknown to other applications without this plugInfo, which is
intended) that pairs with a child `UsdLuxDistantLight` whose direction the
editor keeps synced to the sky's `inputs:sunDirection`
(`source/Runtime/stage/source/hosek_sky.cpp`). The renderer treats the two
as fully independent lights and never rewrites authored directions.

## Layout

| file | role |
|------|------|
| `schema.usda.in` | source of truth for the schema definition (template; base-layer paths filled in at regen time) |
| `regen_schema.py` | regenerates `resources/` via `usdGenSchema` (only needed when `schema.usda.in` changes) |
| `resources/plugInfo.json` | checked-in generated artifact — the runtime type registration |
| `resources/generatedSchema.usda` | checked-in generated artifact — prim definitions / attribute fallbacks |

The checked-in artifacts are what builds ship. The regular build has **no**
dependency on usdGenSchema, jinja2, or the SDK source tree.

## Regenerating

```bash
pip install jinja2   # usdGenSchema's template engine
python source/schemas/RuzinoSky/regen_schema.py
```

Env overrides: `RUZINO_SDK_DIR`, `RUZINO_BIN_DIR`.

## Pitfalls (learned 2026-09-07, all verified on this SDK)

- The base schema layers MUST come from the OpenUSD **source** tree
  (`SDK/OpenUSD/source/OpenUSD-*/pxr/usd/{usd,usdLux}/schema.usda`). The
  shipped `generatedSchema.usda` files have their `/GLOBAL` codegen metadata
  stripped (by design — output artifacts carry no recipe), so usdGenSchema
  rejects them with "Could not find the defining layer" / "GLOBAL prim not
  found".
- `inherits` uses the *unqualified* class name: `</DomeLight>`, not
  `</UsdLuxDomeLight>`.
- The `@...@` around sublayer asset paths in `schema.usda.in` is usda
  syntax — the regen script replaces the bare placeholder names only.
- The generated `plugInfo.json` has leading `#` comment lines (invalid
  JSON) and `@PLUG_INFO_*@` placeholders; the regen script fixes both.
  `LibraryPath` is empty and `Type` is `resource` — there is no DLL.
- The schema registry opens schematics at the exact path
  `<ResourcePath>/generatedSchema.usda` — keep it directly under
  `resources/`, not in a subdirectory.
- The `PXR_PLUGINPATH` env var does NOT register plugins with this build's
  monolithic `usd_ms`; discovery only works through the installed layout
  (`<bin>/usd/<plugin>/resources/`, scanned via the root usd plugInfo's
  `"Includes": ["*/resources/"]`).
