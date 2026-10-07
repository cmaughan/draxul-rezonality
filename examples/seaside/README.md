# SEASIDE

A 2-by-2 nature scene: one stretch of coast across a day, built from four
Rezonality scenes that share one project directory, wave/sky helpers and a
filmic bloom composite.

## Launch

Build Draxul with Rezonality enabled, then from a terminal pane inside the
target Draxul Space run (Windows: `py` instead of `python3`):

```text
python3 plugins/rezonality/examples/seaside/launch.py
```

Outside Draxul, name the destination explicitly with `--space <id>` (see
`draxul space list --json`). `--draxul`, `--session`, `--server-runtime-dir`,
`--name` and `--paused` are also accepted. Every run creates a new tab and
prints its tab and pane ids. The launcher runs `generate_models.py` first if
any mesh is missing.

## Pane map

| | Left | Right |
| --- | --- | --- |
| Top | `dawn` - Sea Stacks | `kelp` - Kelp Forest |
| Bottom | `sunset` - Broad Reach | `night` - Lighthouse Point |

- **dawn** (raymarched): eroded limestone sea stacks, a headland and an arch
  with the sun rising through it. Includes mirror-calm analytic swell with
  rock reflections, height mist banks and gulls.
- **kelp** (meshes + volumetrics): a giant-kelp forest swaying in the surge.
  A school of sardines and a few garibaldi share one baked mesh and are
  placed per fish in `fish.vert`. Also caustic-lit sand and rocks, Snell's
  window overhead, god rays integrated to scene depth, marine snow and a
  bubble vent.
- **sunset** (meshes): a sloop on a Gerstner swell. The hull heaves, pitches
  and heels from the same wave train the ocean mesh uses. Includes wind-filled
  sails that glow with transmitted sunlight, a Kelvin wake, glitter path,
  cloud bands and distant islands.
- **night** (meshes + volumetrics): a banded lighthouse and keeper's cottage
  on a basalt headland. Two sweeping beams are integrated through sea mist up
  to a linear-depth target. Includes lantern glare, moon glitter,
  bioluminescent surf, the Milky Way and a distant village shore.

Mesh passes write HDR into `Scene` (and linear depth into `Linear` where a
later volumetric pass needs it). Rezonality has no instancing or per-draw
transforms, so `generate_models.py` bakes per-instance data into vertex colours
and UVs: kelp strand phase and height, fish id and species, sail coordinates.
The vertex shaders animate from that data. The ocean and seabed reuse one
camera-centred polar grid.

Left-drag orbits, the mouse wheel dollies and Space pauses.

## Regenerating the meshes

Edit `generate_models.py` and run `python3 generate_models.py`; a visible pane
reloads the models automatically. `shoreline_radius()` there is mirrored by
`shorelineRadius()` in `night_common.glsl`, which drives the surf.
