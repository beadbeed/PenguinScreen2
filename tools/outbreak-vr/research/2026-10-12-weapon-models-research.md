# The game's own weapon models in the VR hands (research, 2026-10-12)

Goal: show Resident Evil Outbreak's own weapon model in the right hand, and a holstered pistol at the right hip,
instead of the procedural shapes in `pcsx2/VR/HandModel.cpp`. Alex approved extracting from his own disc, locally.
Nothing extracted may be committed, uploaded or shared. This note holds no game data: only file names, structure,
counts and rough sizes found by reading his disc read-only.

Discs checked: `D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso` and the File #2 image
next to it (SLPM-65692). The scan scripts live in this session's scratchpad, not in the repo.

## Short answer

- The weapons are in `OBJSxx.NBD`, one per scenario, inside `NETBIO00.DAT` -> `rXXX.afs`. Each file is "every item you
  can pick up in this scenario, low-poly". Each item is one root node of the model's hierarchy, and **the root's group id
  is the item id**: the same ids as Outbreak-Research's item table, which `VR/ItemData` already uses. Mesh *i* belongs to
  root *i*. So no new mapping table is needed.
- The format (NBD = TIM2 textures + AMO mesh + AHI hierarchy) is documented by working open-source code: Fothsid's
  libthanatos and OBTool. Both are GPL-3.0, the same licence as PCSX2. I confirmed the layout byte by byte on Alex's discs.
- A gun is 200 to 350 triangles with one or two 128x128 4-bit textures. A melee weapon is 12 to 230 triangles with a
  256x256 8-bit texture. The CPU rasteriser can draw these with no trouble.
- **Recommended: route (a).** At emulator start, read the OBJS files from the disc the emulator already has open, decode
  them in memory, and draw them as textured triangles in HandModel's rasteriser. Route (b), capturing the game's GS draw,
  doesn't work well here. In first person the game culls the local character, so there is nothing to capture. Even when
  the weapon is drawn, the GS only gets 2D projected vertices posed by the game's animation, not by the controller.
- Hand pick: **Godot XR Tools hands** (`Hand_low_R.gltf` / `Hand_low_L.gltf`), **CC0 1.0**. 938 triangles, rigged with 26
  joints named exactly like OpenXR's `XR_EXT_hand_tracking` joints. Grip, pistol and hold poses come with it.

## 1. Where the weapon models live

### Disc layout (both Files)

