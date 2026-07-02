# OpenAnsel Technical Read — Issues & Answers

## Issue 1 — Hook Stopped Working

### Root cause

The dummy-device approach created a device with `D3DCREATE_SOFTWARE_VERTEXPROCESSING` and different `D3DPRESENT_PARAMETERS` than the game. The vtable could point at a different internal implementation class than the game's actual device. The 5-second sleep was also unreliable — UE3 games may not create the D3D9 device until after loading screens.

### Fix applied

Replaced the dummy-device approach with **export-level hooking**:

1. Hook `Direct3DCreate9` / `Direct3DCreate9Ex` exports via `GetProcAddress` + `MH_CreateHook`.
2. When the game calls `Direct3DCreate9`, hook `IDirect3D9::CreateDevice` (vtable index 16) on the returned object.
3. When `CreateDevice` runs, hook the game's **actual device** vtable — `Present`, `EndScene`, `SetTransform`, `SetVertexShaderConstantF`.
4. Also hook `IDirect3DSwapChain9::Present` from the device's swap chain.

The init thread now polls for `d3d9.dll` to be loaded (up to 60s) instead of using a fixed sleep. This gives the exact vtable the game is using with zero flag-mismatch risk.

### Answers

- **Are you checking `MH_STATUS`?** Yes, with `ok &=` and logging on every `MH_CreateHook` / `MH_EnableHook`.
- **Which Present are you hooking?** Both `IDirect3DDevice9::Present` (vtable 17) and `IDirect3DSwapChain9::Present` (vtable 3), obtained from the game's actual device.
- **What changed?** Eliminated the dummy device entirely. Hooks are now installed on the game's real device, obtained through the `Direct3DCreate9` → `CreateDevice` chain.

---

## Issue 2 — Black/Blank Capture

### Root cause

Two bugs in `ReadBackTile`:

1. **Hardcoded `rowBytes = desc.Width * 4`** — assumed 4 bytes per pixel regardless of the actual backbuffer format. If the game uses a non-32-bit format, the row copy miscalculates.
2. **Hardcoded `DXGI_FORMAT_B8G8R8A8_UNORM`** in the `AddTile` call — regardless of the actual `desc.Format`. If the backbuffer is `D3DFMT_X8R8G8B8` or another format, DirectXTex misinterprets the bytes.

### Fix applied

- Added `BytesPerPixel(D3DFORMAT)` helper that maps D3D9 formats to their byte sizes.
- Added `D3DFormatToDXGI(D3DFORMAT)` helper that maps D3D9 formats to the correct DXGI equivalents.
- `ReadBackTile` now uses `desc.Format` to compute `rowBytes` dynamically and passes the correct DXGI format to `AddTile`.

### Answers

- **What swap effect are you using?** The game controls this; the copy reads the resolved backbuffer regardless.
- **Copy before or after original `Present`?** Before.
- **Map on the same thread?** Yes — all hooks run on the game's render thread.
- **MSAA resolve gap?** Not present — `GetRenderTargetData(resolved, staging)` is called after `StretchRect`.

---

## Issue 3 — Tiling Architecture (UE3 / Shader Pipeline)

### Root cause

Mirror's Edge is UE3, which uses the **programmable pipeline** (vertex shaders). UE3 uploads projection matrices via `SetVertexShaderConstantF`, **not** `SetTransform`. The original `SetTransform` hook never fired after frame one, so every tile captured the same unmodified frame — producing four identical screenshots.

### Fix applied

Added a `SetVertexShaderConstantF` hook (vtable index 83) with a two-phase approach:

1. **Scanning phase** (first frame of capture): scan all `SetVertexShaderConstantF` calls for a 4-register block that matches the heuristic shape of a D3D9 left-handed projection matrix (`LooksLikeProjectionMatrix`). Once found, store the register index and extract frustum parameters.
2. **Injection phase** (subsequent frames): when the game writes to the identified register, replace the 4 float4 constants with the per-tile projection matrix.

The `SetTransform` hook is still kept for fixed-function games — both paths feed into the same `ExtractProjectionParams` / `BuildAndApplyTileMatrix` helpers.

### Additional fixes in image stitching

- **`AddTile` input copy**: was `memcpy(srcImg->pixels, pixels, height * srcImg->rowPitch)` which could read past the input buffer if `rowPitch > width * bpp`. Fixed to row-by-row copy using the source's actual width.
- **`AddTile` output copy**: was copying `tileImg->rowPitch` bytes per row (including padding) into the stitched image, which could write garbage into adjacent tiles. Fixed to copy `tileImg->width * bytesPerPixel` per row.

---

## Architecture Question

**Are you inside the game process as an injected DLL, or external?**

Injected DLL. The injector uses `CreateRemoteThread` + `LoadLibraryW` to load `openansel_dx9.dll`.

**Do you have hooks on draw calls, or only on `Present`?**

Hooks on `Present`, `EndScene`, `SetTransform`, and `SetVertexShaderConstantF`. No `Draw` / `DrawIndexed` hooks. The per-tile matrix is injected at both the fixed-function (`SetTransform`) and shader (`SetVertexShaderConstantF`) levels, covering both rendering paths.
