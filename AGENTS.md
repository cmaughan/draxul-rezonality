# Rezonality Agent Guide

Rezonality is the Draxul-native continuation of VkLive. It is mounted in a
Draxul checkout at `plugins/rezonality` and builds against Draxul's public
plugin SDK and allowlisted `Draxul::PluginSupport::*` leaves.

When this repository is mounted in Draxul, read the parent repository's
`CLAUDE.md` before changing code. Its cross-platform, plugin-boundary,
validation, and vertical-slice rules apply here.

Product-specific rules:

- Rezonality owns its scene language, shader compiler, renderers, examples,
  assets, tests, plans, and tracker. Do not move product code into Draxul.
- Keep Vulkan and Metal behavior paired unless a plan explicitly records a
  temporary platform gap.
- Preserve imported VkLive code where practical. Prefer narrow adapters around
  Draxul's lifecycle and borrowed GPU objects over broad rewrites.
- Draxul owns the window, swapchain/drawable, command submission, and
  presentation. Rezonality owns only resources it creates and commands it
  records through the plugin frame callbacks.
- Failure-tolerant editing is the primary product invariant: a failed reload
  must report diagnostics and keep the last successfully rendered generation.
- Prefer real dynamic-module integration tests, render snapshots, and
  edit/reload smoke scenarios. Add isolated unit tests only where they protect
  parser/compiler edge cases or make hard failure paths deterministic.
- Work items live under `kanban/`; designs and research live under `plans/`.

## Build and validation

Run Rezonality through the parent Draxul checkout so the actual module is
built, staged, dynamically loaded, and exercised across the public ABI. Do not
replace this with a standalone mock executable.

Windows development loop:

```text
py do.py build debug
py do.py test debug --rezonality
py do.py smoke debug --skip-build
```

macOS development loop:

```text
python3 do.py build debug
python3 do.py test debug --rezonality
python3 do.py smoke debug --skip-build
```

Use the same cache for the aggregate and smoke. Use Release only for final
confirmation, optimized/render behavior, or a Release-only failure:

```text
py do.py test release --rezonality
py do.py smoke release --skip-build
```

The `--rezonality` scope is deliberately opt-in. It runs core coverage plus
the real Rezonality native-module contract/edit tests and registered Rezonality
render goldens. Core-only runs must not require this submodule, its compiler,
or its assets. When the submodule/target is absent, Rezonality tests and render
cases must not be registered.

For a completed Rezonality slice, run one `test ... --rezonality` aggregate and
one same-cache smoke. Focused CTest filters are for diagnosis and iteration;
do not repeat them immediately before the aggregate unless they materially
reduce risk. Report configure, compilation, aggregate, render, smoke, and
unavailable-platform costs separately.

## Render goldens

Rezonality render tests use Draxul's hidden render-test harness: they create no
interactive window, load the staged DLL/module through the ABI, render a fixed
paused frame, save it, and compare it with the platform reference image.
Render tests must remain conditional on `draxul-rezonality-plugin` being
available.

- Add a deterministic render manifest under the parent repository's
  `tests/render/` and register it in `tests/render/manifest.json`.
- Keep Windows Vulkan and macOS Metal reference images separate. Bless on the
  platform being represented; never copy a Windows golden to macOS and claim
  Metal coverage.
- A rendering change needs its affected Rezonality render scenario in the
  aggregate. Use the generated `do.py` bless command rather than invoking
  `--render-test` directly.
- Golden comparisons complement, rather than replace, live-edit recovery
  tests: a static image cannot prove candidate rollback or file watching.

The audio spectrum golden must use `"audio_source":"synthetic"`; render tests
must never depend on microphone permission, ambient sound, or an installed
device. Manual live-audio checks use `"audio_source":"input"`. Rezonality
shares live capture per exact recording-device selection and pauses/clears it
when all subscribers are hidden.

## Live-edit and failure-recovery checks

Exercise live editing against a copied fixture, never by destructively editing
the staged plugin payload or checked-in example in-place. A meaningful vertical
test uses the real module and proves this sequence:

1. load the project and observe a ready/live generation;
2. make a valid shader or scene edit and observe a newer generation;
3. introduce invalid GLSL, scene syntax, or a missing asset;
4. verify diagnostics identify the attempted generation while the last valid
   generation remains active;
5. restore the file and verify a still-newer generation becomes ready/live.

Automatic watching is the normal user path. The `rezonality_reload`
presentation action bypasses debounce and is the deterministic force-reload
path for tests. A Draxul launch uses the staged copy under the selected build
directory; editing a source checkout only reloads when `project_path` points to
that checkout explicitly.

For agent-driven work, prefer the checked-in layout generator over assembling
an editor/view split piecemeal:

```text
py plugins/rezonality/tools/rezonality_layout.py --project D:/path/to/project | draxul layout apply - --json
```

Capture the returned `editor` and `view` aliases. The generated plugin config
also contains a stable `diagnostics_id`. Read the corresponding bounded JSON
from the Rezonality plugin cache at `diagnostics/<diagnostics_id>.json`; use
`attempted_generation`, `active_generation`, `severity`, `path`, `line`, and
`message` to distinguish success, rollback, and repair without scraping the
rendered pane. Repeat `--project` to create multiple independent views. The
`draxul-rezonality-agent-layout` isolated-server integration test protects this
layout and terminal-control contract.

The bundled Windows compiler is `tools/win/glslangValidator.exe`; the macOS
payload is `tools/mac/glslangValidator`. Shader-stage suffixes are part of the
project contract (`.vert`, `.frag`, and ray-stage suffixes such as `.rgen`,
`.rmiss`, and `.rchit`). Include all watched project dependencies in the
fingerprint so a save cannot silently fail to schedule a candidate.

## Platform and GPU behavior

- Vulkan and Metal implementations should land together. Windows validation
  may leave an explicit macOS build/golden/manual gate unchecked, but must not
  silently substitute a raster approximation for a missing Metal path.
- Capability-dependent projects must fail inertly and actionably on unsupported
  hardware, without losing the pane or the last supported active generation.
- Plugins record only into Draxul's borrowed command buffer. They never submit,
  commit, present, retain target objects, or wait the device idle.
- GPU generations retire only after every Draxul frame slot that used them has
  completed. Failed candidates must destroy only their own resources.

## Manual pane check

Launch through a normal Draxul Session and pass a source checkout explicitly
when editing it:

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality \
  --plugin-config '{"project_path":"D:/path/to/project"}' --json
```

Put a terminal/editor beside the view, save a valid change, break it, and
repair it. Resize the split throughout. Confirm the image remains pane-local,
the terminal stays responsive, hidden tabs stop scheduling frames, and orbit,
wheel dolly, pause, and reload affect only the intended instance.