The ISO9660 root holds `NETBIO00.DAT`, `NETBIO01.DAT` and (File #1 only) `NETBIO02.DAT`, plus `0FLIST.DIR`, which lists
them. Each `.DAT` is an AFS archive.

AFS layout:
- `"AFS\0"`, then a u32 count.
- `count` x (u32 offset, u32 size), relative to the archive start, 2048-aligned.
- Then (u32 offset, u32 size) of a name table. Some archives put that pointer just before the first file instead.
- The name table has 48-byte records: a 32-byte name, then a date and a size.
- AFS archives nest.

| Archive | File #1 contents | File #2 contents |
|---|---|---|
| `NETBIO00.DAT` | `romdata.afs` (1344 files) and one `rXXX.afs` + `rXXX_h.bin` per scenario: r001, r002, r028, r035, r041, plus `rfff.afs` (lobby/network UI) | `romdata.afs` and r006, r010, r015, r026, r040, plus r020-r023, r027, r029, r030 (practice and Elimination/battle modes) and `rfff.afs` |
| `NETBIO01.DAT` | sound (`adxsnd.afs`, `bgm.afs`, voices, `sXXenv.afs`) | sound |
| `NETBIO02.DAT` | movies (File #1); File #2 keeps its movies elsewhere | |

What `romdata.afs` holds: the character models (`P00_00.nbd` ... `P07_02.nbd` for the eight playable characters; `HP`
= cutscene versions, `EP` = the zombified ones), the enemies (`E*.NBD`), the NPCs (`N*.NBD`, `HN*.NBD`) and their
animations. `pc_*.bin`, `plNN_pc_*.bin` and `sm_*.bin` are motion data. `wpNN.wmd` (120-736 bytes each) look like
small weapon keyframe tables, probably the slide and hammer animation. **No weapon meshes are in romdata.afs**, and the
player models carry no weapon either (Kevin's `P00_00.nbd` is 4 meshes, 3910 triangles, 36 nodes, nothing else).

Each scenario's `rXXX.afs` holds:
- `OBJSxx.NBD`: the gameplay items, weapons included.
- `OBJnn_nn_X_nn.NBD`: high-poly cutscene props. A few weapons are here too, e.g. Raymond's SPAS-12 in r001, but they
  are cutscene versions.
- `EFxx.NBD` and `DEFxx.NBD`: gun effects (shells, splats, muzzle billboards) and similar rigid effect models, no
  weapons.
- Room models (`rXXXNNNN.nbd`), `.sld` room textures, and the `itemNNNNN.tbl` placement tables.

`OBJSxx.NBD` files on the discs:

| File #1 | File #2 |
|---|---|
| r001 `OBJS01.NBD` (58 items), r002 `OBJS02` (47), r028 `OBJS28` (48), r035 `OBJS35` (45), r041 `OBJS41` (53) | r006 `OBJS06` (66), r010 `OBJS10` (68), r015 `OBJS15` (61), r026 `OBJS26` (60), r040 `OBJS40` (57), r020 `OBJS20` (34), r021-r023 (55 each), r027 (56), r029 (64), r030 (60) |

In every one of the 17 files, the number of AHI roots equals the number of meshes.

### Item id -> model

In `OBJSxx.NBD` the AHI root list holds one root per item. The root node's `groupId` is the item id. Mesh *i* in the
AMO mesh list is the item of root *i*. I checked this against sizes and shapes: id 0 Handgun is 22 cm long, 400 Kevin's
.45 is 21 cm, 103 Iron Pipe is 1 m, 106 Long Pole is 1.3 m, 300+ potted herbs are 40 cm tall, 403 Jim's coin is 2 cm
across, and 10150 Newspaper is flat. Common items are repeated in every scenario with identical geometry.

Triangle counts of the weapons found, both discs together. Size is the length of the longest side, rounded.
"Pages" are the texture pages the mesh uses.

| Id | Item (ItemData name) | Tris | Size | Pages | Found in |
|---|---|---|---|---|---|
| 0 | Handgun | 259 | 22 cm | 2 x 128x128 4bpp | most OBJS on both Files |
| 1 | Handgun SG | 250 | 19 cm | 2 x 128 4bpp | F1 OBJS35, F2 OBJS26/29 |
| 2 | Handgun GL | 279 | 20 cm | 128 8bpp | F2 OBJS10/29 |
| 3 | Magnum Handgun | 274 | 27 cm | 128 4bpp | F1 OBJS35, F2 several |
| 4 | Magnum Revolver | 352 | 31 cm | 2 x 128 4bpp | F1 OBJS01/02, F2 several |
| 5 | Handgun HP | 264 | 20 cm | 4 x 128 8bpp | F2 OBJS15/29 |
| 7 | Revolver | 321 | 17 cm | 2 x 128 8bpp | F2 OBJS29/40 |
| 8 | Burst Handgun | 315 | 25 cm | 3 x 128 4bpp | F1 OBJS28, F2 several |
| 10 | Submachine Gun | 342 | 70 cm | 128 4bpp | F1 OBJS28, F2 several |
| 11 | Shotgun | 250 | 80 cm | 128 4bpp | F1 OBJS01/02/35, F2 several |
| 12 | Shotgun E | 258 | 98 cm | 2 x 128 8bpp | F2 OBJS10/26/40 |
| 13 | Hunting Rifle | 315 | 114 cm | 3 x 128 8bpp | F2 OBJS40 |
| 14 | Assault Rifle | 286 | 84 cm | 2 x 128 4bpp | F1 OBJS41, F2 several |
| 15 | Capsule Gun (BLUE) | 204 | 27 cm | 2 x 128 8bpp | F2 many |
| 20 | Rocket Launcher | 224 | 103 cm | 256 8bpp | F1 OBJS41, F2 several |
| 23-25, 27 | Grenade Launcher (burst/flame/acid/BOW gas) | 250 each (one shape) | 72 cm | 2 x 128 | various |
| 28 / 29 | Pesticide / Flame Spray | 42 / 112 | 22 cm | 256 8bpp | F1 OBJS01/41, F2 several |
| 30 | Stun Gun | 94 | 17 cm | 256 8bpp | F2 many |
| 31 | Nail Gun | 114 | 24 cm | 256 8bpp | F1 OBJS01 |
| 32 | Ampoule Shooter | 210 | 33 cm | 2 x 128 4bpp | F1 OBJS41 |
| 100 / 101 / 102 | Survival / Folding / Butcher Knife | 94 / 38 / 30 | 29 / 20 / 33 cm | 128 4bpp / 256 8bpp | various |
| 103 / 104 / 105 | Iron Pipe / Curved / Bent | 32 / 56 / 64 | 100 / 92 / 51 cm | 256 8bpp | every OBJS |
| 106 / 107 / 111 / 112 | Long Pole / Square Timber / Wooden Pole / Throwable Stick | 12 / 12 / 16 / 16 | 129 / 100 / 100 / 50 cm | 256 8bpp | various |
| 109 / 110 / 113 / 115 | Axe / Scrub Brush (push broom) / Spear / Hammer | 52 / 30 / 70 / 139 | 82 / 140 / 113 / 100 cm | 256 8bpp | various |
| 114 | Molotov Cocktail | 172 | 38 cm | 256 8bpp | many |
| 116 / 119 / 117 / 118 | Crutch / Broken Crutch / Stun Rod / Concrete Piece | 98 / 20 / 230 / 32 | 126 / 50 / 104 / 33 cm | 256 8bpp | various |
| 400 | Kevin's 45 Auto | 299 | 21 cm | 2 x 128 4bpp | every OBJS on both Files |
| 401 | Mark's Handgun | 259 (same shape as 0) | 22 cm | 2 x 128 4bpp | every OBJS |
| 407 / 408 | David's Folding Knife / Monkey Wrench | 38 / 84 | 20 / 18 cm | 256 8bpp | every OBJS |

Not found in any OBJS file: 6 (S&W model 500), 16-18 (Capsule Gun red/green/white), 26 (Grenade L. Extinguish).
They probably share a sibling's mesh: 15 for the capsule guns, 23 for the launchers. Fall back to the sibling.

Community sources agree with this:
- Outbreak-Research's 3D page (Richard Mandel's FAQ) lists OBJSxx as "All of the items you can pick up ... (weapons,
  herbs, sprays, tools, papers, etc.)" and names the weapons per scenario.
- The DeviantArt tutorial uses AFS Explorer + OBTool on `romdata.afs`, but notes that OBTool "cannot extract" items and
  objects, only boned models. In fact the OBJS weapons do carry weights, as described below.
- The Models Resource has Outbreak characters only, no weapons.

## 2. Format details needed to decode a weapon mesh

All little-endian. Sources: libthanatos (`OBNbd.cpp`, `OBModel.cpp`, `OBMesh.cpp`, `OBTStripList.cpp`, `OBNode.cpp`,
`OBSld.cpp`, `OBTextureList.cpp`, `OBMaterial.cpp`, `OBRenderAttribs.h`), OBTool (`convert_to_fbx.cpp`, `tim2.c`,
`tim2utils.c`), and my own reads of the discs.

### NBD container

The header is 16-byte entries `{u32 fourcc (3 ASCII chars), u32 offset, u32 size, u32 texCount}`.
- `OBJSxx.NBD` has a 0x40-byte header with 4 entries: `TEX`, `AMO`, `AHI`, empty.
- Character NBDs have 0x80 bytes with 8 entries: TEX, AMO, AHI, empty, `SDW` shadow, then an optional add-on AMO/AHI
  pair.
- Room NBDs use `TID` (a list of AFS texture ids) instead of `TEX`.

**TEX:** `texCount` x (u32 compressed size, data). Each texture is an LZSS-compressed TIM2 ("SLD"). The scheme works on
16-bit words:
- A 16-bit flag word, read MSB first.
- Flag bit 0: copy one literal u16.
- Flag bit 1: read a u16 code. count = code >> 11; if that is 0, the next u16 is the count. offset = code & 0x7FF, in
  words back; offset 0 means a run of zero words.

This is about 40 lines (libthanatos `OBSld::decompress`).

### AMO (the mesh)

AMO is a chunk tree. Every chunk starts `{u32 type, u32 count, u32 size}`, with size including the 12-byte header.

```
Model (1)
  Date (0x20000)
  MeshList (2) -> count x Mesh (4)
    PrimLists (5)              -> 1-2 strip lists: TStripListFW (0x30000) / TStripListPW (0x40000)
                                  each: count x { u32 n (bit 31 = a direction flag), n x u32 index }
    MaterialRefList (0x50000)  -> int32 per mesh material slot -> index into MaterialList
    TStripMaterialList (0x60000) -> int32 per strip -> index into MaterialRefList
    VertexList (0x70000)       -> float3 position
    NormalList (0x80000)       -> float3 unit normal
    TexCoordList (0xA0000)     -> float2 u, v (0..1 of the texture page; v counts from the top; OBTool writes 1-v for FBX)
    ColorList (0xB0000)        -> float4 RGBA on a 0..255 scale (on the handgun: 0,0,0,255; looks unused with Lambert)
    WeightList (0xC0000)       -> per vertex: u32 n, n x {u32 slot, float weight in percent}
    JointRefList (0x100000)    -> int32 per weight slot
    RenderAttribs (0xF0000)    -> 18 x int32 (cull, alpha blend, z write, filtering, address mode...); absent on weapons
  MaterialList (9) -> Material chunks 0x120000 constant / 0x130000 Lambert / 0x140000 Phong / 0x150000 vertex colour:
                      ambient, diffuse, specular (float4 each), specular decay, texture count, 50 pad, texture ids
  TextureList (0xA) -> 268-byte refs {header, id (index into the NBD's TEX list), width, height, tiling, pad}
```

- Strip indices index all the per-vertex arrays at once, so the mesh is de-indexed. Weapons use FW (single-weight)
  strips only. Characters also use PW (blended) strips.
- Triangles alternate winding along a strip (OBTool swaps the last two indices on odd triangles). None of the weapon
  strips I checked set bit 31.
  - libthanatos reads that bit with an operator-precedence bug, so don't copy that line.
  - A safer way to orient each triangle: flip it when its geometric normal points away from the sum of its vertex
    normals.
- The mesh subchunk count can be larger than the chunk really holds; libthanatos warns about this for File #1 OBJS.
  Stop at the chunk's byte size.
- Materials on weapons: Lambert, diffuse 0.95 grey. Shading is texture x lighting.

### AHI (hierarchy)

- Hierarchy chunk `0xC0000000` (count = nodes + 1).
- A root list (`0x80000000` with the top bit cleared, int32 ids).
- Then nodes: `0x40000001` plain, `0x40000002` mesh, `0x40000003` joint. Each node stores id, parent, child, next
  sibling, then scale[4], rotation[4] (Euler radians, XYZ), translation[4], meshId and groupId, then 46 pad words.
  Joint nodes store the rotation negated and the scale as its reciprocal.
- In OBJS every node is a plain node with meshId -1, and the item id is in groupId.

### Bones and moving parts

Each weapon mesh is rigidly skinned: every vertex has one weight of 100. It is split into 1 to 3 parts by weight slot:
- Pistols (0, 400): slot 0 frame, slot 1 the magazine in the grip, slot 2 the slide.
- Magnum Revolver: frame, hammer, cylinder.
- Shotgun: body, pump.

The JointRefList ids point outside the OBJS hierarchy for most items (e.g. 17-19, 254-256), so they look like ids into a
table in the game. Only the slot order matters to us. Positions are already in bind pose (model space): the whole gun
draws correctly with no joint transforms. Moving the slide part is how our recoil and locked-slide feedback could use
the real slide.

### Units, axes and pose

- Centimetres, Y up: Kevin's model runs 0-173 cm on Y and faces +Z.
- Items are stored in their on-the-floor pose. A pistol lies on its side in the XZ plane: barrel toward -X, top of the
  slide toward +Z, thickness along Y.
- The origin is near the trigger, so probably the hand's attach point. Long guns use the same axes; the shotgun's origin
  is near its middle, about where the trigger is.
- Melee items lie along X, centred on the origin, so their hold point is not given.
- Into HandModel's pointing frame (-Z forward, +Y up, +X right), for guns:

```
model -X -> -Z
model +Z -> +Y
model +Y -> +X
```

  This is a proper rotation, then scale x 0.01 to metres.
- Mirroring is not yet checked (see open questions).

### TIM2 textures

- The standard TIM2 header, plus an Outbreak extra: a u16 global texture id at offset 8.
- Byte 5 is the header format. 0 (File #1 OBJS) puts the picture header at 0x10. 1 (File #2 OBJS) puts it at 0x80.
- Picture header: total/CLUT/image sizes, header size, CLUT colour count, mipmaps, CLUT type, image type, width,
  height, GS TEX0/TEX1.
- Weapon pages:
  - Either 128x128 4bpp (16-colour CLUT) or 256x256 8bpp (256-colour CLUT).
  - CLUT type 0x03 = RGBA32, with CSM1 because bit 7 is clear. For 8bpp, swap CLUT entries 8-15 with 16-23 in every
    group of 32. 4bpp needs no swap.
  - 4bpp stores the low nibble first.
  - Pixels are linear, not GS-swizzled.
- CLUT alpha on the weapon pages is 255, not the PS2 0x80 scale. Handle both: if the maximum alpha is at most 0x80,
  double it.
- Several items share each page.

## 3. The two routes compared

### (a) Read the models from the disc and convert them to our own mesh (recommended)

What it needs:
- **Disc access.** PCSX2's `IsoReader` (`pcsx2/CDVD/IsoReader.h`) finds `NETBIO00.DAT` (`LocateFile`) through the live
  CDVD, so ISO, CHD and CSO all work, and its `ReadFile` loads whole files.
  - NETBIO00.DAT is about 0.9-1.2 GB, so don't use ReadFile on it. Read only the sectors needed: the AFS tables, the
    scenario AFS tables and the OBJS files (about 4 MB for File #1, 10 MB for File #2), with `DoCDVDreadSector`.
  - Do this at boot on the CPU thread, as `cdvdLoadDiscElf` does, or give a worker its own file reader on the image path.
    `IsoReader::ReadSector` is private, so a small public sector-range read is needed.
- **A parser,** about 400 lines of our own C++: AFS, NBD, AMO and AHI (only the chunks above), SLD, TIM2 4/8bpp. Bounds
  check every offset and size so a malformed file can't crash the emulator.
  - Porting from libthanatos/OBTool is allowed (GPL-3.0, keep the attribution). Writing our own from this note is just as
    quick, and avoids their STL-heavy, crash-on-bad-data style.
- **A weapon library:** item id -> mesh (positions, normals, UVs, part slot) + decoded RGBA8 pages. Build it once per
  disc serial (SLPM-65428 / SLPM-65692 only) and hand it to the compositor through an atomic pointer.
  - Keep it in memory only. An optional cache in the user's data folder must never go in the repo; add a .gitignore
    guard if a local tool writes previews.
  - Tests use a small synthetic NBD built in code, never disc data.
- **Hold transforms:**
  - Per kind: the axis mapping above.
  - Per item, fitted from the mesh's bounds: put the grip in the fist and the bore on HandModel's bore line (6.7 cm over
    the grip). Melee weapons get a hand-tuned hold point along the shaft.
  - The muzzle for the laser comes from the mesh instead of `kPistolMuzzleZCm` / `LongGunFor()`.

Effort: about 2 days for loader + renderer + holster, plus about 1.5 days for the new hand, plus a short headset tuning
session (about 4-5 days total, see Steps).

Risk: low to moderate.
- The format is known and checked.
- The open part is how the grip looks in the hand (tuning) and the melee hold points.
- It works offline and online the same way: it reads only the disc and writes nothing to game memory.
- The Frame build (ARMSX2) has the same IsoReader, so the same code ports.

A local Python tool can do the same reading for previews (OBJ export to a folder outside the repo, for tuning in
Blender). It is useful but optional. The emulator path should not depend on it.

### (b) Capture the game's own draw from the GS at runtime

What it would need:
1. The game must draw the weapon at all. In first person the profile's `hold` points the camera focus at an empty slot,
   so the room's near-camera cull hides the local character. The held weapon is part of that character and is almost
   certainly culled with it. Capture would mean un-hiding the body and then dropping every body draw except the weapon.
2. Picking out the weapon's draws. Its texture page is shared with the other items in the room, including ones lying on
   the floor. A match by TBP/CBP plus primitive count is fragile.
3. Turning the draws back into 3D. The GS gets projected screen-space vertices, and our stereo path already displaces
   them per eye. A mesh would mean un-projecting with the game's camera matrix (the profile reads the view matrix,
   so this is possible, but noisy in Z).
4. Re-posing it. The captured weapon sits where the game's aim animation put the character's hand, not where Alex's
   controller is. Moving it to the controller needs the game's hand-bone transform, which we don't have.

Effort: one to two weeks. Risk: high, with no clean result at the end.

The only useful bit of (b) is a one-off check: take a GS dump in third person with the weapon raised and confirm the
held weapon's triangle count and texture page match the OBJS mesh.

Reading the loaded OBJS data from EE RAM is a middle way, but it gives only the current scenario's items and needs the
buffer's address found per scenario. It has no advantage over the disc.

## 4. What HandModel needs

How it works today (`pcsx2/VR/HandModel.cpp`):
- `BuildWorld` fills `s_mesh` (a `std::vector<Tri>`, reserve 768) with flat-shaded triangles from box and segment
  builders: hand, pistol, long gun, melee, thrown, spray.
- `Tri` = 3 positions, a face normal and a material id.
- `RenderImpostor` projects each triangle from one eye onto the quad plane. `RasterTri` fills a constant colour per
  triangle with an edge-function loop and a 1/z depth test. `SoftenEdges` antialiases the outline.
- The compositor (`XRCompositor.cpp`, `kHandSlots = 2`) uploads each 256x256 image to that hand's swapchain and redraws
  it only on view drift, articulation, a shot or age (`NeedsReraster`).

To draw a textured game mesh:
1. **`Tri`**: add `u[3], v[3]` and a texture index (none = today's material colour).
2. **Textures**: decoded RGBA8 pages from the library. Convert texels to linear light with the existing sRGB tables
   before lighting.
3. **`RasterTri`**: a textured variant. The loop already has the barycentric edge values (`ea, eb, ec`) and per-vertex
   1/z. Interpolate u/z and v/z the same way and divide by the interpolated 1/z for perspective-correct UVs.
   Nearest-texel sampling is enough at this size (a gun covers roughly 100-200 px of the 256 image). Bilinear is cheap
   if wanted.
4. **`Shade`**: split it into a light factor (ambient + Lambert + headlight + rim + spec, per face as now) and a pack
   step. A textured pixel = texel x factor + rim/spec. Flat shading is fine; Gouraud from the mesh normals is an option.
5. **`BuildWorld`**: when the library has the item, append its triangles, mapped through the hold transform, in place
   of `BuildGunTop`/`BuildLongGun`/`BuildMelee`. They go in the pointing frame, untilted, like `BuildGunTop`. Then the
   existing recoil, mirror and world transform apply.
   - The pistol slide moves by translating the slide-part vertices along the barrel.
   - The procedural shapes stay as the fallback: no disc, another serial, a parse failure, or an id with no mesh.
6. **Muzzle and fore-end**: `LaserFrom` and `OffHandSupports` should take the muzzle and fore-end from the item's mesh.
7. **Cull and winding**: keep the culling by face normal. Take each triangle's normal from its geometry, oriented by
   its vertex normals.
8. **SelfTest**: add a synthetic textured triangle and a synthetic NBD parse.

Budget: a gun is 250-350 triangles, the current procedural hand and gun fit in 768, and the CC0 low hand is 938.
About 1,300 triangles per image is nothing for this rasteriser. The fill of at most 65k pixels dominates, and it runs
only on redraw.

**The holster at the right hip** is a third impostor slot (kHandSlots 2 -> 3, its own swapchain and quad layer).
- Pose it from the body: the hip zone `SpatialControls`/`BeltPouches` already place (the holster ball, zone 0), turned
  with the body yaw.
- It shows the equipped handgun mesh in a muzzle-down holster pose, plus a simple procedural holster sleeve, while the
  weapon is lowered. It is empty while the weapon is raised. With a long gun or melee weapon equipped it shows nothing,
  unless Alex wants otherwise.
- It needs redrawing only when the head moves relative to the hip. Check the layer budget (the Frame branch has one).

## 5. Hand model pick

**Godot XR Tools hand models** (Godot VR community; `addons/godot-xr-tools/hands/`):
- **Licence: CC0 1.0** (`hands/License.md`: "DigitalN8m4r3 aka Miodrag Sejic has waived all copyright ... to Hand Models
  for the Godot XR Tools", 7 November 2022). `About.md` says the meshes came from MakeHuman's CC0 assets. The repo
  itself is MIT. Committing a converted copy is allowed; a credit line is polite, not required.
- `Hand_low_R.gltf` / `Hand_low_L.gltf`: **938 triangles, 603 vertices**, one mesh with position, normal, UV and
  4-joint skin weights.
  - Gloved variants: `Hand_Glove_low_*.gltf`, 1160 triangles. The full versions are 3052 (bare) / 3490 (gloved).
  - Textures are in `hands/textures/` (PNG albedo, normal and ORM; skin tones, camo and lab gloves).
- **26 joints named like OpenXR's `XR_EXT_hand_tracking`**: Palm, Wrist, Thumb Metacarpal/Proximal/Distal/Tip, and
  Index/Middle/Ring/Little Metacarpal/Proximal/Intermediate/Distal/Tip. Controller-driven curls work, and real hand
  tracking could drive it later with no retargeting.
- Poses: Default, Grip, Grip 1-5, Grip Shaft, Hold, Pistol, Pinch, Point and more. They ship as Godot `.res` animations
  and in `blend/animations_hand_*.blend`.
  - Export the few we need (open, Grip, Pistol, Grip Shaft) from Blender as joint rotations, or keep our own
    per-finger curl angles.
- Integration:
  - A small Python converter: glTF JSON with embedded buffers -> a compact C++ array (positions, normals, UVs, joint
    weights, bind matrices). The result is committed, since it's CC0.
  - CPU skinning of 600 vertices into `s_mesh` each redraw, with grip and trigger blending between poses.

Alternatives checked:
- WebXR Input Profiles `generic-hand` (three.js uses it). W3C Software and Document License: permissive, but it needs
  the notice.
- Valve's SteamVR Unity Plugin gloves. BSD-3-Clause, SteamVR skeleton.
- BlendSwap CC0 hands (SparrowHawk "Hands Rigged", erik90mx "Low Poly Hand"). CC0 per the listings, but .blend only and
  not checked in detail.
- Most Sketchfab VR hands are CC-BY.

## Recommended route, steps

1. **Loader** (`pcsx2/VR/GameModels.{h,cpp}`, about 1 day).
   - At VM boot for SLPM-65428/65692, find `NETBIO00.DAT` and read the AFS tables.
   - Read every `rXXX.afs/OBJSxx.NBD` (or just the scenarios on that disc).
   - Parse and decode into an item-id-keyed library: union across scenarios, the first copy wins, sibling fallbacks for
     6, 16-18 and 26.
   - Log one line: how many weapons loaded, from which files.
   - Nothing is written to disk.
2. **Textured raster** in HandModel, with hold transforms, the slide part, and the muzzle and fore-end from the mesh.
   Procedural fallback kept (about 1 day).
3. **Holster** slot at the right hip (about 0.5 day).
4. **CC0 hand**: converter, committed data, skinning, poses. This replaces the procedural glove; the left hand gets the
   mirror model (about 1.5 days).
5. **Headset tuning** with Alex: the grip offset per weapon class and the melee hold points (a short session).
6. Port to the ARMSX2 `outbreak-vr-frame` branch (same IsoReader; the rasteriser is portable C++).

Total: about 4-5 days of work plus one tuning session.

## Open questions

- **Is the third-person held weapon the OBJS mesh?** Very likely (same low-poly set, origin at the trigger, parts
  skinned for slide/hammer animation), but unproven. One GS dump in third person would settle it.
- **Mirroring.** Is the game's coordinate system the same handedness as ours? Check the ejection-port side or text on an
  item texture. If it's mirrored, flip the thickness axis at load. For a weapon this is nearly invisible either way.
- **The game's own grip transform** (hand bone to weapon) is not in the OBJS files. It is probably in code or the motion
  data, and unknown. We fit and tune our own, which is fine for VR.
- **Melee hold points**: the meshes are centred, so where the hand goes on a pipe or pole is our choice.
- **What the `wpNN.wmd` files drive** (probably the slide, hammer and cylinder animation) and what the out-of-range
  JointRefList ids mean. Not needed for a static model plus our own slide motion.
- **Missing ids** 6, 16-18 and 26: confirm the sibling meshes look right.
- **Disc reads off the CPU thread:** whether `DoCDVDreadSector` is safe outside boot. Use it at boot, or open a separate
  reader.
- **Holster behaviour** with a long gun or melee weapon equipped, and whether to show the holster sleeve at all. Ask
  Alex.
- **Hand poses**: converting Godot's `.res` animations needs Blender (the `.blend` files) or re-authored curls.

## Sources

- Fothsid, libthanatos (GPL-3.0): https://github.com/Fothsid/libthanatos
  - https://github.com/Fothsid/libthanatos/blob/main/src/OBNbd.cpp
  - https://github.com/Fothsid/libthanatos/blob/main/src/OBMesh.cpp
  - https://github.com/Fothsid/libthanatos/blob/main/src/OBTStripList.cpp
  - https://github.com/Fothsid/libthanatos/blob/main/src/OBNode.cpp
  - https://github.com/Fothsid/libthanatos/blob/main/src/OBSld.cpp
  - https://github.com/Fothsid/libthanatos/blob/main/include/thanatos/OBTypes.h
  - https://github.com/Fothsid/libthanatos/blob/main/include/thanatos/OBRenderAttribs.h
- Fothsid, OBTool (GPL-3.0): https://github.com/Fothsid/OBTool
  - https://github.com/Fothsid/OBTool/blob/main/src/convert_to_fbx.cpp
  - https://github.com/Fothsid/OBTool/blob/main/src/tim2.c
  - https://github.com/Fothsid/OBTool/blob/main/src/tim2.h
  - https://github.com/Fothsid/OBTool/blob/main/src/tim2utils.c
  - https://github.com/Fothsid/OBTool/blob/main/src/extract_scenario_afs.cpp
- Outbreak-Research/Documentation (MIT): https://github.com/Outbreak-Research/Documentation
  - 3D page (model resolutions, OBJSxx item clusters, EFxx, scenario numbers):
    https://github.com/Outbreak-Research/Documentation/blob/main/docs/3D/index.md
  - https://github.com/Outbreak-Research/Documentation/blob/main/docs/3D/models.md
  - https://github.com/Outbreak-Research/Documentation/blob/main/docs/textures/index.md
  - Animations page (`pl_pc`, scenario `sp` files): https://github.com/Outbreak-Research/Documentation/blob/main/docs/game%20data/animations.md
  - Item ids, already used by `VR/ItemData`: https://github.com/Outbreak-Research/Documentation/blob/main/docs/game%20data/items.md
- Tyrant0400Tp, "Method of how to extract models from REOB 1 and 2" (AFS Explorer + OBTool):
  https://www.deviantart.com/tyrant0400tp/journal/Method-of-how-to-extract-models-from-REOB-1-and-2-901286504
- RE 1 2 3 Modding Forum, "Resident Evil Outbreak NBDx Tool and Other Things". Found by search; the page refused fetching
  (HTTP 403): https://www.tapatalk.com/groups/residentevil123/resident-evil-outbreak-nbdx-tool-and-other-things-t1192-s60.html
- The Cutting Room Floor, nested AFS inside NETBIO00.DAT: https://tcrf.net/Resident_Evil:_Outbreak
- The Models Resource, Outbreak File 1 (characters only): https://models.spriters-resource.com/playstation_2/residenteviloutbreakfile1/
- sp00nznet/reo, a static recompilation with its own NBD parser. No licence, reference only, not reusable:
  https://github.com/sp00nznet/reo
- Godot XR Tools hands (CC0 1.0; repo MIT): https://github.com/GodotVR/godot-xr-tools/tree/master/addons/godot-xr-tools/hands
  - https://github.com/GodotVR/godot-xr-tools/blob/master/addons/godot-xr-tools/hands/License.md
  - https://github.com/GodotVR/godot-xr-tools/blob/master/addons/godot-xr-tools/hands/About.md
  - MakeHuman licence, section D (outputs): https://github.com/makehumancommunity/makehuman/blob/master/LICENSE.md
- WebXR Input Profiles generic hand (W3C Software and Document License):
  https://github.com/immersive-web/webxr-input-profiles/tree/main/packages/assets/profiles/generic-hand ;
  https://www.w3.org/Consortium/Legal/copyright-software
- SteamVR Unity Plugin (BSD-3-Clause): https://github.com/ValveSoftware/steamvr_unity_plugin
- BlendSwap CC0 hands: https://blendswap.com/blend/22269 ; https://blendswap.com/blend/2429
- OpenXR `XR_EXT_hand_tracking` joint set: https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_EXT_hand_tracking
- In this repo: `pcsx2/VR/HandModel.{h,cpp}`, `pcsx2/VR/XRCompositor.cpp` (hand slots, redraw), `pcsx2/VR/ItemData.cpp`
  (item ids), `pcsx2/CDVD/IsoReader.{h,cpp}` (disc reads), `bin/resources/vr-profiles/SLPM-65428.yaml` (`hold`: the
  near-camera cull that hides the local body).
