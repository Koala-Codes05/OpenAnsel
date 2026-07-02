# OpenAnsel — Future Roadmap & Technical Q&A

End-to-end vision for a full Ansel-like capture pipeline:

```
OpenAnsel
  ↓
Pause game
  ↓
Free camera
  ↓
Hide HUD
  ↓
Depth export
  ↓
Normals export
  ↓
Motion vectors
  ↓
Lighting buffers (where available)
  ↓
High resolution capture  ← implemented
  ↓
Color grading
  ↓
AI restoration
  ↓
Publish
```

---

## 1. Pause Game

**Q: What does "pause game" mean in this context?**
Freeze the game's simulation loop (physics, animations, AI) while keeping the render loop alive so the user can compose a shot.

**Q: Why is this needed for tiled capture?**
Without pausing, geometry may shift between tiles, producing seams or motion artifacts in the stitched result. It also lets the user frame the perfect moment before capturing.

**Q: How can we pause from an injected DLL?**
The most reliable approach is to suspend the game's main thread(s) via `CreateToolhelp32Snapshot` + `SuspendThread`, but this risks deadlocks if a suspended thread holds a lock the render thread needs. A safer approach is `WaitForSingleObject` on a manual-reset event: the hotkey sets the event, and the game's main loop checks it each frame.

**Q: What's the current status?**
`CaptureConfig::pauseGame` exists as a `bool` field but is never read anywhere in the codebase. Zero implementation.

**Q: What's the difficulty estimate?**
Medium (3–5 days). The hard part is handling thread suspension safely across different engines.

---

## 2. Free Camera

**Q: What is free camera?**
Detach the camera from the player character and let the user orbit, pan, and zoom freely to compose a shot from any angle.

**Q: Why is this hard in a generic hook DLL?**
Camera position, rotation, and FOV are set per-engine. There is no universal D3D9 call that reveals "camera position." In UE3, the view matrix is built from the player controller's location/rotation — you'd need to read engine memory offsets. Each game/engine would need its own plugin or offset database.

**Q: Is there a generic fallback?**
Partially. You can intercept the view matrix after `SetTransform(D3DTS_VIEW)` or from `SetVertexShaderConstantF` and override it with a custom matrix. This lets you rotate/zoom but not move the camera through world space — moving requires knowing the camera position, which is encoded in the view matrix's inverse (the last row/column). You can extract it and add an offset, but you'll lack collision and physics.

**Q: How does NVIDIA Ansel do it?**
Ansel requires per-game integration by the developer — it reads the engine's camera data through a SDK. There is no generic solution.

**Q: Difficulty estimate?**
Very hard (weeks to months). A game-specific offset database would be needed for broad support. A limited "rotate around current point" free-cam is more feasible as a generic hook.

---

## 3. Hide HUD

**Q: Why not just capture before the HUD is drawn?**
The HUD is typically composited in the same backbuffer as the scene. `GetRenderTargetData` captures the fully-composited frame. You'd need to suppress HUD draw calls or render targets.

