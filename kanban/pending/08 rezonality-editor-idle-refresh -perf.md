# Suppress unchanged Rezonality editor refresh work

**Summary:** Refresh Rezonality's editor diagnostics only when they change so idle Neovim instances avoid repeated file reads and error-list updates.

**Source:** `plugins/rezonality/integrations/neovim/rezonality.nvim/lua/rezonality/init.lua`  
**Priority:** P2; static, medium-high confidence. **Reported by:** Claude. Lines 29–32 default to 500 ms diagnostics and 2 s registry refresh; lines 650–684 reread/rebuild and spawn `draxul pane list`; lines 1071–1072 install the repeating timer. Every installed editor instance can do this while unchanged.

- [ ] **Baseline:** Count file reads, `vim.diagnostic.set` calls, spawned commands, and idle CPU per editor instance.
- [ ] **Implement:** Gate unchanged diagnostics by reliable file revision/notification; retain bounded registry refresh unless topology events replace it.
- [ ] **Functional safety:** Check diagnostics arrival, pane disappearance, file replacement, and timer teardown.
- [ ] **Compare:** Require no unchanged diagnostics reset while preserving prompt topology updates.
- [ ] **Platforms:** Check Neovim integration on Windows and macOS with the corresponding CLI.
- [ ] **Acceptance:** Idle editors avoid repeated diagnostics work without losing pane changes.
