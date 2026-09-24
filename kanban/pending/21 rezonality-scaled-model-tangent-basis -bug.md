# Transform model shading vectors with baked scale
**Severity:** HIGH  
**Source:** Codex #22; `plugins/rezonality/src/model_loader.cpp:211`.

Nonuniform scale changes positions but leaves normals and tangent-space vectors unchanged, producing incorrect lighting and normal mapping.

**Investigation**

- [ ] Trace imported vertex bases through baked scale and shared renderer uniforms.

**Fix strategy**

- [ ] Apply inverse-transpose normal transformation and transform/reorthogonalize tangent vectors.
- [ ] Define safe behavior for singular and mirrored scales.

**Acceptance criteria**

- [ ] A sloped normal-mapped model scaled by `(1,2,3)` has the expected shading on Vulkan and Metal.
- [ ] Run Rezonality aggregate tests, relevant render checks, and same-cache smoke.