**Q: What approaches exist?**
1. **Orthographic matrix displacement** (experimental, in `.bak`): HUD elements are usually rendered with an orthographic projection matrix. Detect orthographic matrices and push them off-screen by modifying their X/Y translation. This worked partially but was removed as unstable.
2. **Draw-call filtering**: Intercept `DrawIndexedPrimitive` / `DrawPrimitive` and skip calls that use orthographic matrices or known HUD shaders. Very fragile.
3. **Render-target switching**: Redirect the game's HUD render target to a dummy surface. Requires tracing the render pipeline.
4. **Per-game plugins**: Hide specific HUD elements by name/class if the game exposes them (e.g., UE3's `SetHUDVisible`).

**Q: Current status?**
`CaptureConfig::disableHud` is a placeholder. The orthographic-displacement approach showed promise but was removed. No active implementation.

**Q: Difficulty estimate?**
Medium–Hard (1–2 weeks for a heuristic approach; per-game for reliable results).

---

## 4. Depth Export

**Q: What is depth export?**
Capture the depth buffer (Z-buffer) alongside the color backbuffer to produce a depth map image.

**Q: How would this work in DX9?**
`IDirect3DDevice9::GetDepthStencilSurface` retrieves the depth/stencil surface. You can then `GetRenderTargetData` (or `GetDC` for system-memory access) to read it back. The depth values are typically in the range `[0, 1]` (or `[near, far]` depending on the projection).

**Q: What format would the depth map be saved as?**
16-bit grayscale PNG or 32-bit float EXR for higher precision. The depth values need to be linearized (recovered from the non-linear Z-buffer using the projection matrix's near/far parameters).

**Q: Is depth buffer access feasible as a generic hook?**
Yes, this is one of the more achievable features. The depth buffer is a standard D3D9 resource, and the API for accessing it is well-defined. The main challenge is that some games use `D3DUSAGE_DEPTHSTENCIL` with no CPU readback capability — you may need to create a copy via `CreateDepthStencilSurface` + `UpdateSurface`.

**Q: Current status?**
Not started.

**Q: Difficulty estimate?**
Medium (3–5 days for basic depth capture; another week for linearization and output).

---

## 5. Normals Export

**Q: What is normals export?**
Capture the screen-space surface normals for each pixel, producing a normal map image.

**Q: How can we get normals generically?**
This is very difficult without engine cooperation:
- **Deferred renderers** store normals in a G-buffer render target. You'd need to identify which RT holds normals (by size, format, shader usage) — fragile and game-specific.
- **Forward renderers** don't store normals anywhere after the frame is drawn. The only way is to re-render the scene with a normals-only shader, which requires replacing shaders — essentially impossible as a generic hook.

**Q: How does Ansel do it?**
Ansel requires the game to provide normals through its SDK. Even Ansel cannot extract normals generically.

**Q: Current status?**
Not started. This is arguably the hardest item on the roadmap for a generic solution.

**Q: Difficulty estimate?**
Very hard (weeks to months, per-game). May not be feasible as a generic hook at all.

---

## 6. Motion Vectors

**Q: What are motion vectors?**
Per-pixel velocity data indicating how each pixel moved between the current and previous frame. Used for motion blur, temporal anti-aliasing, and AI upscaling.

**Q: How are motion vectors stored?**
In modern engines, motion vectors are rendered to a separate render target (usually R16G16 half-float format). Like normals, finding this RT generically is very difficult.

**Q: Is there a fallback?**
You could compute optical flow between consecutive frames on the CPU or GPU, but this is expensive and less accurate than engine-provided motion vectors.

**Q: Current status?**
Not started.

**Q: Difficulty estimate?**
Very hard (weeks to months). Similar to normals — essentially requires per-engine integration.

---

## 7. Lighting Buffers (where available)

**Q: What are lighting buffers?**
The intermediate render targets used by a deferred renderer: diffuse, specular, emissive, ambient occlusion, shadow maps, etc. Compositing these allows relighting the scene in post-processing.

**Q: Can we access these generically?**
Only in the most fragile way. Deferred renderers output to multiple render targets (MRT). You could enumerate all render targets bound via `SetRenderTarget` and capture each one, but identifying *which* RT is which (diffuse vs specular vs normal) requires heuristics or per-game knowledge.

**Q: What's the practical approach?**
Capture all render targets during a frame and save them all, letting the user sort out which is which. This is brute-force but technically feasible.

**Q: Current status?**
Not started.

**Q: Difficulty estimate?**
Hard (1–2 weeks for brute-force MRT capture; per-game mapping for meaningful labeling).

---

## 8. High Resolution Capture (already implemented)

See `README.md` and `docs/PROJECTION_MATH.md` for the current implementation. N×M tiled capture via clip-space matrix post-multiplication, GPU readback, gamma correction, and PNG stitching.

**Planned improvements:**
- Streaming stitcher (write tiles to disk incrementally) for >50K resolution outputs
- Async GPU readback (avoid stalling the render thread)
- Worker-thread stitching
- 32-bit float output
- TIFF output
- Fix EXR output (unblocked when vcpkg's `openjph` port builds on x86-windows)

---

## 9. Color Grading

**Q: What is color grading in this context?**
Apply a LUT (lookup table) or tone-mapping curve to the captured image before saving, or save raw linear data with a companion color-grade LUT for later adjustment.

**Q: What would we implement?**
The simplest approach is a set of post-processing presets (warm/cool/vintage/monochrome) applied via DirectXTex's `TransformImage` with a custom function. More advanced: load external 3D LUT files (.cube).

**Q: Current status?**
Not started.

**Q: Difficulty estimate?**
Easy (2–3 days for basic LUT support; a week for .cube file loading).

---

## 10. AI Restoration

**Q: What does AI restoration mean?**
Apply ML-based upscaling, denoising, or inpainting to the captured image to improve quality beyond what the game natively renders.

**Q: What's the technical approach?**
1. **ONNX Runtime** integration for running pre-trained models (e.g., ESRGAN for upscaling, Real-ESRGAN for denoising).
2. **DirectML** as an alternative for Windows-native GPU inference.
3. Run as a post-capture step (not real-time) to avoid frame-time impact.

**Q: Is this realistic for an open-source project?**
Yes, but it adds significant complexity:
- Requires bundling model files (large: 50–500 MB each)
- ONNX Runtime adds ~30 MB to the binary
- GPU memory usage spikes during inference
- Legal/licensing considerations for model weights

**Q: Current status?**
Not started. No ML dependencies in the project.

**Q: Difficulty estimate?**
Medium (1–2 weeks for basic ONNX Runtime integration; weeks more for model selection, tuning, and UI).

---

## 11. Publish

**Q: What does "publish" mean?**
Save the final image (already done — PNG), optionally with sidecar files (depth map, normal map, metadata JSON), or upload to a sharing service.

**Q: What could be added?**
- Batch export: save all capture layers (color + depth + normals + motion vectors + lighting buffers) in a single operation
- Metadata export: camera position, FOV, tile config, timestamp in a companion JSON file
- Direct sharing to platforms (future, not core)

**Q: Current status?**
Basic save-to-PNG is implemented. Multi-layer export depends on the other features being built first.

---

## Implementation Priority (Recommended)

| Priority | Feature | Depends On | Effort |
|----------|---------|------------|--------|
| P0 | Fix EXR output | vcpkg `openjph` fix | Low |
| P1 | Pause game | — | Medium |
| P2 | Hide HUD (ortho displace) | — | Medium |
| P3 | Depth export | — | Medium |
| P4 | Color grading | — | Easy |
| P5 | Free camera (limited) | — | Hard |
| P6 | Lighting buffers (MRT capture) | — | Hard |
| P7 | AI restoration | — | Medium |
| P8 | Normals export | Per-game plugin system | Very Hard |
| P9 | Motion vectors | Per-game plugin system | Very Hard |
