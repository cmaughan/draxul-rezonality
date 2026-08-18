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

