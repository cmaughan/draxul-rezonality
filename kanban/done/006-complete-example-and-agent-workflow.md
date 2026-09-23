# Complete example and agent workflow acceptance

- [x] Stage every supported VkLive scenegraph project and include.
- [x] Generate atomic terminal/editor plus Rezonality layouts.
- [x] Publish bounded, atomic, agent-readable diagnostics in plugin cache.
- [x] Expose project and generation state through presentation status.
- [x] Preserve time, pause, and camera state across compatible module reloads.
- [x] Test server layout, terminal driving, valid edit, failure, rollback,
  repair, and native-module state handoff through real boundaries.
- [x] Run the full registered Windows Vulkan render inventory.
- [x] Build and run the aggregate on macOS Metal.
- [x] Create and validate macOS Metal render references for the complete
  inventory.

Implemented in Rezonality 0.7.0. Windows Vulkan and macOS Metal platform
acceptance artifacts are both recorded in the render inventory.

## macOS validation evidence

- 2026-09-23: built the Release application, plugin, and focused Rezonality test
  targets on Apple M5, then passed all 37 selected core and Rezonality CTests.
- Blessed and visually inspected all seven Metal references. Every registered
  Rezonality render comparison passed, followed by a same-cache Release smoke.
