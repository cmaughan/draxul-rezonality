# Rezonality

Rezonality is the Draxul-native continuation of
[VkLive](https://github.com/cmaughan/vklive): a fault-tolerant live graphics
viewer for shader and scene editing. It will render inside a Draxul pane using
Vulkan on Windows and Metal on macOS while projects are edited from any other
terminal or editor pane.

All six planned vertical slices are implemented. Rezonality parses ordered
VkLive-style passes, named surfaces, cameras, and Assimp-backed OBJ/glTF models.
It renders pane-sized intermediate targets, uploads immutable geometry and PBR
material generations, binds HDR and material textures, then composites through
Draxul's continuation target. On ray-capable devices it also builds plugin-owned
BLAS/TLAS resources and renders the preserved Cornell-box project through
Vulkan ray shader groups or the native Metal ray kernel. Saving any scenegraph,
shader, include, model, or texture dependency triggers a debounced rebuild. A
bad edit, missing asset, or unsupported ray capability is reported while the
complete last-known-good generation keeps rendering.

The `audio_spectrum_analysis` project adds a product-owned stereo waveform and
FFT texture. Live instances share one SDL recording stream per selected input
device, so two panes do not open the same microphone twice. Capture pauses and
its queued samples are cleared when every subscribing pane is hidden; showing
one again resumes capture and rendering. Rezonality does not own an audio
output stream or a system-loopback facility in this slice.

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality --json
```

That server mutation form creates a shared plugin tab inside a normal Draxul
Session. A direct `draxul --plugin dev.draxul.rezonality` launch now does the
same thing automatically: it attaches to the selected Session, keeps its shell
tab, creates and focuses a Rezonality tab, and leaves terminal/editor tabs and
splits available. Only Draxul's internal `--render-test` harness uses an
isolated product window.

Instances accept a bounded JSON configuration like this:

```json
{
  "project_path": "D:/art/my-shader-project",
  "scenegraph": "default.scenegraph",
  "auto_reload": true,
  "paused": false,
  "compile_debounce_ms": 150,
  "diagnostics_id": "my-live-view",
  "audio_source": "input",
  "audio_device": ""
}
```

`audio_source` may be `input` (the default recording device), `synthetic` (the
deterministic test signal), or `silent` (an explicit inert fallback). Set
`audio_device` to an exact SDL recording-device name to override the default.
The setting is only used by projects declaring the reserved `AudioAnalysis`
surface. `diagnostics_id` is an optional stable lowercase filename stem for
the agent-readable status described below; it accepts letters, digits, `.`,
`_`, and `-`.

Omit `project_path` to open the bundled `examples/simple` project. Bundled
`default`, `blend_waves`, `deferred_shading`, `protoplanetary_disc`,
`pbr_robot`, `robot2`, `ray_tracer`, and `audio_spectrum_analysis` projects exercise depth, MRT, float targets,
ordered sampling, Assimp model loading, textured PBR materials, HDR
environments, acceleration structures, and native ray dispatch. The
`robot2` variant preserves the neon procedural environment and reflective
lighting experiment while `pbr_robot` remains the original reference scene.
The `examples/nyx_flight_deck` package combines multiple Rezonality projects,
a glTF/PBR scene, and two PowerShell telemetry panes into a reproducible 5-by-2
dashboard. Launch it from the parent Draxul checkout with:

```powershell
pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File plugins/rezonality/examples/nyx_flight_deck/launch.ps1
```

The
protoplanetary demo now runs its preserved VkLive raymarch, sphere overlay, and
composite shaders directly. Its active live-edit sources are
`examples/protoplanetary_disc/vklive-original/screen.frag` and
`examples/protoplanetary_disc/vklive-original/copy.frag`; the obsolete
top-level approximation shaders have been removed. Other original VkLive
source remains under each project's `vklive-original/` directory. Animated
shaders receive frame-safe camera/model/view/projection uniforms at
approximately 60 Hz while visible. Space pauses/resumes animation, left-drag
orbits the active camera, and the mouse wheel dollies. Use the pane action
**Reload Rezonality Project** to bypass the debounce and force a rebuild. The
status progresses through `building`, `ready`, and `live`. A failed candidate
is surfaced in the pane-local status pill as `BUILD FAILED gN`; when a prior
generation remains active it explicitly says `rendering last good gM`, followed
by the source location and compiler message. The indicator persists while that
failed candidate is current and clears after the next successful reload.

Typical live-edit check:

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality \
  --plugin-config '{"project_path":"D:/art/my-shader-project"}' --json
```

Edit `screen.frag` from another Draxul terminal and save it. No embedded editor
or application menu is involved.

For an atomic terminal-plus-view workspace, generate a declarative layout and
pipe it directly to the Draxul server. Repeat `--project` to add more views:

```text
py plugins/rezonality/tools/rezonality_layout.py --project D:/art/my-shader-project | draxul layout apply - --json
```

The generator gives each view a stable `diagnostics_id`. Rezonality atomically
publishes bounded schema-v2 JSON at the plugin cache path under
`diagnostics/<diagnostics_id>.json`. The record retains the primary diagnostic
at the top level for simple consumers and includes a bounded `diagnostics`
array with every compiler-reported file, stage, severity, line/column, and
message. It also contains attempted and active generations, timestamps, and
the last successful render time. Agents can therefore watch a
compile fail, confirm the old active generation remains, repair the source, and
observe the next successful generation without scraping pixels. Plugin module
replacement also preserves the matching project's elapsed time, pause state,
and camera position through Draxul's hot-reload handoff.

## Product boundary

Rezonality owns the scene parser, shader compiler, model and texture loading,
Vulkan/Metal scene renderers, examples, diagnostics, tests, and file watching.
Draxul owns the window, pane layout, input routing, swapchain/drawable, command
submission, presentation, plugin discovery, and command palette.

There is deliberately no embedded source editor, project menu, node-graph
panel, standalone SDL window, or ImGui application shell in this port.

See:

- [VkLive research](plans/vklive-research.md)
- [Vertical-slice port plan](plans/port-plan.md)

## Building

Rezonality builds by default as a submodule mounted at
`plugins/rezonality` in a Draxul checkout. Use the normal Draxul workflow:

```text
py do.py build debug
py do.py test debug --rezonality
py do.py smoke --skip-build
```

The Rezonality scope loads the staged native module through the exported ABI,
copies the real PBR and Cornell-box projects, and drives valid shader edits,
invalid GLSL, scene errors, missing models/textures, and successful recovery.
It also creates an isolated server, applies the generated terminal-plus-view
layout, and drives the terminal through the public pane commands. On Windows
it renders every registered Rezonality scenario—default, waves, deferred,
disc, robot, ray, and audio—and compares them with checked-in references. These checks are opt-in:
core-only tests do not run them, and Draxul does not register the render cases
when the Rezonality submodule/target is absent.

