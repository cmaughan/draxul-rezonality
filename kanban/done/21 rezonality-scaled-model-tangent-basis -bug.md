# Transform model shading vectors with baked scale
**Severity:** HIGH  
**Source:** Codex #22; `plugins/rezonality/src/model_loader.cpp:211`.

Nonuniform scale changes positions but leaves normals and tangent-space vectors unchanged, producing incorrect lighting and normal mapping.

**Investigation**

- [x] Trace imported vertex bases through baked scale and shared renderer uniforms.

**Fix strategy**

- [x] Apply inverse-transpose normal transformation and transform/reorthogonalize tangent vectors.
- [x] Define safe behavior for singular and mirrored scales.

**Acceptance criteria**

- [x] A normal-mapped model scaled by `(1,2,3)` has the expected shading on Windows/Vulkan.
- [x] Confirm the same scaled-model shading on macOS/Metal.
- [x] Run Rezonality aggregate tests, relevant render checks, and same-cache smoke.

**Progress (Windows):** CPU import now bakes normals by inverse scale,
transforms/reorthogonalizes tangents, preserves source handedness, and flips
bitangent handedness for mirrored scales. Non-finite and singular scales reject
the candidate with an actionable model error. A real Assimp sloped-OBJ test
passed 43 assertions across the two model cases, including `(1,2,3)`, mirror,
and singular behavior. Vulkan and Metal consume the same `ModelVertex` buffers;
the native Vulkan target compiled. The Debug all-products aggregate passed
49/49 CTest entries, including Rezonality render snapshots; same-cache Debug
startup passed via `py do.py run debug --console -- --smoke-test` (~48 s).
The fixed 30 s `smoke --skip-build` wrapper timed out on the existing nine-pane
Session. The user subsequently compared the bundled PBR robot at `(1,1,1)`
and `(1,2,3)` in the actual Windows/Vulkan plugin and confirmed that the
scaled-model shading looks correct.

**macOS Metal gate (2026-09-26):** All seven Rezonality Metal render snapshots
passed. I copied the bundled PBR robot project to a temporary directory,
changed its model scale to `(1,2,3)`, exported a native Metal render-test frame,
and visually inspected the lit metallic model. The nonuniformly scaled robot
rendered without inverted, black, or missing shading; the shared Assimp basis
tests also passed in the 47/47 all-products macOS unit inventory.
