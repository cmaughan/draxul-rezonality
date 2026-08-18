# Fault-tolerant single-pass live shader

- [x] Preserve and stage VkLive's platform `glslangValidator` tools.
- [x] Stage and load the original `simple` project.
- [x] Parse the single-pass `vs`/`fs` scenegraph subset.
- [x] Compile GLSL to SPIR-V on a per-pane worker thread.
- [x] Detect external file edits with a debounced content fingerprint.
- [x] Render the screen rectangle with Vulkan and translated Metal pipelines.
- [x] Keep the last prepared GPU generation after compile or prepare failure.
- [x] Retire replaced GPU generations as Draxul frame slots complete.
- [x] Expose building, ready, live, and source-located error status.
- [x] Force compilation through the existing Rezonality pane action.
- [x] Exercise valid, broken, and repaired edits through the plugin contract.
- [x] Add a real Draxul render-test scenario.
