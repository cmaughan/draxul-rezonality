# Rezonality

Rezonality is the Draxul-native continuation of
[VkLive](https://github.com/cmaughan/vklive): a fault-tolerant live graphics
viewer for shader and scene editing. It will render inside a Draxul pane using
Vulkan on Windows and Metal on macOS while projects are edited from any other
terminal or editor pane.

The first live-renderer slice is implemented. Rezonality loads the original
VkLive `simple` project, compiles its GLSL off the UI thread with the preserved
`glslangValidator` tool, and renders its `screen_rect` into the Draxul pane on
Vulkan or Metal. Saving a shader triggers a debounced rebuild. A bad edit is
reported in the pane status while the last successfully prepared GPU pipeline
continues rendering; repairing the file advances to the next generation
without restarting Draxul.

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality --json
```

Use that server mutation form for live editing: it creates a shared plugin tab
inside a normal Draxul Session, where terminal/editor panes remain available.
Launching `draxul --plugin dev.draxul.rezonality` directly creates an explicit
standalone product window; that mode is useful for focused viewing and render
tests, but intentionally has no server-owned shell panes to split into.

Instances accept a bounded JSON configuration like this:

```json
{
  "project_path": "D:/art/my-shader-project",
  "scenegraph": "default.scenegraph",
  "auto_reload": true,
  "compile_debounce_ms": 150
}
```

Omit `project_path` to open the bundled `examples/simple` project. Use the
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

