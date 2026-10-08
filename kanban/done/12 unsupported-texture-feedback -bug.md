# Reject unsupported previous-frame and aliased inputs

**Summary:** Reject unsupported feedback inputs so scenes cannot read a texture while writing the same output.

**Priority:** P1  
**Source:** `plugins/rezonality/src/live_project.cpp`

**Evidence and trigger:** B26; the parser accepts previous-frame sampling, but both backends resolve it to the current target texture.

- [x] **Investigate:** Traced sampler flags and target identity through both native backends. The parser records `!name` as `Sampler::previous_frame`. Neither backend reads that flag. Metal (`native_backend_metal.mm`) maps each sampler name to a surface index and binds the surface's current `texture`; Vulkan (`native_backend_vulkan.cpp`) writes the surface's current `attachment.view` into the pass descriptor set. Neither keeps a history copy, so `!A` reads the texture being rendered. Neither rejects a pass that samples one of its own targets, including the implicit `default_color` target.
- [x] **Fix:** `parse_scene_text` now runs `validate_pass_inputs` before asset resolution or preparation. It rejects previous-frame samplers and any sampler named among the same pass's targets, and reports the pass header line. Both backends are protected because candidates reach them only through this parser.
- [x] **Acceptance:** `Rezonality rejects texture feedback before preparation` covers four cases: an earlier pass's output stays a valid input, `!surface` is rejected, a pass sampling its own target is rejected, and sampling the implicit `default_color` is rejected. Each failure has a diagnostic naming the pass and surface. `The staged Rezonality module rejects texture feedback edits` drives the real module through valid g1, then previous-frame g2 and aliased g3 `BUILD FAILED` candidates at `default.scenegraph:17` and `:7`, then repaired g4. Both failures are rejected while parsing and never reach native preparation. `RuntimeController` keeps the active generation on every failed result, which the existing runtime tests cover. The contract harness records no frames, so it cannot show "rendering last good" itself.
- [x] **Validation:** macOS `--rezonality` aggregate (Metal, including the staged-example inventory and goldens) and same-cache smoke. The change is CPU-only and runs before either backend, so Vulkan gets identical behavior and Windows CI covers it without a separate gate.

**Model:** `gpt-6.1-sol`
