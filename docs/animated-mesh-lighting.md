# Animated mesh lighting

Notes on the `fixAnimatedMeshLighting` patch in `src/bf/skinning.cpp`.

## The problem

`SkinningShader2Bones.vs` skins a vertex position by blending two bone matrices, but it never
applies a bone rotation to the normal. The lighting term is constant per vertex, so the lit side of
an animated mesh is welded to the geometry and swings around with the animation instead of staying
towards the sun. Two more things are wrong with the normal the game writes: it is rotated by a
fixed `(x,y,z) -> (-x,z,y)`, built from the globals at `0x009AB6C0` and `0x009AB700`, that
positions never get, and about half of them are not unit length.

A normal cannot simply be rotated by one bone. Vertices near a joint are influenced by two, and one
normal cannot be expressed in two bone spaces at once.

## The fix

The vertex is moved to the usual skinning formulation, where it is expressed in a single space and
the matrices carry the difference between the bind pose and the current pose:

| | before | after |
| --- | --- | --- |
| `v7`, `v8` | bone local offsets | mesh space bind positions, `offset * M_bind` |
| `v3` | a normal in a stray frame, bone independent | plain mesh space normal |
| `c[12 + 3*slot]` | the bone's own matrix `M_cur` | delta `M_bind^-1 * M_cur` |

Positions come out the same, because `(offset * M_bind) * (M_bind^-1 * M_cur)` is `offset * M_cur`.
That also means `skinningShadowGen`, which reads the same vertex buffer and the same constants but
only uses positions, is unaffected.

One mesh space normal is now valid for both bones, so the shader blends it exactly like the
position:

```
mov a0.x, r3.x
dp3 r4.x, VERTEX_NORMAL, c[a0.x+BONE_START_1]
dp3 r4.y, VERTEX_NORMAL, c[a0.x+BONE_START_2]
dp3 r4.z, VERTEX_NORMAL, c[a0.x+BONE_START_3]
mul r5.xyz, r4.xyz, WEIGHTS.x
... same again for bone 1, accumulated with mad ...
dp3 r6.x, r5.xyz, r5.xyz
rsq r6.x, r6.x
mul r5.xyz, r5.xyz, r6.x
dp3 r1.x, r5.xyz, -LOCAL_SPACE_LIGHT_DIR.xyz
```

## Recovering the bind pose

The skin file stores each influence as a bone local offset, which is all the engine needs to place
a vertex, so nothing records how a bone was oriented relative to the mesh. The skeleton is no help
either: at vertex buffer build time its bone array holds an unrelated pose, and none of its bones
maps the offsets onto the mesh.

It is recoverable from the vertex data. For a vertex with a single influence, `offset * M_bind` is
the mesh position by construction, so eight or more such vertices pin `M_bind` down by least
squares, needing no engine state at all. A bone is fitted as soon as it has enough equations, and
offsets already written for it are rebased on the spot, since the vertex buffer stays locked for
the whole build loop. Fits are rejected unless they reproduce known vertices.

Bones used only by blended vertices collect no equations and stay unfitted. Their offsets keep the
game's convention and their constant is left as the bone's own matrix, which is consistent for
position but leaves their lighting as it was.

## Known limitation: soldier arms

Soldier arms are not lit correctly yet, seen on Galactic Conquest soldiers. Legs, torso and rigid
objects come out right, so this is specific to how arms are weighted rather than something wrong
with the patch as a whole.

The likely reasons, roughly in order of confidence:

- **Arm bones never get fitted.** Only vertices with a single influence give an equation, and arms
  are the most heavily blended part of a soldier, so their bones can collect nothing to fit from
  and are left in the game's own convention. This alone would explain it.
- **Blended vertices carry usable equations that are not being used.** For a two bone vertex,
  `o1 * M1 = (p_source - w0 * (o0 * M0)) / w1` once `M0` is known, so fitting could spread outwards
  from the bones that fit directly. Any such scheme has to bound the fitted translation, since a
  large one costs float precision in the rebasing and shows up as stray vertices at the joints.
- **A partly fitted joint mixes conventions.** If one of a vertex's two bones is fitted and the
  other is not, the blended normal picks up one correct term and one that was never rebased.
- **Linear blend skinning does not preserve normals exactly.** Even with both bones fitted, blending
  two rotated normals is an approximation, so a sharply bent elbow will not be perfect.

## The three hooks

| Address | Purpose |
| --- | --- |
| `0x005B1F0C` | tail of the per vertex loop in `buildVertexBuffer`, where the skinned and unmatched branches rejoin. Fits bind poses, rebases offsets, rewrites the normal. `edi` is the vertex offset, `[esp+0x101f4]` the vertex buffer base, `esi` the mesh template, `ebp` and `[esp+0x10]` the source vertex, `[esp+0x10204]` the lod. |
| `0x005B0D2C` | the bone constant buffer is assembled and about to be uploaded. Replaces each bone matrix with the delta. Buffer at `esp+0xd0`, `ebx` is the bone count, `ebp` the object being drawn. |
| `0x00674BBA` | `IDirect3DDevice8::CreateVertexShader`, inside the function that loads a shader by name. That name is its first argument and is still on the stack, at `[esp+0x120]` once the five call arguments are pushed, so one hook both recognises the shader and replaces `pFunction` at `[esp+8]`. |

Bind poses are keyed on mesh template, lod and bone slot, which is what both the build and the
render side can name. A slot with no recorded bind pose is left alone, which is correct: nothing was
rebased against it.

## Where the shader comes from

`shaders.rfa` is left untouched. The shader is carried in `src/bf/skinningshader2bones.h` as a raw
D3D token stream, always hiding the shader from the rfa file.

## Regenerating the embedded shader

The shader source sits next to the header it generates, at `src/bf/SkinningShader2Bones.vs`. It is
DICE's original with the normal handling added, and is not part of the build: it is assembled by
hand with nvasm and converted to the header, which is what the code actually compiles against.

`-d` leaves out the debug info, without it nvasm pads the shader with `D3DSIO_COMMENT` tokens that
D3D ignores anyway:

```
nvasm src\bf\SkinningShader2Bones.vs -s -d
powershell -File tools\vso2header.ps1 -Source SkinningShader2Bones.vso ^
    -Destination src\bf\skinningshader2bones.h -Name g_skinningShader2BonesTokens
```

The converter is a straight dword dump of the file, it only checks that the stream starts with a
vertex shader version token and ends with `D3DSIO_END`.

## Reference

| | |
| --- | --- |
| `0x005B14C0` | `buildVertexBuffer`, fills the 64 byte skinned vertex |
| `0x005B0BEC` | start of the bone constant buffer assembly |
| `0x006746D0` | vertex declaration builder: v0 FLOAT3, v1 FLOAT3, v3 FLOAT3, v5 D3DCOLOR, v7 FLOAT3, v8 FLOAT3 |
| `0x009AB6C0`, `0x009AB700` | the two globals forming the stray normal rotation |
| mesh template `+0x298` | `+0x50` is 1 when the game treats the mesh as skinned |
| mesh template `+0x2A4` | slot to bone index table, `+ lod*0x70 + slot*4` |
| mesh template `+0x704` | bone count per lod |
