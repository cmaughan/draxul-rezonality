# Suppress unchanged Rezonality editor refresh work

**Summary:** Refresh Rezonality's editor diagnostics only when they change so idle Neovim instances avoid repeated file reads and error-list updates.

**Priority:** P2 — unchanged editor diagnostics cause avoidable polling and UI work.

**Source:** `plugins/rezonality/integrations/neovim/rezonality.nvim/lua/rezonality/init.lua`  
**Reported by:** Claude; static, medium-high confidence. Lines 29–32 default to 500 ms diagnostics and 2 s registry refresh; lines 650–684 reread/rebuild and spawn `draxul pane list`; lines 1071–1072 install the repeating timer. Every installed editor instance can do this while unchanged.

- [x] **Baseline:** Count file reads, `vim.diagnostic.set` calls, spawned commands, and idle CPU per editor instance.
  - Confirmed in current source. Headless macOS probe, default intervals, two records, one shader buffer, 10 s idle: 40 JSON reads, 24 `vim.diagnostic.set`, 24 `vim.diagnostic.reset`, 4 `draxul pane list` spawns, 28 ms Neovim CPU.
- [x] **Implement:** Gate unchanged diagnostics by reliable file revision/notification; retain bounded registry refresh unless topology events replace it.
  - Each tick stats the records (inode, size, mtime, ctime; the publisher replaces records by rename) and rereads only new or changed ones; removal is detected from the listing. Buffers republish only when their diagnostic signature changes (`BufReadPost` and `:RezEnable` still force). The 2 s registry poll is retained; identical CLI output, or an unchanged unavailable reason, skips the rebuild. `:RezRefresh` still rereads every record.
- [x] **Functional safety:** Check diagnostics arrival, pane disappearance, file replacement, and timer teardown.
  - `draxul-rezonality-neovim` (`tests/rezonality_neovim_test.lua`) now runs a 20 ms/60 ms auto-refresh phase: an atomically replaced record arrives in the open buffer, a closed pane drops its instance and diagnostics, and `:RezDisable` stops the timer, clears diagnostics, and does no further reads or registry work.
- [x] **Compare:** Require no unchanged diagnostics reset while preserving prompt topology updates.
  - Same probe after the change: 0 JSON reads, 0 set, 0 reset, 4 spawns (bounded registry kept), 8 ms CPU. The integration test asserts zero idle reads/sets/resets with registry polling still active; against the old `init.lua` it observes 124 reads, 82 sets, and 123 resets and fails.
- [x] **Platforms:** Check Neovim integration on Windows and macOS with the corresponding CLI.
  - macOS: the headless integration test passed (registry provider) and the idle probe exercised the spawned `pane list` path through a stand-in CLI. Windows (2026-10-08): `draxul-rezonality-neovim` passed in 4.27 s in the parent's core + Rezonality aggregate; paired fresh-profile same-cache Debug smoke passed. [Shared validation evidence and unrelated failures](01%20rezonality-compiler-adapter%20-refactor.md#windows-closure-evidence-2026-10-08).
- [x] **Acceptance:** Idle editors avoid repeated diagnostics work without losing pane changes.

Final Release confirmation (parent, 2026-10-08): same-cache smoke retry passed, exit 0, 33.866647 s including toolchain setup; log `build-ninja-debug/windows-gates/final-release-smoke-retry.log`. The first attempt failed during resource-exhausted MSVC environment setup (68.059 s), before app startup; this was an environmental failure.
