# Rounded Crop — native OBS Studio plugin

Replaces the shader-based version. One filter ("Rounded Crop (Crop, Round, Stroke & Shadow)") with:

- **Crop** — Edges / Center / Aspect Ratio (16:9, 9:16, 1:1, 4:3, 3:4, 21:9, 4:5, 5:4, Custom), Align, and Center Offset. The output really resizes to the crop (no stretching, no transparent leftovers).
- **Corner shape** — Rounded Rectangle or Squircle; one pixel-space radius so corners never skew.
- **+ Add Stroke / + Add Drop Shadow** buttons (up to 6 of each). Each layer has its own "Remove this layer" button. Only the layers you add are shown.
- **Auto canvas padding** — the filter grows its output automatically so strokes, shadows and glows are never clipped. No Expand fields to fiddle with.
- **Style Presets** — built-in Neon Tube, Soft Glow, Long Shadow, Outline Pop (driven by Accent Color / Size / Intensity), plus **Save Preset** / **Delete Selected Saved Preset** for your own looks. Saved presets are stored as JSON in your OBS plugin config folder and capture the shape, stroke and shadow settings.

## Install (Windows)

1. Open the repo's **Releases** page (or the latest successful run under **Actions**, then Artifacts) and download `obs-rounded-crop-windows-x64.zip`.
2. Unzip it into `C:\Program Files\obs-studio\` so that `obs-plugins` and `data` merge with the existing folders. Restart OBS.

## Build locally (Visual Studio 2022 + CMake)

`cmake --preset windows-x64`, then `cmake --build --preset windows-x64 --config RelWithDebInfo`, then `cmake --install build_x64 --config RelWithDebInfo --prefix dist`.

Every push builds automatically on GitHub Actions; pushing a tag (e.g. `v1.0.0`) publishes a Release with the zip attached.

## Use

Right-click a source → Filters → **+** → **Rounded Crop (Crop, Round, Stroke & Shadow)**.

## Verification status

The C source compiles warning-free against the current OBS headers, and `data/rounded_crop.effect` parses cleanly with OBS's own effect parser. It has **not** been run inside OBS (I can't launch it here), so expect to report small runtime issues after the first test.
