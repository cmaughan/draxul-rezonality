# Rezonality

Rezonality is the Draxul-native continuation of
[VkLive](https://github.com/cmaughan/vklive): a fault-tolerant live graphics
viewer for shader and scene editing. It will render inside a Draxul pane using
Vulkan on Windows and Metal on macOS while projects are edited from any other
terminal or editor pane.

The repository currently contains the buildable dynamic-plugin scaffold and
the researched port plan. The live renderer is the next functional slice.

```text
draxul tab create --space <space-id> --name Rezonality \
  --plugin dev.draxul.rezonality --json
```

When project loading lands, instances will accept a bounded JSON configuration
like this:

```json
{
  "project_path": "D:/art/my-shader-project",
  "scenegraph": "default.scenegraph",
  "auto_reload": true
}
```

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

