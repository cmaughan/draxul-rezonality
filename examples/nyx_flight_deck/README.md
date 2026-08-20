# NYX Flight Deck

NYX is a reproducible 5-by-2 science-fiction flight deck for Draxul. It combines
seven animated Rezonality shader scenes, two low-flicker PowerShell telemetry
feeds, and one true glTF/PBR geometry view.

## Launch

Build Draxul with Rezonality enabled and initialize the Rezonality submodule.
From a terminal pane inside the target Draxul Space, run:

```powershell
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File plugins/rezonality/examples/nyx_flight_deck/launch.ps1
```

When launching outside Draxul, identify the destination explicitly:

```powershell
build-ninja-release/draxul.exe space list --json
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File plugins/rezonality/examples/nyx_flight_deck/launch.ps1 -SpaceId space-28
```

The launcher discovers a local Draxul build automatically. Use `-DraxulPath`
when the executable is elsewhere, and `-Session` or `-ServerRuntimeDir` for a
non-default server route. Every run creates a new `NYX // FLIGHT DECK` tab.

## Pane map

| Column | Top | Bottom |
| --- | --- | --- |
| 1 | Reactor Crown | Neon Sentinel glTF robot |
| 2 | Flight Telemetry | Shield Harmonics |
| 3 | Quantum Polyhedron | Target Lock |
| 4 | Celestial Gyroscope | Plasma Wake |
| 5 | Orpheus Comms | Event Gate |

The shader sources live under `shaders/`; all ten original panel entry points
are retained, including the superseded Singularity scene in `panel-5`. The two
terminal programs live under `terminals/` and redraw in place at 2 Hz and 1 Hz
to avoid flicker, wrapping, and scroll churn.

The Neon Sentinel uses the `robot-crt/` scenegraph and shaders while referencing
the adjacent `../robot2` model rather than duplicating
its large glTF texture set. It is actual depth-tested mesh geometry with animated
vertex rotation, PBR materials, a procedural neon environment, and the same
low-resolution CRT composite used by the procedural monitors. In a visible
Rezonality pane, left-drag orbits, the mouse wheel dollies, and Space toggles
animation.
