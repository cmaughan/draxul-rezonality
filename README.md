# Rezonality

Rezonality is the Draxul-native continuation of
[VkLive](https://github.com/cmaughan/vklive): a fault-tolerant live graphics
viewer for shader and scene editing. It will render inside a Draxul pane using
Vulkan on Windows and Metal on macOS while projects are edited from any other
terminal or editor pane.

The first two live-renderer slices are implemented. Rezonality parses ordered
VkLive-style passes and named color, float, depth, and texture surfaces. It
renders pane-sized intermediate targets, binds the common uniform block and
samplers, then composites through Draxul's continuation target. Saving any
scenegraph, shader, include, or texture dependency triggers a debounced rebuild.
A bad edit is reported while the complete last-known-good generation keeps
rendering.

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
  "compile_debounce_ms": 150
}
```

Omit `project_path` to open the bundled `examples/simple` project. Bundled
`default`, `blend_waves`, `deferred_shading`, and `protoplanetary_disc`
projects exercise depth, MRT, float targets, ordered sampling, and texture
upload. The protoplanetary demo runs its preserved VkLive raymarch and
composite shaders directly; only its model/normal overlay awaits the model
slice. Its active live-edit sources are
`examples/protoplanetary_disc/vklive-original/screen.frag` and
`examples/protoplanetary_disc/vklive-original/copy.frag`; the obsolete
top-level approximation shaders have been removed. Other original VkLive
source remains under each project's `vklive-original/` directory. Animated shaders receive frame-safe common
uniforms at approximately 60 Hz while visible; Space pauses/resumes them. Use the
pane action **Reload Rezonality Project** to bypass the debounce and force a
rebuild. The status progresses through `building`, `ready`, and `live`; errors
include the attempted generation and source location.

Typical live-edit check:

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality \
  --plugin-config '{"project_path":"D:/art/my-shader-project"}' --json
```

Edit `screen.frag` from another Draxul terminal and save it. No embedded editor
or application menu is involved.

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

