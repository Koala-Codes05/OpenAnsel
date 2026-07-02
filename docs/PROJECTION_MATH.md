# Projection Math for Tiled Rendering

## Goal

Render a single high-resolution image by dividing it into an `N×M` grid of tiles. Each tile is rendered at the **native** resolution of the backbuffer using a shifted projection matrix, while the **camera position and field of view stay exactly the same**.

## Full Frustum

For a standard left-handed perspective projection (DirectX style):

- `fovY` = vertical field of view (radians)
- `aspect` = width / height
- `n` = near plane distance
- `f` = far plane distance

```
top    = n * tan(fovY / 2)
bottom = -top
right  = top * aspect
left   = -right
```

The full projection matrix (row-major) is:

```
[ 2n/(r-l)     0           0            0 ]
[ 0            2n/(t-b)    0            0 ]
[ (l+r)/(r-l)  (t+b)/(t-b) f/(f-n)    1 ]
[ 0            0          -fn/(f-n)     0 ]
```

## Sub-Frustum for One Tile

For tile `(tx, ty)` where `tx = 0..N-1` and `ty = 0..M-1`:

```
tileWidth  = (right - left) / N
tileHeight = (top - bottom) / M

tileLeft   = left  + tx * tileWidth
tileRight  = tileLeft + tileWidth
tileBottom = bottom + ty * tileHeight
tileTop    = tileBottom + tileHeight
```

The projection matrix for this tile is the same formula as above, but using `tileLeft`, `tileRight`, `tileBottom`, `tileTop` instead of `left`, `right`, `bottom`, `top`.

## What This Achieves

- Each tile is rendered at the full native resolution of the game's backbuffer.
- The tile's frustum covers only a portion of the full scene, but because it maps to the full NDC range `[-1, 1]`, it fills the entire backbuffer.
- The camera position, orientation, and field of view are unchanged; only the frustum is offset.
- The final stitched image is `N × nativeWidth` by `M × nativeHeight`.

## Recovering the Original Frustum from a D3D9 Matrix

When we intercept the game's projection matrix in `SetTransform(D3DTS_PROJECTION)`, we can recover the original parameters from the matrix elements (row-major):

```
A = m[0][0] = 2n / (r-l)
B = m[1][1] = 2n / (t-b)
C = m[2][0] = (l+r) / (r-l)
D = m[2][1] = (t+b) / (t-b)
E = m[2][2] = f / (f-n)
F = m[3][2] = -fn / (f-n)

n = -F / E
right = n * (C + 1) / A
top   = n * (D + 1) / B
fovY  = 2 * atan(top / n)
aspect = right / top
f = E * n / (E - 1)
```

These recovered values are used to build the per-tile sub-frustums.

## Implementation: Clip-Space Tile Matrix (Current Approach)

The current DX9 backend uses a simpler and more robust approach: instead of recovering frustum parameters and building an off-axis projection, we **post-multiply** a clip-space NDC-scale+offset matrix onto the camera's existing VP matrix.

Given a clip-space (row-major) VP matrix `M`, we compute:

```
T = clip-tile matrix for tile (tx, ty) in an NX×NY grid
M' = M × T
```

Where `T` is defined as:

```
T = [ NX    0    0  0 ]
    [  0   NY    0  0 ]
    [  0    0    1  0 ]
    [  ox   oy   0  1 ]

ox = NX - 2*tx - 1       (shift in NDC x)
oy = 2*ty + 1 - NY       (shift in NDC y; DX9 NDC has +y = up)
```

This post-multiply shifts the NDC-space tile into the full backbuffer.

### Why this approach?

- No need to recover `fovY`, `aspect`, `near`, `far` from the matrix elements.
- Works with **any** projection matrix (perspective, orthographic, custom).
- Does **not** require knowing the original frustum parameters.
- The camera position, orientation, FOV, and near/far planes remain exactly the same.
- Handled identically for both fixed-function (`SetTransform`) and shader (`SetVertexShaderConstantF`) paths.

### VP Matrix Heuristic

In the programmable-pipeline path, we need to find which shader constant register holds the VP matrix. The heuristic (`LooksLikeVPMatrix`) checks whether the third column (camera forward vector in a row-major VP) has unit length within ±10%:

```
forwardZ = d[3]² + d[7]² + d[11]²
0.81 ≤ forwardZ ≤ 1.21  ⇒  VP matrix candidate
```

### `GetVertexShaderConstantF` Interception

UE3 reads the VP matrix back each frame via `GetVertexShaderConstantF` for CPU-side frustum culling and LOD selection. If it reads our tiled matrix, it sees geometry outside the shifted frustum and aborts rendering (Present never fires). The `GetVertexShaderConstantF` hook intercepts these reads and returns the **original** (un-tiled) VP matrix stored during the last `SetVertexShaderConstantF` call.

See `src/math/tile_projection.h` for the implementation of `BuildClipTileMatrix`.

## Example: 2×2 Tiles

If the native resolution is 1920×1080 and we want 2×2 tiles:

- Tile `(0, 0)` covers the upper-left quadrant of the scene, rendered at 1920×1080.
- Tile `(1, 0)` covers the upper-right quadrant, rendered at 1920×1080.
- Tile `(0, 1)` covers the lower-left quadrant, rendered at 1920×1080.
- Tile `(1, 1)` covers the lower-right quadrant, rendered at 1920×1080.

The final stitched image is 3840×2160.
