# Reuse decoded Rezonality assets across shader-only edits

**Summary:** Reuse unchanged models and images when editing shaders so Rezonality reloads do not repeatedly import the same assets.

**Source:** `plugins/rezonality/src/live_project.cpp`  
**Priority:** P2; static, medium-high confidence. **Reported by:** Claude. `build_candidate()` at lines 1316–1326 reloads models and lines 1357–1385 decode images for every candidate, including a `.frag` edit. The watcher detects the edit, but no decoded-asset cache separates unchanged inputs.

- [x] **Baseline:** Count import/decode calls, reload latency, and peak memory for repeated shader edits in an asset-rich project.
  - `examples/pbr_robot`, macOS Debug, injected compiler: every candidate
    imported 1 glTF model and decoded 1 HDR image (817,338,784 decoded
    bytes, almost all model textures). A cold build and a shader-only
    rebuild both took about 5.5–7.9 s. The pending candidate held a second
    full copy of the decoded assets next to the active build.
- [x] **Implement:** Cache bounded immutable decoded assets by source/options and complete external dependencies.
  - `src/decoded_asset_cache.{h,cpp}`: one entry per model (path, scale,
    texture flip) or image (path, rgba8/rgba32f). Model imports record every
    file Assimp opened or probed plus external texture files through a
    recording `IOSystem` in `load_model()`. An entry is reused only while
    every dependency keeps its stat stamp, and is not kept if any input
    was still racy when decoded.
  - `ShaderBuild::models` now holds `SharedModel` (an immutable
    `shared_ptr<const ModelData>` that converts to `const ModelData&`), so
    reused imports are aliased, not copied. Renderer call sites are
    unchanged. Image pixels are still copied into each surface, with
    retained image bytes capped at 256 MiB.
  - Bounded to the assets of the latest fully resolved scene plus the build
    in progress (`finish_build()`).
- [x] **Functional safety:** Detect same-size edits and model-referenced files; preserve break/repair and last-good generations.
  - `Rezonality reuses decoded assets across shader-only candidates`: a
    same-size `.mtl` edit re-imports only that OBJ and changes its material,
    a broken image fails the candidate while models stay cached and repair
    decodes only the image, a racy input is never retained, and removing a
    model shrinks the cache.
- [x] **Compare:** Require zero unchanged model/image decodes for shader-only edits without unbounded cache growth.
  - Same project: shader-only rebuild 27 ms with 0 model and 0 image
    decodes, versus about 5.5 s before. The rebuilt candidate shares the
    active build's model storage, so its peak model memory drops from 2x to
    1x (about 800 MB saved here). The live watch test checks that a shader
    edit through `LiveProject` reports 1 model and 1 image reuse with no new
    decodes.
- [x] **Platforms:** Check reloads on Vulkan and Metal, aggregate and smoke.
  - The cache is CPU-only, and the backends receive the same
    `const ModelData&`. macOS/Metal aggregate (including the Rezonality
    render goldens and contract tests) and smoke pass locally. The
    Vulkan source is unchanged. Its `create_model(..., build.models[i], ...)`
    call relies on the same implicit `SharedModel` conversion as Metal. It
    cannot be built on macOS, so routine Windows CI covers it.
- [x] **Acceptance:** Shader edits rebuild shader work without reimporting unchanged assets.
