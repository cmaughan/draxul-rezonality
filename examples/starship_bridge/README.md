# STARSHIP // BRIDGE

A 2-by-2 starship control panel built from four Rezonality scenes that share
one project directory, a bloom chain and a HUD composite.

## Launch

Build Draxul with Rezonality enabled, then from a terminal pane inside the
target Draxul Space run (Windows: `py` instead of `python3`):

```text
python3 plugins/rezonality/examples/starship_bridge/launch.py
```

Outside Draxul, name the destination explicitly with `--space <id>` (see
`draxul space list --json`). `--draxul`, `--session`, `--server-runtime-dir`,
`--name` and `--paused` are also accepted. Every run creates a new tab and
prints its tab and pane ids.

A single view can also be opened on its own:

```text
draxul tab create --space <space-id> --name Warp --plugin dev.draxul.rezonality \
  --plugin-config '{"project_path":"<repo>/plugins/rezonality/examples/starship_bridge","scenegraph":"warp.scenegraph"}' --json
```

`project.toml` defaults to `helm.scenegraph`.

## Pane map

| | Left | Right |
| --- | --- | --- |
| Top | `helm` — HELM // TACTICAL | `nav` — NAVIGATION |
| Bottom | `warp` — ENGINEERING // WARP CORE | `sensor` — SENSORS // LONG RANGE |

- **helm**: the generated Vanguard-class cruiser (`ship.obj`) as real
  depth-tested geometry with procedural hull plating, emissive engines,
  nacelle plasma, windows, navigation lights and a holographic scan sweep, over
  a nebula and tactical grid with sensor rings and contacts.
- **nav**: a raymarched ice giant with banded storms, aurora, atmosphere,
  mutually shadowing rings, a cratered moon and a plotted trajectory.
- **warp**: a volumetric plasma column with segmented counter-rotating
  constrictor rings, arcs, a reflective deck and a panelled reactor bay.
- **sensor**: a holographic terrain survey drawn as contours and grid,
  revealed by a sweeping scan front, with tethered contacts and a radar scope.

Every pane renders HDR into `Scene`, extracts a quarter-resolution bright
pass, blurs it at eighth resolution and composites bloom, anamorphic streak,
chromatic aberration, ACES tone mapping, scanlines, vignette, grain and a
per-pane HUD (`post_common.glsl`, specialised by `post_<pane>.frag`).

Left-drag orbits, the mouse wheel dollies and Space pauses. Raymarched panes
build their rays from the scenegraph camera in `ray.vert`, so they follow the
camera controls too.

## Regenerating the ship

`ship.obj` is checked in. To change the hull, edit `generate_ship.py` and run
`python3 generate_ship.py`; a visible pane reloads the model automatically.
UV.x carries the material part id used by `ship.frag`.
