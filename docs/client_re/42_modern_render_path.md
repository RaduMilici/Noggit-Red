# 42. The modern render era: what Legion+ assets change for the renderer

Date: 2026-09-16. Follows [41 (loading modern CASC clients)](41_modern_casc_client_support_research.md).
Scope: Classic Era 1.15.9.69722 and Anniversary 2.5.6.69795 as shipped in `F:\World of Warcraft`
(wow_classic_era / wow_anniversary). Everything below is measured on those files or taken from the
client-derived references named in each section; nothing is tuned by eye.

noggit already keeps TWO render engines apart -- the 1.12 one (`Model::_uses_classic_layout`, v256/257
files; `ProjectVersion::CLASSIC` for WMO / sky rules) and the 3.3.5a one (everything else). This note adds
the THIRD, the Legion+ engine that every modern CASC client runs, and records what actually differs for the
assets those clients ship.

## 0. Where the era is decided

| Level | Accessor | Rule |
|---|---|---|
| M2 file | `Model::renderEra()` -> `Vanilla / WotLK / Modern` | classic layout (v256/257) / v264 / MD21 container (v27x). Per FILE: a modern project can still open a WotLK-era M2 by path. |
| WMO / terrain / sky | `ProjectVersion` (`isModernCascVersion`) | CLASSIC_ERA, ANNIVERSARY (and CATA..SL) |

Consumers: `ModelRender::{fixShaderIdBlendOverride, fixShaderIDLayer, computePixelShaderIDs, draw,
buildStaticBatchKey}`, `ModelRenderPass::initUVTypes`, `ParticleSystem` (head/tail, EXP2), `m2_frag.glsl`
(`alpha_key_modern`), `StaticBatchKey::modern_alpha` (batched / MDI paths).

## 1. Survey: what the Classic Era assets around Northshire actually use

172 M2 + 21 WMO roots + 384 group files + 3 tex0 files referenced by azeroth 31..33 / 47..50 and 32_50
(`scratchpad/survey_modern_assets.py`, fdids from the obj0 MDDF / MODF of those tiles).

**M2** -- version 274 (171) / 272 (1). Header global flags: 0x80 (172), 0x2000 chunked .anim (171),
0x200000 "24500 upgraded model format" (171), 0x200 (2), 0x8000 (2), 0x10 (2); NO 0x8 (texture combiner
combos). Chunks: MD21, SFID, TXID, TXAC (171), LDV1 (171), EXP2 (9 = the particle models). Materials: flags
0x0 (206) / 0x4 two-sided (101) / 0x10 depth-write (5) / 0x14 (9) / 0x1 unlit (2) / 0x11, 0x13, 0x15, 0x3,
0x5 (1 each); blend modes 0 (221), 1 AlphaKey (91), 2 Alpha (11), 4 Add (6). Textures: type 0 only (336),
flags 0x3 / 0x0 / 0x1 / 0x2. Skin batches: shader_id 0x0 (212), **0x10 (107)**, **0x8001 (4)**; batch
flags 0x10 (314), 0x90 (4), 0x0 (5); flags2 0x0 except one 0x1. Particles: flags 0x1, 0x2, 0x8, 0x20,
0x400, 0x1000, **0x20000** (16); blend 4 (12) / 2 (4); emitter type 1 (15) / 2 (1). No compressed gravity
(0x800000), no multi-texture (0x10000000).

**WMO** -- root chunks MVER MOHD MOMT MOGN MOGI MOSB MOPV MOPT MOPR MOVV MOVB MOLT MODS MODD MFOG MODI (12
of 21) GFID (21). MOHD flags: none. MOMT: shader 0 Diffuse (424) / 1 Specular (11); flags 0x1, 0x4, 0x8,
0x10, 0x20, 0x40, 0x80; blend 0 (422) / 1 (13). MOGP flags: 0x1, 0x4, 0x8, 0x40, 0x80, 0x200, 0x800,
0x1000, 0x2000, 0x8000, 0x10000. Group sub-chunks: MOPY MOVI MOVT MONR MOTV MOBA MOLR MODR MOBN MOBR MOCV
MLIQ -- **plain MOPY, no MPY2 / MOGX / MOBS / MFVR / MDAL / MOCV2 / second MOTV** in this set (the parser
handles them anyway, 41 sec 10a).

**Terrain (tex0)** -- chunks MVER MAMP (0) MDID MHID MCNK; MHID all zero (no height textures), no MTXP;
MCLY flags only 0x100 (use_alpha_map), no 0x200 compressed alpha, no 0x800 / 0x1000 texture scale; 1-4
layers per chunk. I.e. WotLK-style alpha blending, no height-based blending.

**Conclusion:** the converted vanilla / TBC content is the WotLK feature set inside the Legion container.
The render-relevant differences are in the M2 skin shader ids, the particle record, the alpha-key
reference, and the DB-driven pieces (41). WMO and terrain need nothing beyond correct parsing today; the
Legion-era WMO / terrain features are documented in sec 5-6 for the day retail-era content is opened.

## 2. What changed in the M2 skin / material path (implemented)

Reference: wowdev M2/.skin ("Cataclysm and later: shader_id is stored; if 0x8000 is set the low bits
index s_modelShaderEffect, otherwise M2GetPixelShaderID / M2GetVertexShaderID decode the bits") and the
client-derived WoWViewerCpp `m2Object.cpp` (M2ShaderTable, getPixelShaderId, getVertexShaderId).

**Before Cata** the client computed shader_id at load from the material blend, the texture-unit lookup
and the transparency animation (WotLK `sub_836980 / sub_837680`; noggit's `fixShaderIdBlendOverride`).
Modern files ship an EMPTY texture-unit lookup, so that code path had nothing to derive from and its
"fuckporting" fallback forced every MD21 batch to `shader_id = 0, texture_count = 1` =
Combiners_Opaque: the texture alpha never reached the alpha test, and every alpha-keyed canopy drew its
black background. That was the "trees with black leaf backgrounds".

**Now, era Modern:**

- `shader_id` is used as authored. 1 texture: `(id & 0x70) ? Combiners_Mod : Combiners_Opaque`.
  2 textures: bits 4-6 set -> Mod family by `id & 7` {0 Mod_Opaque, 1/2/5 Mod_Mod, 3 Mod_Add,
  4 Mod_Mod2x, 6 Mod_Mod2xNA, 7 Mod_AddNA}; clear -> Opaque family {0 Opaque_Opaque, 1/2/5 Opaque_Mod,
  3/7 Opaque_AddAlpha, 4 Opaque_Mod2x, 6 Opaque_Mod2xNA}.
- `0x8000 | n` indexes the 36-row **0-based** s_modelShaderEffect (table in `ModelRender.cpp`,
  `s_modern_shader_effect`). noggit's WotLK table is 1-based (its `case N` = modern index N-1), so the
  same bytes mean different shaders per era: 0x8001 is Opaque_Mod2xNA_Alpha in a WotLK file and
  Opaque_AddAlpha over Diffuse_T1_Env in an MD21 file (ballista / weapon rack: weapon texture +
  `armorreflect3.blp` sphere-mapped and added by alpha = the weapon shine).
- uv sources come from the vertex shader: 1 texture `(id & 0x80) ? Env : ((id & 0x4000) ? T2 : T1)`;
  2 textures `!(id & 0x80)`: `(id & 8) ? T1_Env : ((id & 0x4000) ? T1_T2 : T1_T1)`, else
  `(id & 8) ? Env_Env : Env_T1`; 0x8000 rows carry their own vertex column.
- rows whose pixel shader noggit has no combiner for map to the nearest one (3-texture variants drop the
  third texture; EdgeFade / Depth / Crossfade / Guild / Illum drop the effect) -- each row is marked in
  the table. Seven combiners were added to `m2_frag.glsl` (ids 23-29: Mod_AddAlpha, Mod_AddAlpha_Alpha,
  Opaque_Alpha_Alpha, Opaque_ModNA_Alpha, Mod_Add_Alpha, Opaque_Alpha, Opaque_Mod_Add_Wgt) with the
  expressions of WoWViewerCpp `commonM2Material.slang`; `*_Wgt` texture weights are taken as 1 (noggit's
  passes carry no weight stream).
- `fixShaderIDLayer` (WotLK layer merge) is skipped like for classic files; layers are authored.
- **Alpha-key reference**: 1.12 = 128/255 with src-alpha blending, 3.3.5a = 224/255 blending off (doc
  40 sec 10); Legion+ = **128/255**, blending off (WoWViewerCpp: `blendMode == 1 && discardAlpha <
  0.501960814 -> discard`). `alpha_key_modern` uniform, `StaticBatchKey::modern_alpha` so batched groups
  never mix eras.
- Material flags / blend modes are unchanged from WotLK for this content (0x1 unlit, 0x2 unfogged, 0x4
  two-sided, 0x8 depth test, 0x10 depth write; 0x800 "prevent alpha for custom elements" is WoD+
  character gear). Texture types 0..26 exist; only 0 (file) occurs here; TXID supplies the id (41).
- Header flag 0x8 (texture combiner combos) keeps the WotLK blend-override path; no file here sets it.

## 3. Modern shader tables (for reference)

s_modelShaderEffect (WoWViewerCpp M2ShaderTable, 0-based; pixel / vertex):
0 Opaque_Mod2xNA_Alpha / T1_Env; 1 Opaque_AddAlpha / T1_Env; 2 Opaque_AddAlpha_Alpha / T1_Env;
3 Opaque_Mod2xNA_Alpha_Add / T1_Env_T1; 4 Mod_AddAlpha / T1_Env; 5 Opaque_AddAlpha / T1_T1;
6 Mod_AddAlpha / T1_T1; 7 Mod_AddAlpha_Alpha / T1_Env; 8 Opaque_Alpha_Alpha / T1_Env;
9 Opaque_Mod2xNA_Alpha_3s / T1_Env_T1; 10 Opaque_AddAlpha_Wgt / T1_T1; 11 Mod_Add_Alpha / T1_Env;
12 Opaque_ModNA_Alpha / T1_Env; 13 Mod_AddAlpha_Wgt / T1_Env; 14 Mod_AddAlpha_Wgt / T1_T1;
15 Opaque_AddAlpha_Wgt / T1_T2; 16 Opaque_Mod_Add_Wgt / T1_Env; 17 Opaque_Mod2xNA_Alpha_UnshAlpha /
T1_Env_T1; 18 Mod_Dual_Crossfade / T1; 19 Mod_Depth / EdgeFade_T1; 20 Opaque_Mod2xNA_Alpha_Alpha /
T1_Env_T2; 21 Mod_Mod / EdgeFade_T1_T2; 22 Mod_Masked_Dual_Crossfade / T1_T2; 23 Opaque_Alpha / T1_T1;
24 Opaque_Mod2xNA_Alpha_UnshAlpha / T1_Env_T2; 25 Mod_Depth / EdgeFade_Env; 26 Guild / T1_T2_T1;
27 Guild_NoBorder / T1_T2; 28 Guild_Opaque / T1_T2_T1; 29 Illum / T1_T1; 30 Mod_Mod_Mod_Const /
T1_T2_T3; 31 Mod_Mod_Mod_Const / Color_T1_T2_T3; 32 Opaque / T1; 33 Mod_Mod2x / EdgeFade_T1_T2;
34 Mod / EdgeFade_T1; 35 Mod_Mod_Depth / EdgeFade_T1_T2.

Pixel shader semantics (WoWViewerCpp `commonM2Material.slang`, `tex` = texture 1, `tex2` = texture 2;
"discardAlpha" is what the alpha test sees, "specular" is added to the lit colour):
Opaque: mesh*tex.rgb, alpha = mesh.a. Mod: mesh*tex.rgb, alpha = tex.a. Opaque_Mod: mesh*tex*tex2, alpha
= tex2.a. Opaque_Mod2x: mesh*tex*tex2*2, alpha = tex2.a*2. Opaque_Mod2xNA: mesh*tex*tex2*2. Opaque_Opaque:
mesh*tex*tex2. Mod_Mod: mesh*tex*tex2, alpha = tex.a*tex2.a. Mod_Mod2x: x2, alpha = tex.a*tex2.a*2.
Mod_Add: mesh*tex, alpha = tex.a + tex2.a, specular = tex2. Mod_Mod2xNA: mesh*tex*tex2*2, alpha = tex.a.
Mod_AddNA: mesh*tex, alpha = tex.a, specular = tex2. Mod_Opaque: mesh*tex*tex2, alpha = tex.a.
Opaque_Mod2xNA_Alpha: mesh*mix(tex*tex2*2, tex, tex.a). Opaque_AddAlpha: mesh*tex, specular = tex2*tex2.a.
Opaque_AddAlpha_Alpha: mesh*tex, specular = tex2*tex2.a*(1-tex.a). Opaque_Mod2xNA_Alpha_Add: + tex3*tex3.a*w.b.
Mod_AddAlpha: mesh*tex, alpha = tex.a, specular = tex2*tex2.a. Mod_AddAlpha_Alpha: mesh*tex, alpha =
tex.a + tex2.a*lum(tex2), specular = tex2*tex2.a*(1-tex.a). Opaque_Alpha_Alpha: mesh*mix(mix(tex, tex2,
tex2.a), tex, tex.a). Opaque_Mod2xNA_Alpha_3s: mesh*mix(tex*tex2*2, tex3, tex3.a). Opaque_AddAlpha_Wgt:
mesh*tex, specular = tex2*tex2.a*w.g. Mod_Add_Alpha: mesh*tex, alpha = tex.a + tex2.a, specular =
tex2*(1-tex.a). Opaque_ModNA_Alpha: mesh*mix(tex*tex2, tex, tex.a). Mod_AddAlpha_Wgt: mesh*tex, alpha =
tex.a, specular = tex2*tex2.a*w.g. Opaque_Mod_Add_Wgt: mesh*mix(tex, tex2, tex2.a), specular = tex*tex.a*w.r.
Opaque_Mod2xNA_Alpha_UnshAlpha: mesh*mix(...)*(1-glow), specular = tex3*glow. Mod_Dual_Crossfade /
Mod_Masked_Dual_Crossfade: crossfade of tex / tex2 (/ tex3) by animated weights. Opaque_Alpha: mesh*mix(tex,
tex2, tex2.a). Mod_Depth / Mod_Mod_Depth: as Mod / Mod_Mod. Guild*: guild colours from genericParams. Illum:
alpha = tex.a. Mod_Mod_Mod_Const: mesh*(tex*tex2*tex3*const), alpha likewise.

Alpha test: `blendMode == 1 (AlphaKey): discard if discardAlpha < 0.501960814`; other blends:
`finalOpacity = discardAlpha * meshOpacity`.

## 4. Particles (implemented: head/tail, EXP2 multipliers; documented: the rest)

- **Record**: 492 bytes = the 476-byte v264 record + `multiTextureParam0[2]` + `multiTextureParam1[2]`
  (16 bytes at the END). 41 sec 10a: the stride is detected from the track headers, not from global flag
  0x200 (Classic Era sets the flag CLEAR on 492-byte records).
- **Head / Tail**: v264 keeps `particleType` (0x2c) / `headOrTail` (0x2d). Cata+ replaced those two bytes
  with `multiTexScale[2]` (fixed_point<int8, 2, 5>) and moved the style into the flags: **0x20000
  HeadStyle, 0x40000 TailStyle** (wowdev M2#Particle_flags, "Cata+"). `ParticleSystem` now reads the style
  from the flags for Modern files (16 Northshire emitters carry 0x20000).
- **EXP2** (`M2ExtendedParticle {float zSource; float colorMult; float alphaMult; M2PartTrack<fixed16>
  alphaCutoff}`, offsets relative to the chunk data): parsed into `Model::_extended_particles`; colorMult /
  alphaMult applied to the particle colour in `ParticleSystem::update`. All 9 local files carry
  `{0, 1, 1, no keys}` so nothing changes on screen today. `alphaCutoff` (a per-life alpha-test threshold)
  is parsed but not yet applied: noggit's particle alpha test is one uniform per draw, the client's is per
  particle; no local file has keys. **EXPT** (the pre-EXP2 form: zSource, colorMult, alphaMult only) is not
  parsed; no local file has it.
- **texture field** (Cata+ with flag 0x10000000 MultiTexture): `texture_0 : 5, texture_1 : 5, texture_2 : 5`
  packed; `multiTextureParam*` are the per-layer uv scroll / scale; 0x20000000 uses Modx4 and 0x40000000
  three colours. Not implemented; no local file uses it.
- **Compressed gravity** (flag 0x800000): the gravity track values are `{int8 x, y; int16 z}` vectors
  instead of floats. Not implemented; no local file uses it. Files that do would read garbage gravity --
  guard before opening retail-era effects.
- **Other Cata+ flags** used here: 0x1 Shaded, 0x2 SortParticles, 0x8 Unfogged, 0x20 InheritBoneScale,
  0x400 ClampTailToAge, 0x1000 XYQuad -- all already honoured by the WotLK path.
- **PGD1** (geoset per emitter), **RPID / GPID** (recursive / geometry particle model fdids), **DETL** (light
  detail): not needed for the Northshire set; RPID / GPID would need the fdid-based model key (41).
- **ParticleColor.db2** (`particleColorIndex`): no adapter yet (creature recolours only).

## 5. WMO: nothing new needed for this content; what Legion+ files can add

MOMT shader table (wowdev WMO): 0 Diffuse, 1 Specular, 2 Metal, 3 Env, 4 Opaque, 5 EnvMetal,
6 TwoLayerDiffuse, 7 TwoLayerEnvMetal, 8 TwoLayerTerrain (second texture gets "_s"), 9 DiffuseEmissive,
10 waterWindow (auto MOTA), 11 MaskedEnvMetal, 12 EnvMetalEmissive, 13 TwoLayerDiffuseOpaque,
14 submarineWindow, 15 TwoLayerDiffuseEmissive, 16 DiffuseTerrain, 17 AdditiveMaskedEnvMetal,
18 TwoLayerDiffuseMod2x, 19 TwoLayerDiffuseMod2xNA, 20 TwoLayerDiffuseAlpha, 21 Lod, 22 Parallax,
23 UnkDFShader. noggit implements 0-6 (`wmo_frag.glsl`: Env = 3, EnvMetal = 5, TwoLayerDiffuse = 6 use
the second texture; MOMT texture slots 1/2/3 are fdids in modern roots, 41). WoWViewerCpp
`commonWMOMaterial.slang` semantics: Diffuse `rgb = tex, a = tex.a`; Specular `+ calcSpec(tex.a)`; Metal
`+ calcSpec(((tex*4)*tex.a).x)`; Env `emissive = tex2*tex.a*distFade, opacity 1`; Opaque `opacity 1`;
EnvMetal `emissive = tex*tex.a*tex2*distFade`; TwoLayerDiffuse `mix by vColor2.a`; TwoLayerEnvMetal
`mix by 1-vColor2.a + tex3 emissive`; TwoLayerTerrain `mix by vColor2.a, spec from tex2.a`;
MaskedEnvMetal `mix by tex3.a*vColor2.a`; Parallax height offsets; UnkDFShader 4-layer weights from
vColorSecond.
MOMT flags: 0x1 unlit, 0x2 unfogged, 0x4 two-sided, 0x8 exterior light, 0x10 SIDN (window glow at night),
0x20 window, 0x40 / 0x80 clamp S / T, 0x100 unknown -- all already in the WMO batch flags.
Group additions since WotLK: MPY2 (u16 flags, u16 material -- DF 10.0), MOCV2 (second colour set, alpha =
texture blend; FixColorVertexAlpha must NOT touch it; MOGP 0x1000000), second / third MOTV (0x2000000 /
0x40000000), MOLV / MOLP (SL lights / cut planes), MFVR (per-vertex fog volume ref), MDAL (detail doodad
layers), MOGX (query face start), MOBS, MAVG / MAVD (ambient volumes, override MOHD ambient), MOUV (uv
scroll per material, 7.3), MOSI / MODI / GFID (fdids, 41). MOGP flags2: 0x1 canCutTerrain, 0x40 / 0x80
split groups, 0x100 attachment mesh. FixColorVertexAlpha (load-time MOCV rewrite by MOHD ambient and
batch class) is unchanged since WotLK and stays as noggit has it.

## 6. Terrain: nothing new needed for this content; what Legion+ files can add

MCLY flags: 0x1-0x4 animation rotation, 0x8-0x20 speed, 0x40 enabled, 0x80 overbright, 0x100 use alpha
map, 0x200 compressed alpha, 0x400 cube-map reflection, 0x800 / 0x1000 texture scale (WoD+). MTXF: 0x1 do
not load `_s` / `_h`, 0x70 texture scale (MoP+). **MTXP** (`{flags, heightScale, heightOffset, pad}` per
texture) + **MHID** (`_h.blp` height textures) drive WoD+ height-based layer blending -- absent / zero in
the Classic Era tiles, so alpha blending is exact today. MCLV (per-vertex light, Cata+), MCBB (blend
batches: terrain-WMO blend meshes MBMH / MBBB / MBNV / MBMI, MoP+), MCDD (detail doodad disable bitmask),
MCMT (material ids per layer), MCRD / MCRW (split refs, 41), MAMP (alpha resolution), MTCG (colour
grading, SL). MHDR flags 0x1 MFBO, 0x2 "northrend". `_s.blp` specular companions are looked up by name
in the client; noggit does not load them.

## 7. Textures

BLP is unchanged (BLP2, DXT1/3/5 / palettised / uncompressed). What changed is addressing: M2 by TXID, WMO
by MOMT fdids, terrain by MDID / MHID, skybox by LightSkybox.SkyboxFileDataID, ground effects by
GroundEffectDoodad.ModelFileID -- all in 41. Texture types 0..26 exist; converted vanilla content uses 0.

## 8. Not yet done / open

- EXP2 alphaCutoff per particle; EXPT; multi-texture particles; compressed gravity (no local users).
- 3-texture M2 batches (Diffuse_T1_Env_T1 etc.): noggit passes carry two textures; rows 3, 9, 17, 20, 24,
  26, 28, 30, 31 of the table drop the third.
- WMO shaders 7-23 and MOCV2 / MOUV / MAVG / MAVD / MOLV effects; terrain MTXP / MHID height blending,
  MCLV, MCBB blend meshes, MCDD -- none used by the Classic Era / Anniversary world tiles inspected.
- The Vulkan m2 shader does not know pixel shaders 23-29 (falls to its default branch); VK is not a
  target at the moment (user, 2026-09-16).

## 10. Second pass (2026-09-16 later): creatures, WMO-only maps, particles, animations

User report after build 19: no creatures or game objects, WMO-only maps (Molten Core, Mauradon) empty,
particles wrong (the portal too big and shooting outward instead of spinning). Each was traced to a
concrete data or index change; "see how the client might interpret a different index meaning".

### 10.1 Particles: the zSource slot is a placeholder in modern files

Field-by-field comparison of the SAME emitters in the 3.3.5a file and the 1.15.9 file
(`scratchpad` compare script; stormwindmageportal01 18 emitters, generaltorch01, elwynncampfire,
fountainparticles, housesmoke, stonepyre01, elwynntallwaterfall01):

| Field | 3.3.5a | 1.15.9 | Meaning |
|---|---|---|---|
| `zSource` M2Track (record offset 240, noggit's `Gravity2` / `deacceleration`) | 0.0 | **255.0 on every emitter of every file** | the record slot is dead; the live zSource is `EXP2.zSource` (0.0 in all of them). WoWViewerCpp assigns `exp2Rec->zSource` over the record. noggit fed 255 into `p.speed -= dir * deaccel * dt` -> particles shot away. Fixed: Modern files take zSource from EXP2 (or 0). |
| `flags` | 0x20020 | 0x20021 | +0x1 "Shaded" (lighting) on every emitter -- no motion change |
| `enabledIn` track | 1 key | 0 keys | modern emitters ship no enable keys; noggit's `enabled.uses(anim)` path treats that as always on |
| `headCell` / `tailCell` | 4 keys | 0 keys | flipbook cell tracks dropped where the sheet is 1x1 |
| portal emitter 17 `spin` | -0.022 | -1.25 | an authored re-tune in the portal file only (every other model's spin is identical), NOT a unit change |
| everything else (speed, lifespan, rate, area, gravity, sizes, colours, alpha, drag, tumble, wind, follow) | identical | identical | |

So the particle "index" change is one slot: zSource moved to EXP2 and the old slot holds 255.

### 10.2 External .anim files are chunked

171 of 172 Northshire models set header flag 0x2000 ("chunked anim files"). Their .anim files begin with
an `AFM2` chunk (the track data; offsets relative to the chunk payload) followed by `AFSA` / `AFSB`
(skeleton attachment / bone data). noggit's track reader assumed a raw file. `Model::finishLoading`
now rebases the ClientFile onto the AFM2 payload. None of the 172 local models actually carries an AFID
(all their sequences are in-file, e.g. rabbit.m2 18/18, the portal 4/4), so this is protective for
models that do (characters, most creatures with many animations).

### 10.3 WMO-only maps: the global WMO is a fileDataID

`moltencore.wdt`: MPHD flags 0x241 (global object, MAID), MAIN with 0 tiles, **no MWMO**, MODF
`nameID = 108286, flags = 0x8` (modf_entry_is_filedata_id). noggit built the global WMO instance from
`globalWMOName` (the MWMO string), which was empty -> nothing drawn. `MapIndex` now resolves a
flags-0x8 nameID through the listfile (or a registered synthetic `fdid/<id>.wmo`) and the existing
`WMOInstance(mWmoFilename, &mWmoEntry)` path draws it.

### 10.4 Unlisted paths: open by root name hash

The community listfile names 159,749 of the 213,203 files; 1,210 map folders exist but the dungeon
folders are largely unnamed (`world/maps/mauradon/` -- Blizzard's spelling -- has no listed WDT). The
Classic Era root carries the Jenkins96 name hash for EVERY entry (421 of 421 blocks). `CASCArchive::openFile`
/ `exists` now fall back to `CascOpenFile(path, CASC_OPEN_BY_NAME)` when the listfile has no id, so
`world/maps/<Directory>/<Directory>.wdt` opens straight from Map.db2's Directory. Verified offline:
`WORLD\MAPS\MOLTENCORE\MOLTENCORE.WDT` hashes to fdid 788659 in the root.

### 10.5 Creatures, game objects, items: the tables had no adapters

`CreatureDisplayInfo`, `CreatureModelData`, `CreatureDisplayInfoExtra`, `GameObjectDisplayInfo`,
`ItemDisplayInfo`, `CharHairGeosets`, `CharacterFacialHairStyles`, `ParticleColor` stayed empty ("has no
DB2 adapter"), so no display id resolved to a model. Adapters now re-emit the 3.3.5 layouts (column
tables in `ModernDBC.cpp`). The index changes per table:

| Table | 3.3.5 | 1.15.9 (layout) | Adapter rule |
|---|---|---|---|
| CreatureModelData | `ModelName` path | `FileDataID` (3FFEE408) | listfile path, else synthetic `fdid/<id>.m2` registered with the listfile |
| CreatureDisplayInfo | `TextureVariation[3]` names relative to the model folder | `TextureVariationFileDataID[4]` (F75F884C) | full path (World.cpp keeps a variation as-is when it contains '/'); `PortraitTextureFileDataID` likewise; ParticleColorID written to columns 12 and 13 (noggit reads 12, wowdev lists 13) |
| CreatureDisplayInfoExtra | `NPCItemDisplay[11]`, `BakeName` | items in `NPCModelItemSlotDisplayInfo` (parent = extra id, `ItemSlot` 0..10 in the same head..cape order), `BakeMaterialResourcesID` -> TextureFileData (F5E49981) | slots gathered by parent; bake texture by material resource |
| GameObjectDisplayInfo | `ModelName`, `Sound[10]` | `FileDataID`, no sounds (7C5F0B90) | path as above; sounds 0 |
| ItemDisplayInfo | `ModelName[2]`, `ModelTexture[2]`, `Texture[8]` | `ModelResourcesID[2]` -> ModelFileData (fdid rows parented by resource), `ModelMaterialResourcesID[2]` -> TextureFileData, `ItemDisplayInfoMaterialRes {ComponentSection 0..7, MaterialResourcesID}` (9F3AB8A9) | first fdid per resource; component order ArmUpper ArmLower Hand TorsoUpper TorsoLower LegUpper LegLower Foot = the 3.3.5 Texture[8] order |
| CharHairGeosets | `RaceID` inline | `RaceID` relation (0DFA0F80) | parent id |
| CharacterFacialHairStyles | RaceID SexID VariationID Geoset[5] | same + non-inline id (7D8B51FB) | direct |
| ParticleColor | Start/MID/End[3] | same (D58506F1) | direct |
| CharSections, HelmetGeosetVisData | present | **gone** (ChrCustomization* / ChrModel / TextureFileData replace them) | no adapter: humanoid NPCs get the base character model, no skin/face/hair textures yet |

Open: the Legion+ character customization chain (ChrModel -> CreatureDisplayInfo, ChrCustomizationElement
/ Material -> TextureFileData) for humanoid skins; CreatureSoundData (sound only).

### 10.6 Verification (builds 21-24, OpenGL, isolated bench copy)

- Molten Core (map 409), camera `mc_lavafalls (789.6, -100, -1125.6)`: the global WMO
  `blackrock_lower_guild.wmo` (fdid 108286, 11 groups with MLIQ lava) loads and renders the interior
  with lava; capture `hz/era_mc4.png`. Two harness-side fixes were needed to even see it:
  `NoggitWindow::enterMapAt` replaced any requested position on a global-WMO map with the WMO's outer
  corner (now only when no explicit position / bookmark was given), and the bench readiness flag was
  raised only by the terrain tile pack (now also by the global WMO's finished load).
- Northshire, facing south: unchanged from build 19 (trees keyed, 0 exceptions), and the
  "has no DB2 adapter" list shrank from 26 tables to sound / misc ones plus `CharSections` and
  `HelmetGeosetVisData`, which do not exist in 1.15.
- Creature / game-object spawns come from the project's MySQL world tables; the bench project has no
  database, so the display-id chain was verified by the adapters loading (no empty-table errors) and
  the column mapping above, not by a rendered spawn.

## 11. Sound and music (2026-09-16, third pass)

User report: no music or sound. Two independent causes.

**Tables.** `SoundEntries.dbc` no longer exists: `SoundKit.db2` (2EB0F915: SoundType, VolumeFloat, Flags,
MinDistance, DistanceCutoff, EAXDef, SoundKitAdvancedID, ...) carries the row and `SoundKitEntry.db2`
(8F82FF7D: SoundKitID relation, FileDataID, Frequency, Volume, PlayerConditionID) the files; `SoundKitName`
is not shipped in 1.15. The adapter rebuilds the 3.3.5 30-column row: DirectoryBase = the listfile
directory of the kit's files, File[10] = the basenames (the players compose `dir + "\\" + file`),
Freq[10] from Frequency; entries whose fdid the store does not ship (SoundKitEntry lists retail ids,
e.g. 566017) are dropped. `ZoneMusic`, `ZoneIntroMusicTable`, `FootstepTerrainLookup`, `TerrainType`,
`CreatureSoundData`, `SoundProviderPreferences`, `WMOAreaTable` keep their columns (relations for WMOID);
`SoundAmbience` keeps `AmbienceID[2]` and adds start/stop kits; `SoundWaterType` is gone and is
synthesised from `LiquidType.SoundBank / SoundID` (one row per fluid speed 0/4/8 so the
class + speed lookup always hits); `LoadingScreens` maps the wide / narrow fdids.

**Files.** The listfile names 266,347 `.ogg`, 6,501 `.mp3` (music) and 1 `.wav` under `sound/`. Every
effect is Ogg Vorbis. noggit hands archive bytes to Qt5 through a temp file: `QMediaPlayer` (DirectShow /
WMF on Windows, no Vorbis decoder on a stock machine) and `QSoundEffect` (WAV only) -- so every effect
played as silence while mp3 music would have worked once the tables existed. `Noggit::Audio::decodeForQtMedia`
(stb_vorbis, `src/external/stb`, compiled in `ui/SfxPlayer.cpp`) decodes "OggS" data to 16-bit PCM WAV
and the temp file gets a `.wav` suffix; mp3 / wav pass through. Applied in SfxPlayer, ZoneMusicPlayer
(music and ambience decks), WaterSoundPlayer and the SoundEntry picker.

## 12. Terrain on the Season of Discovery maps (2026-09-16, fourth pass)

User report: broken texture blending on newer Classic Era maps such as Scarlet Enclave. Map.db2 names the
SoD maps by number (Directory "2856" = Scarlet Enclave, "2868" = Eastern Plaguelands Scarlet Raid Phase),
so their files are `world/maps/2856/2856_40_25.adt`, unlisted in the community listfile and reached only
through the root name hash (sec 10.4).

Survey of tiles (38..40, 25) of map 2856 against Northshire:

| | Northshire (azeroth.wdt MPHD 0x248) | Scarlet Enclave (2856.wdt MPHD 0x3ca) |
|---|---|---|
| MCLY flags | 0x100 only | 0x100 + **0x200 alpha_map_compressed** on ~all layers |
| MCAL per layer | 2048 bytes (4-bit) | RLE streams, 132..10,571 bytes per chunk |
| MPHD 0x4 big alpha | absent | absent |
| MCNK flags | -- | 0x8000 do_not_fix_alpha_map + 0x10000 high_res_holes on all 256 chunks |
| MHID / MTXP | zero / absent | zero / absent (MPHD 0x80 "height texturing" is set but no height textures ship) |

noggit's `Alphamap` tested the compressed flag only inside the big-alpha branch (in WotLK data 0x200 never
occurred without MPHD 0x4), so every RLE layer was read as a 4-bit 2048-byte map: the log showed
`MCAL: 4-bit alpha layer truncated (693/2048 bytes left in file)` and the blend came out as noise. The
per-layer flag now decides first: 0x200 = RLE 64x64 regardless of the WDT; uncompressed layers keep the WDT
rule (0x4 = 4096 bytes, else 2048 4-bit). This matches the client (wowdev ADT/v18 MCAL: "compressed if
MCLY flags & 0x200; else big alpha if MPHD 0x4 (or MCNK ...)"). The high-res holes and do-not-fix flags
were already handled by the modern tile loader (41).

Verification (build 26): map 2856 tile (40, 25) from above, camera (21600, 420, 13500): before, a single
rock texture over the whole tile and 1400+ `MCAL ... truncated` lines; after, the dirt and path layers
blend over the rock and the log has none. Captures `hz/era_se.png` / `hz/era_se2.png`. Sound build (25):
the sound tables no longer report "no DB2 adapter"; the remaining empty tables are Spell* / Faction*
(game-view spell visuals) and HelmetGeosetVisData.

### 12.1 The uncompressed layers: MPHD 0x80 means 8-bit too (sixth pass)

User report after build 29: "still broken ground textures in some areas in scarlet enclave raid" (New Avalon,
tile 42,28, camera over (22879, 211, 15434)). Survey of the tile's `_tex0.adt` and its neighbours:

| Tile (map 2856) | RLE layers (MCLY 0x200) | uncompressed layers | their size |
|---|---|---|---|
| 42,28 | 625 | 3 (chunks 13,4 / 15,8 / 7,12) | 4096 bytes each |
| 40,25 | 606 | 2 | 4096 |
| 41,28 | 580 | 5 | 4096 |
| 42,29 | 549 | 1 | 4096 |
| 43,27 | 430 | 0 | -- |

Every uncompressed layer is 8-bit (4096 bytes) although the WDT (0x3ca) has no MPHD 0x4: the client
selects the 8-bit formats when the WDT carries 0x4 OR 0x80 (wowdev ADT/v18 MCAL: the 4096-byte and
compressed forms are used "when bit depth is 8, i.e. 0x4 or 0x80 set in the WDT's MPHD"). noggit's
`MapIndex` set its big-alpha mode from 0x4 alone, so those few chunks per tile were read as 4-bit 2048-byte
maps -- each byte split into two nibbles, the blend a fine noise over the grass. `mBigAlpha` now also
follows 0x80 on modern (CASC) projects; the 3.3.5a client ignores 0x80, so WotLK projects keep the 0x4-only
rule and the classic-project exception (Turtle maps with garbage MPHD flags) is unchanged.

Verification (build 31, isolated bench copy, map 2856): straight down over chunk 13,4 of tile 42,28 from
120 units, the previous build draws the grass with a regular dotted streak pattern (each 8-bit byte split
into two nibbles), the new build a smooth blend; chunk 15,8 is unchanged (its second layer is nearly
transparent there). Captures `hz/era_se3b_before.png` / `hz/era_se3b_after.png`, `hz/era_se3c_*.png`.
The same run's log no longer lists HelmetGeosetVisData among the tables without an adapter (sec 14).

## 13. Equipment on NPCs: one item, many files (2026-09-16, fifth pass)

User report: shoulder pads far too large and misplaced, a missing helmet, a wrong body texture.
`ItemDisplayInfo` (9F3AB8A9) no longer names files: `ModelResourcesID[2]` and `ModelMaterialResourcesID[2]`
name RESOURCES, and `ModelFileData` (2AE4E788, fdid rows parented by ModelResourcesID) / `TextureFileData`
(BD7C74C2, parented by MaterialResourcesID) list every file of a resource. `ComponentModelFileData`
(AD90D87A: GenderIndex 0/1/2-any, ClassID, RaceID 0-9, PositionIndex 255/0/1) and
`ComponentTextureFileData` (B32B030A: GenderIndex 0/1/3-any, ClassID, RaceID) say which wearer each file
is for. Measured in 1.15.9: 8,263 model resources with one file, 131 with two, 113 with sixteen (helmets: 8
races x 2 genders), 2 with fifteen; 19,671 material resources with one texture, 2,004 with two (male /
female), 37 with three. The first-file pick of the previous adapter therefore handed a human male the
first race's helmet and a random gender's texture, and noggit's WotLK race-suffix rule (`_HuM` etc.)
cannot fit files that carry no such suffix.

Now: a resource with ONE file resolves to its path in the row; with several, the row carries
`modelres:<ModelResourcesID>:<side>` or `matres:<MaterialResourcesID>` and `World.cpp` resolves it with the
wearer (`Noggit::DB2::resolveItemVariantName(name, race, sex)`) at the three consumers: attachment
models (`resolve_item_attachment_model_path`), attachment textures (`resolve_texture_path`) and the body
texture compositor (`resolve_item_texture_component_filename`, now gender-aware) plus the cape texture.
Selection: side first when the resource distinguishes sides, then exact race + gender, race + any
gender, any race + gender, generic; class-specific files are skipped (NPC displays name no class).
Unlisted files get a registered synthetic path. The selection panel now shows `fdid:<id>` for objects the
listfile does not name.

## 14. Humanoid NPCs: hair, facial hair, helmet visibility (2026-09-16, sixth pass)

User report (build 29, a Steamwheedle deck: goblin, two humans in Defias bandanas): "npcs missing hair".
noggit dressed NPCs the WotLK way -- CreatureDisplayInfoExtra.HairStyleID -> CharHairGeosets for the hair
geoset, CharSections for the hair / skin / face textures, HelmetGeosetVisData for what a helm hides. In
1.15.9:

| Table | 1.15.9 | Consequence |
|---|---|---|
| CharSections | absent (listfile names it; the root has no such fdid) | no hair texture (type 6), no extra skin (type 8) |
| HelmetGeosetVisData | absent | `helmet_vis != 0 -> bald` fallback: the bandana (vis 247) balded its wearer |
| CharacterFacialHairStyles | present, 136 rows, but RaceID / SexID / VariationID are 0 on every row | facial hair never resolved |
| CharHairGeosets | present, 148 rows | hair geoset resolved -- but from a column the client no longer reads (below) |
| CreatureDisplayInfoOption | 35,409 rows, 7,283 of 7,287 extras (5 per NPC) | the client's actual source |
| ChrCustomizationOption / Choice / Element / Geoset / Material, ChrModel, ChrRaceXChrModel, ChrModelTextureLayer, CharComponentTextureLayouts / Sections, HelmetGeosetData | present | |

### 14.1 The chain

`CreatureDisplayInfoOption` (parent = the extra id) lists the chosen `ChrCustomizationChoice` per option
(skin, face, hair style, hair colour, facial hair; goblin males have only skin + style). Each chosen choice's
`ChrCustomizationElement` rows apply when their `RelatedChrCustomizationChoiceID` is 0 or also chosen:
hair MATERIALS hang on the STYLE choice related to the COLOUR choice, face materials on the FACE choice
related to the SKIN choice, facial-hair materials on the FACIAL choice related to the COLOUR choice
(measured the same way on 1.14.2 in RE_notes/33). Elements carry either a `ChrCustomizationGeoset`
{GeosetType, GeosetID} = geoset Type*100+ID (type 0 = hair, id 0 = none; 1/2/3 = the facial families with
absolute ids; 16/33 troll-only) or a `ChrCustomizationMaterial` {ChrModelTextureTargetID,
MaterialResourcesID}. The race/sex `ChrModel` (via `ChrRaceXChrModel`, 18 rows for races 1-9) names a
`CharComponentTextureLayoutID` (1 = 512x512, 2 = 1024x512) whose `ChrModelTextureLayer` rows map a target to
an M2 texture type: target 10 -> type 6 (hair), target 2 -> type 8 (extra skin), every other target (1 base,
4/5 face, 7/8 facial, 11/12 scalp, 13/14 pelvis/torso, 16) -> type 1, the body composite. For NPCs that
composite is the bake (`BakeMaterialResourcesID` -> TextureFileData, set on 7,285 extras), so only types 6
and 8 need the chain; the base skin (target 1) is kept as the fallback when a bake is missing.

Measured against the legacy columns (all 7,283 extras with options): the hair geoset agrees on 6,979 and
differs on 247 (e.g. human male extra 182: HairStyleID 11 -> CharHairGeosets 12, chain 2); 428 chain NPCs
are bald (type 0 id 0); 6,433 have a type-6 hair texture, 541 a type-8 extra skin. Troll male style choice
17829 carries two type-0 elements (geoset 1, then 0); CharHairGeosets resolves that variation to 1, so the
first element per type wins. The 1.12 Turtle rule that gives race 9 a Geoset100 hair variant is skipped on
modern projects: the Classic Era goblin (ChrModel 17/18) has ordinary group-0 hair.

### 14.2 Helmets

`HelmetGeosetData` (313 rows, parent = the old HelmetGeosetVisData id, still what
`ItemDisplayInfo.HelmetGeosetVis[2]` indexes: 15 of the 16 vis ids used by 33,332 items) lists one row per
race per hidden geoset group -- 0 hair (101 rows), 1/2/3 facial (41/51/65), 7 ears (55); RaceBitSelection is
0 everywhere. Re-emitted as the 3.3.5 layout (five race masks: hair, facial 1-3, ears). Row 247 hides the
facial groups and keeps the hair, exactly the WotLK bandana row, so on modern projects the hair column is
read as the race mask it is (the WotLK "any bit" rule stays for WotLK data).

Code: `Noggit::DB2::creatureCustomization(extra, race, sex)` (ModernDBC) walks the chain once per NPC;
`resolve_creature_geoset_selection` applies its geosets instead of the legacy hair / facial lookups when
the extra has options, and the texture resolver feeds the chain's type-6 / type-8 / target-1 paths into the
section slots the WotLK code already consumes. Adapter `buildHelmetGeosetVisData` from HelmetGeosetData.

## 15. Modern WMO interiors: batch material ids and MNLD lights (2026-09-16, seventh pass)

User report (build 31): "karazhan crypts dungeon is full of missing textures inside the WMO. also missing the
torch lights from the floating torch mechanic". Karazhan Crypts is map 2875 (Directory "2875", 12 tiles at
34..36 x 50..53, MPHD 0x3c8, no global object); the crypt is `world/wmo/dungeon/md_crypt/classic_md_crypt_d.wmo`
(fdid 6403827, unlisted in noggit's listfile, placed by tile MODF flags 0xc with doodad set 1 "Karazhan
Crypts Fur", rotation (0, 181.5, 0), 19 groups + 3 LOD levels in GFID, 28 materials (25 Opaque, 3 Diffuse),
2,018 doodads of 425 models, all textures and models openable). Reproduced in the bench (map 2875, cameras at
the group centres computed from MOGI through noggit's placement transform): every room drew black with the
doodads floating in it (`hz/era_kara_g2/g6/g8.png`). Two data changes, both also present in the Scarlet
Enclave monastery of sec 13:

### 15.1 MOBA `flag_use_material_id_large`

wowdev SMOBatch (Legion+): flag 0x2 moves the material index to the uint16 at +0x0A (inside the old
bounding-box bytes) and leaves `material_id` (+0x17) at 0. Measured: crypt 172 of 172 batches carry the flag
(ids up to 24), the monastery 308 of 308 (ids up to 47). noggit read `material_id` only, so EVERY batch of
these WMOs used material 0 -- the "missing textures" (one texture over the whole crypt) and the monastery's
"broken textures" report that the sec 13 captures could not pin (material 0 there happens to be a stone). The
group loader now takes the 16-bit id when the flag is set (a value above 255 is logged and left alone; the
raw batch struct keeps its 8-bit field).

### 15.2 Interior light: MNLD / MNLR replace MOLT / MOLR

| Source | Crypt | Monastery |
|---|---|---|
| MOHD ambient | 0xFF000000 (black) | -- |
| MOCV mean rgb per group | 1.9, 1.5, 0.0, 0.0 ... (of 255) | 13.7, 4.8, 14.2, 6.2 ... |
| MOLT / MOLR refs | 5 lights / 0 refs | 0 / 0 |
| MNLD / MNLR refs | 283 lights / 389 refs (every group) | 0 / 0 |
| MDAL | 0xFFFFFFFF on all 19 groups | 0xFFFFFFFF on 33 of 37 |

So the crypt has no baked light at all: the Shadowlands+ `MNLD` chunk (184-byte records: type 0 point / 1
spot, lightIndex, flags, doodadSet, inner colour, position, rotation, attenStart, attenEnd, intensity, outer
colour, blend range, flicker, cookie fdid, falloff, cone angles) carries its 237 point and 46 spot lights --
inner colours (53,109,110) blue-grey, (204,112,60) / (255,205,96) torch oranges, attenEnd 0.7..45, intensity
0.09..1.0, doodadSet 0 on 272 and 1 on 11 -- and each group's `MNLR` lists which of them light it (3..10 per
room). The groups' `MOLR` is absent (their 0x200 flag refers to MNLR). The user's "torch lights" are these
MNLD lights sitting on the torch doodads. `MDAL` is 0xFFFFFFFF wherever it appears (both WMOs), i.e. an
unset sentinel rather than a white ambient; it is left unapplied.

Implemented: the root parses MNLD into `WMO::new_lights` (same WMOLight record: position swapped like
MOLT, colour, intensity, attenEnd as the cull radius, plus doodad set and type), groups parse MNLR into
`new_light_refs`; `WMORender` feeds a group's MNLR/MNLD pair to `setWmoGroupPointLights` when present and
MOLR/MOLT otherwise, and the global pool (`collect_wmo_lights`) adds the MNLD lights of every loaded WMO
whose doodad set is 0 or the instance's. Spot lights are drawn as point lights at their position; the
falloff is the WMO shader's existing (1 - d/r)^2 with r = attenEnd (the WotLK MOLT 6-unit floor does not
apply to MNLD). The MNLD field semantics come from wowdev; the modern client's exact falloff curve is not
measured here.

Verification (build 32, bench, map 2875): with the diffuse-only WMO debug view the crypt draws fully textured
-- ceiling slabs, brick walls with skull reliefs, dirt floor (`hz/era_kara_g6_dbg1.png`; before the batch
fix every one of those was material 0's pillar stone). Beside the three strongest MNLD lights (colour
238,194,38, intensity 1, attenEnd 45, group 13) the vault hall renders lit in exactly that gold
(`hz/era_kara_l1.png`, `hz/era_kara_l2.png`) -- the only light source that room has, since its ambient and
MOCV are black. The rooms at the centres of groups 2, 6 and 8 stay dark: their MNLR lights are the
blue-grey 53,109,110 torches with attenEnd 4.5..6.3, so only the walls within a few units of a torch pick
up light, which is the authored look of the dungeon (its darkness is the mechanic). Anything the user
sees as a "floating torch" that is spawned by the server (creature / game object) is outside this: noggit
collects M2 lights from placed doodads only.

## 16. Doodads inside dark interiors (2026-09-16, eighth pass)

User report (build 32, Karazhan Crypts game view): bones, skulls and the character stand fully lit inside a
black room -- "all these objects need to be affected by the shadow, black fog or whatever they have in this
map". Data: the crypt's 2,018 MODD entries carry colour (0,0,0,255) on 1,747 of them and an MDDI colour
multiplier of 0.0 on 1,841, i.e. no authored light for the props beyond the room's point lights; the
monastery's MODD colours are real ((118,89,40) x 0.46 ...). noggit lights a WMO doodad the way the 3.3.5a
client lights a unit standing in an interior group (RE_notes/15): the baked MOCV floor colour under it,
sampled through `WMOGroup::sample_ground_color`, which for the crypt is black -- so the props SHOULD go
black plus point lights. Replaying that sampler offline under the group-6 camera finds a collidable floor
4.8 units down with MOCV (0,0,0); the in-process diagnostic (`NOGGIT_LIGHT_DEBUG=1`, extended to print the
containing volume's group, its lit-floor colour count, grid cell and the direct sample) agrees:
`sample=1 (0,0,0) floorY=-44.6` -- yet the cached probe stayed `(0,0,0 a=0)` = outdoor.

Cause: `interior_light_at` caches OUTDOOR results for static objects. The first probe of a cell runs
while the WMO's volumes do not exist yet (the async loader flips `finishedLoading` mid-frame, after the
frame's WMO-set fingerprint check, so the WMO's own doodads are classified against the previous volume
list in the very frame the WMO appears) and that outdoor value is never revisited: the WMO-set change
deliberately does not wipe the cache (the 2026-08-04 stream-hitch fix). A second freeze sits in the
per-instance-animated doodad path: the persistent instance-buffer snapshot stores whatever the sampler
returned, including samples the 48-per-frame budget DEFERRED (returned as outdoor), until the next WMO-set
change.

Fix: on a WMO-set change every cached OUTDOOR entry is dropped (interior entries depend only on their own
static WMO and stay, so the steady state still recomputes nothing), and the instance-buffer snapshot is
rebuilt on the next frame whenever a sample it took was deferred. WotLK-era WMOs keep their exact
behaviour; the change only matters for objects that were classified before their room existed.

### 16.1 The doorway spill on do-not-fix files

With the cache fixed the probe classified the crypt cells as interior but with the spill encoding at its
maximum (`a=1`): the floor's baked alpha read 255, and the doodad shader lerps the room light fully toward
the outdoor light at that value. Baked alpha per group on the same map:

| WMO | root flags | deep-interior groups | entrance groups |
|---|---|---|---|
| crypt (SoD) | 0x41f (0x8 do-not-fix set) | alpha 255 on every vertex (16 of 19 groups) | group 0: 943 of 1,188 verts at 0..63 |
| monastery (SoD) | 0x41f | 255 everywhere (29 of 33 MOCV groups) | groups 20/21/26: mean 48 |
| duskwood inn (WotLK-era) | 0x0 | 0 everywhere (max 0..16) | group 1: mean 106, up to 255 |
| guard tower (WotLK-era) | 0x0 | mean 14 | up to 255 near the door |

The raw alpha of a do-not-fix file is the inverse of a WotLK file's. wowdev (CMapObjGroup::FixColorVertexAlpha):
with MOHD 0x8 the client skips the fix except for one step -- the vertices of the interior and exterior
batches get alpha 0 / 255 and only the transition batch keeps its authored alpha. noggit's face path already
applies exactly that in `load_mocv`, but the floor-alpha table for the unit/doodad sample was built from the
PRISTINE alpha captured before it. It now applies the same rule: on a do-not-fix file every vertex from the
interior batch on samples as alpha 0 (deep interior, no spill); transition-batch vertices keep their authored
value. WotLK files (no 0x8) are untouched.

### 16.2 Point lights on interior M2s

With both fixes the props went dark like their rooms, and the vault hall (sec 15 verification) showed the
last gap: its walls glow gold from the MNLD lights while every statue, drape and pillar is a black
silhouette. The M2 interior branch adds no point lights at all -- the 3.3.5a rule measured on units
("zero point lights reach an interior M2", MOLT lights become linear-falloff directionals the unit path
skips). A Shadowlands-style room has nothing else: black ambient, black MOCV, only MNLD. So the light
buffer now tags each point light in colour.w -- 0 M2 light, 1 WMO MOLT, 2 WMO MNLD -- and the interior
branch adds the nearest MNLD lights with the same 1/(0.7d + 0.03d^2) falloff the outdoor branch uses; the
unit rule keeps skipping MOLT only. WotLK data never carries a 2.

## 17. New client product with no listed build: layout-hash lookup (2026-09-16, ninth pass)

User report: a new project "Forever Beta" (product `wow_classic_beta`, the 1.60.1.69893 client) shows no
maps. Log: every DB2 adapter fails with `layout <hash> (build '1.60.1.69893') is not in
definitions\<table>.dbd`, Map included -- yet most of those hashes ARE in the shipped files (ItemDisplayInfo
9F3AB8A9, SoundKit A7FB0451, Light 5F16BC84 ...). Two causes:

1. `blizzard-database-library` `DatabaseDefinition::Read` cut `LAYOUT_TOKEN.size() - 1` characters off a
   `LAYOUT` line, so every stored hash was "T <hash>" and a lookup by layout hash could never match. It
   went unnoticed because the loader falls back to the client build, and every client until now
   (1.15.9.69722, 2.5.6.69795) was listed under its layouts. 1.60.1.69893 is listed nowhere in the shipped
   snapshot, so nothing resolved. The token length is fixed (COMMENT had the same off-by-one); hash lookup
   is now the primary path it was meant to be.
2. The shipped definitions (the `dist/definitions` submodule, copied next to the executable at build time)
   predate the 1.60.1 layouts: Map D43AFAC3, AreaTable 9995B797, LightParams A7F31923 and LightData 360DA016
   exist only upstream. The submodule working tree, the release folder and the bench copy were refreshed
   from WoWDBDefs master (1,342 definitions, which lists both 1.60.1.69876 and 1.60.1.69893).

### 17.1 Files the beta lists but cannot deliver

With the tables loading, the bench crashed in `blp_texture::finishLoading` under `LiquidTextureManager::upload`
(TextureManager.cpp:827, a null BLP header). The beta client keeps unreleased content behind TACT keys
CascLib does not have (its `map.db2` frame a6942aae... is one; the Python CASC reader refuses it with
"encrypted BLTE frame"). Such a file OPENS (so `exists` said yes) and fails on the first read; the archive
layer's `readFile` ignored the failure (an `assert` that Release builds drop) and returned a non-EOF file
whose buffer was never filled -- for a 0-byte size an empty vector, whose data pointer the header read
dereferenced. Fixes: `CASCArchive::exists` probes one byte after opening and reports an unreadable entry as
missing (logging the CascLib error, 1005 = encrypted); `ClientData::readFile` treats a failed or empty
read as missing; `blp_texture` rejects files shorter than a BLP header; the liquid uploader skips a
liquid type whose texture cannot load instead of taking the map down.
The texture-painter preview (`BLPRenderer::render_blp_to_pixmap`) draws a black swatch instead of
letting the read exception escape, and `CASCArchive::getFileSize` reports 0 when CascLib has no size.

Verification (build 40, bench, Forever Beta, map 0): the map view opens and runs its 120 frames without an
exception; the log lists 32 entries CascLib reports as encrypted (Windows error 6002 = ERROR_FILE_ENCRYPTED),
the fallback `textures/shanecube.blp` itself as empty, and every `xtextures/12_water/...` liquid texture as
missing, so the terrain draws without those textures. The beta store on this machine is a partial
Battle.net install with TACT-keyed content: the root has 1,441,771 entries (2,741,566 records, files carry
one record per locale) but the local index holds 1,393,675 entries shared with Classic Era, and the
beta's `light.db2`, `areatable.db2` and `map.db2` frames need keys CascLib does not have. Maps now list;
what a map shows depends on which of its files the local store can deliver. What is NOT in scope here: a
TACT key source for the beta and CascLib's choice between the two content-flag variants the beta root
carries for plain files (0x12080001 and 0x12080000 for shanecube; the second one is readable, CascLib
picks the first).

### 17.2 White terrain, yellow sky: the beta's LightData Time field

User report (build 40, Forever Beta, Elwynn): terrain white, sky yellow -- "they added new shadows and
lighting for this client but idk if this is the issue". It is not the lighting model: the beta's tileset
textures read fine (7 of 7 on tile 32,48) and `Light`/`LightParams`/`LightSkybox` keep their 1.15.9 layouts
(map 0 loads 80 light rows). `LightData` moved to layout 360DA016 (nineteen new `Field_1_60_1_*` columns
after the fog fields) and, measured on all 5,375 rows, its `Time` column stores `0x10000 | half-minutes`:
raw 65536..68406, low 16 bits 0..2870 in the same 47 distinct steps as 1.15.9, and every other field of
a matching row (LightParamID 1: direct 0x5e99c6, ambient 0x1a3855, sky 0x536f) identical to the 1.15.9
row. Bit 16 is set on every row, so it is a constant tag, not a time. noggit's band adapter copied the raw
value into the WotLK LightIntBand/LightFloatBand keys, which the sky interpolates over 0..2880: every key
sat past the end of the day, the interpolation degenerated (the sky init also logged "error with getting
an entry in LightIntBand DBC (0)"), and the frame came out with white light and a yellow fog band. The
adapter now keeps the low 16 bits.

### 17.3 Untextured terrain: CascLib keeps the first of two root records

After the Time fix the beta's Elwynn rendered as flat dark shapes under a proper sky: no terrain
texture at all, although the Python CASC reader delivers every tileset texture of the same tile. The beta
root (TSFM version 2, 1,441,771 ids in 2,741,566 records) lists most plain files twice, in two groups with
content flags 0x12080001 and 0x12080000 (same locale mask 0x1f3f6). Measured with the Python reader on
both records: `textures/shanecube.blp` -> 700,236 bytes / 44,876 bytes (both readable, different
assets); `tileset/generic/black.blp` -> first record's EKey "not found locally or on CDN", second record
1,513 bytes. CascLib's `CASC_FILE_TREE::InsertById` keeps the FIRST record for a FileDataId, so noggit
opened the unavailable variant: `CascOpenFile` succeeded (the root entry exists), `CascGetFileSize64` failed,
and with the readFile guard of 17.1 the file counted as missing -- every tileset texture, and the
fallback, gone. The library is fetched from upstream at configure time, so the fix lives in
`src/external/casclib-patch/FileTree.cpp`, copied over the fetched tree by `cmake/FindCascLib.cmake`:
when a duplicate id arrives and the stored entry has no local data (`CASC_CE_FILE_IS_LOCAL` clear) while
the new one has, the node takes the new CKey entry. Which of the two variants the real client prefers
(the 0x1 content-flag bit is not in CascLib's flag list) is not measured here; a locally present copy is
the only usable choice for an editor.

### 17.4 Maps the list dropped: WdtFileDataID

The beta's map list also lost 30 ids (13, 2720, 2784, 2789, 2791, 2804, 2806, 2807, 2817, 2832, 2853,
2856, 2868, 2875, 2902, 2921, 2959, 2980, 2991, 2995..2999, 3002, 3005, 3021, 3065, 3104, 3109) with
"has no WDT file": those maps live in numbered folders the community listfile does not name, and unlike
the Classic Era root (sec 10.4) the beta root carries no name hashes (content flag 0x10000000 on every
block), so the path test fails. Map.db2 has carried `WdtFileDataID` since 8.1; the Map adapter now
registers `world/maps/<Directory>/<Directory>.wdt` -> that id with the listfile when the listfile lacks
it, logs each such map with its name, and the split tile files come from MAID as before. The "has no WDT"
line now prints the map name too.

Verification (build 42, bench, Forever Beta): Elwynn from the Westfall border renders textured terrain,
trees and a blue sky (`hz/forever_sky2_rgb.png`; the bench PNG keeps the alpha channel, which terrain
writes as 0 for the bloom mask, so view it without alpha). The 30 dropped maps now resolve through their
WDT id: 2720 The Searing Basin, 2784 Demon Fall Canyon, 2789 The Tainted Scar, 2791 Storm Cliffs, 2804 The
Crystal Vale, 2806 Shadow Hold, 2807 Burning of Andorhal, 2817 Starfall Barrow Den, 2832 Nightmare Grove,
2853 Deadwind Pass, 2856 Scarlet Enclave, 2868 Eastern Plaguelands (Scarlet Raid Phase), 2875 Karazhan
Crypts, 2902 The Scarab Dais, 2921 Naxxramas, 2959 City of Dalaran, 2980 Dalaran City, 2991 Zephras Isle,
2995 Hyjal Crater, 2996 Warsong Gulch (Winter), 2997 Darkspear Islands, 2998 Excavation Site: Wetlands,
2999 Ruins of Lordaeron, 3002 Half-Pint Tavern, 3005 Battle for Gilneas, 3021 Eastern Kingdoms
Preserved, 3065 The Hall of Thanes, 3104 "nothing to see here", 3109 Manor Mistmantle. Two stay out: 13
(test) has no WDT anywhere, and 3109's WDT (id 7937200) is not in the local beta store yet. Whether a map's
tiles are all present locally is a separate question the same partial install decides; the remaining 32
"cannot be read" entries are the TACT-keyed files of 17.1.

## 18. Fidelity variants in the Forever Beta root: what "SD / HD" is in the data (2026-09-16, tenth pass)

User: "in this client, they have two graphic fidelity, SD version and HD version of models, most models
like characters should have two maybe wmos or idk but check for that and add that feature for our noggit
to toggle between them".

Survey of the 1.60.1.69893 root (1,441,771 file ids, 2,741,566 records; 253,802 ids carry more than
one record). Grouped by what the records of one id differ in:

| files | ids | records differ in | meaning |
|---|---|---|---|
| ogg, mp3, db2, html, unnamed | 128,352 + 2,900 | locale flags only | one record per locale, same or per-locale data |
| blp | 95,774 | content flag bit **0x1**, different CKeys | the **HD / SD texture pair** |
| blp | 1,290 (+1,018 with the same CKey) | content flag 0x80 | low-violence variant (CascLib `CASC_CFLAG_LOW_VIOLENCE`) |
| m2 / skin / anim / bone | 1,174 / 1,827 / 1,769 / 96 | content flag 0x80 | low-violence creature and character models (scourge, skeleton, ...) |
| wmo | 0 | | WMOs have no variants at all |

Bit 0x1 is the high-resolution side: the same file id decodes to a 512x1024 BLP under 0x12080001 and a
128x256 one under 0x12080000 (`textures/shanecube`-class id 189543: 350,716 B vs 23,036 B), 512x256 vs
128x64 for 121595 (`character/scourge/faciallowerhair00_00.blp`). Both records are real files with
different CKeys; nothing else in the root marks them. Not every HD record is downloaded in a partial
Battle.net install (`creature/fireelemental/ember.blp`: HD EKey absent, SD 8x8 present), so the choice
must stay "local first".

Character models are a separate mechanism, not a root-record pair: the beta ships 23 WoD-style
`character/<race>/<sex>/<race><sex>_hd.m2` files next to 20 plain siblings, and BOTH have CreatureModelData
rows (`humanmale.m2` = CMD 49, `humanmale_hd.m2` = CMD 7661; the user's own session drew
`nightelfmale_hd.m2` 46 times and `humanmale.m2` 13 times). They do not share a texture layout: the plain
human male (16,848 vertices) spreads its UVs over the whole 0..1 square (CharComponentTextureLayouts 1 =
1024x1024, i.e. the classic 256x256 sections at 4x), the HD one (226,519 vertices) keeps most of the body
below u = 0.5 (layout 2 = 2048x1024 with the 1024x1024 face/hair region on the right). A bake texture made
for one does not fit the other, so swapping model files by name would be a guess, not a client rule.
CreatureDisplayInfo 1.60.1 (layout 7275F5F6) carries `ConditionalCreatureModelID<u16>`, the column the
client has for a conditional model swap; the file's BLTE frame is TACT-keyed for the offline reader (key
name 783882c6e7909a78) so which displays carry a conditional model could not be listed here. noggit loaded
the table without error in the user's session, so the in-process CascLib copes with it. No model-level
toggle was added on that basis; the texture pair is the switch the data supports.

**Implemented (build 44).** `src/external/casclib-patch/FileTree.cpp` exports
`CascSetPreferredContentFlags(mask, value)`; `CASC_FILE_TREE::InsertById` now decides between two records
of one id as: a record whose content is local beats one whose content is not (the 17.3 rule), and between
two local records the one whose content flags equal `value` under `mask` wins; mask 0 keeps the upstream
"first record" order. `BlizzardArchive::Archive::CASCArchive::setPreferredContentFlags` wraps it,
`ApplicationProject::loadProject` calls it with mask 0x1 from the Settings > Paths box **"Prefer HD texture
variants"** (QSettings `casc/prefer_hd_textures`, default on = what the user saw before) right before the
client storage opens; the root manifest is resolved once at `CascOpenStorageEx`, so the box applies when a
project is opened.

Verification (build 44, bench, Forever, Redridge cliff camera (19467, 150, 26400) yaw 90 pitch 25, 200
frames each): the same view with the box off and on differs by a mean of 6.7/255 per channel over the
near cliff face, and the on-capture carries 26% more high-frequency detail there (gradient energy 15.8 vs
12.5) -- the 4x texture, visibly sharper rock grain (`hz/forever_lake_sd_vs_hd.png`, left off, right on).
The QSettings value was absent before the test (default on) and absent after it.

## 19. PBR water: the beta's LiquidType material 130 (2026-09-16, tenth pass)

User: "they also got new water". MH2O of the first 900 continent tiles of the beta (Azeroth + Kalimdor,
through the WDT MAID root ids): liquid types 1250 (128,274 layers on 604 tiles), 1325, 1240, 1288, 1297,
1343, 1319, 1320, 7, 1324, 1313, 5; the instance's second u16 is a LiquidObject id (5924..6016), as in
8.x+, and `liquidObjectVertexFormat` already maps it. The rows (LiquidType layout D1ECEEC9, decoded
offline):

| id | Name | Flags | SoundBank | MaterialID |
|---|---|---|---|---|
| 1240 | PBRWater - Generic - Lake | 0x100F | 0 | 130 |
| 1250 | PBRWater - Generic - Ocean | **0x140F** | 0 | 130 |
| 1288 | PBRWater - Generic - River | 0xF | 0 | 130 |
| 1297 / 1313 | PBRWater - Generic - Swamp - Green / Brown | 0x100F | 0 | 130 |
| 1319 / 1343 | PBRWater - Generic - SpringWater - Forest / ClearSkies | 0x100F | 0 | 130 |
| 1320 / 1324 / 1325 | PBRWater - Teldrassil - City / Lake - Forest / Lake - Large | 0xF / 0x100F | 0 | 130 |

All 24 PBR rows have SoundBank 0 (the legacy Ocean row 2 has 1); bit 0x400 is set on exactly the two
"Ocean" rows and on none of the other 22. LiquidMaterial 130 = Flags 1, LVF 0 (height + depth).
`Texture[0..5]` are `xtextures\12_water\<zone>\12fx_water_<zone>_foam_high|mid|low|rim.blp` plus two
more -- the foam and rim maps of the new shader, listed in LiquidTypeXTexture as six file ids per row
(1240 / 1250 / 1288 all share 7475833, 7475835, 7475834, 7475836, 7660147, 7480794; Type 255) with no
`%d` frame sequence, unlike the classic rows (type 1 = 30 frames 219901..219930). The colour ramps sit in
the row's `Color[3]` / `Float[18]`; there is no base colour texture. The community listfile names none of
the 12_water files and the beta root carries no name hashes, so by name they resolve to nothing.

What noggit did with that: `LiquidTextureManager` keys a texture profile per liquid id from the DBC
Texture string, so every PBR id had none and `LiquidRender` fell back to profile 1 (river) for all of
them, ocean included (`Turtle water: missing liquid profile 1240`, 32 times in the user's log), and
`liquid_layer::mclq_liquid_type` classed the ids with the vanilla "groups of four" rule ((id-1) % 4):
1240 came out slime, 1343 magma, 1250 ocean by coincidence.

**Implemented (build 44).** The DB2 -> DBC LiquidType adapter translates material-130 rows into the WotLK
schema: Type = 1 (ocean) when Flags has 0x400, else 0 (water); Texture[0] = the classic frame template of
that family (`XTextures\ocean\ocean_h.%d.blp` / `XTextures\river\lake_a.%d.blp`, both readable in the
beta store); the other five strings stay. `liquid_layer::mclq_liquid_type` returns
`LiquidTypeDB::liquidClass(id)` (the Type column) on modern projects instead of the id heuristic.
**Second finding, same pass: the water was still BLACK after that.** Bench on Lake Everstill (tile 36_49,
132 layers of 1325, MH2O byte-identical to the Classic Era tile except the id: Era 5, beta 1325), the
Duskwood stream and the Swamp of Sorrows coast (1250): black surfaces, no "missing profile" line any
more. noggit colours water from the zone's LightData river/ocean bands (`OceanCloseColor` ... through the
lighting UBO), and the beta's LightData authors those on 421 of its 815 light params only; the 372 params
1.60.1 added for the PBR-water zones leave them 0 (the Classic Era table: 375 of 443, the same param ids
with and without). PBR water does not read them: the row carries `Color[3]` (12.x layout D1ECEEC9). Which
entry is which is not documented (wowdev stops at the 6.0 `Color[2]` with "no specific meaning"), so
from the 24 rows: entry 1 is the water body colour ("Fel Tainted - Lake" 00ff13 fel green, "Test 0"
ff001b red, Generic Lake 87beff sky blue), entry 2 is the darkest on 18 of 24 rows (deep colour), entry 0
is a pale tint repeated across unrelated rows (ccdbb3 on SkyElfLand lake, SkyElfLand river, WMO interior,
Stratholm; dcd7c6 on the Tanaris puddle, Desolace puddle and Lake - Dirty) -- a secondary term the
legacy shader has no slot for. Implemented (build 45): the adapter hands (Color[1], Color[2]) to the two
WotLK Color slots for material-130 rows; `LiquidRender` looks the row up once per liquid id and writes
0x80RRGGBB / 0x00RRGGBB into the two spare words of `LiquidChunkInstanceDataUniformBlock`
(`row_color_light` / `row_color_dark`); `liquid_frag.glsl` uses them as the shallow -> deep ramp when
bit 31 is set, keeping the zone's LightParams water alphas and the ocean/water depth ramp by the row's
class. Classic rows write 0 and render exactly as before.

Verification (build 45, bench, Forever, 300 frames each): Lake Everstill (19467, 150, 26400) blue with the
authored shore-to-centre gradient (`hz/forever_water2_lake_rgb.png`, frame mean 45 -> 108), the Swamp of
Sorrows coast (21600, 150, 29600) a teal ocean under the fog band (`hz/forever_water2_ocean_rgb.png`),
the Duskwood stream (18400, 150, 26933) pale and shallow (`hz/forever_water2_river_rgb.png`). No
"missing liquid profile" line in any run; the shader-uniform warnings are the same eight pre-existing
lines the build-44 run had.

NOT done: the PBR surface itself (foam/rim/normal maps, the Float[38] and Coefficient[4] terms) -- that
is a new water shader, and the six file ids above are where its inputs are.

## 20. LightData columns only 1.60.1 has (2026-09-16, tenth pass)

User: "they added new shadows and lighting for this client ... new fog and light". Layout 360DA016 has 63
columns: 0..42 are the 1.15.9 / retail set, 43 = `Field_10_0_0_44649_042` (0 on all 5,375 rows), 44 =
`Field_12_0_0_63854_043` (float 0..1, non-zero on 1,104 rows; present in 12.x too), then FIFTEEN
`Field_1_60_1_69876_045..059` that no retail layout has (12.1.5's 6ABF2921 stops at 48 columns), then the
three coefficient arrays. Values over the beta's 5,375 rows:

| column | type | values |
|---|---|---|
| 045, 046, 047 | u32 | 0 everywhere |
| 048 | float | 1.0 or 1.5, every row |
| 049 | float | 0 .. 4.0, 44 distinct, 5,358 rows non-zero |
| 050 | float | 0 .. 1.5, 10 distinct |
| 051 | float | 0 .. 8.0, 79 distinct |
| 052 | float | 0 .. 5.0, 145 distinct |
| 053 | float | 0 .. 6.1, 58 distinct |
| 054 .. 057 | u32 | 0 everywhere |
| 058 | float | 0.1 .. 5.0, 43 distinct, every row |
| 059 | float | 0.2 .. 3.0, 15 distinct, every row |

The fog columns the 8.x port added are unused by this data: FogHeight, FogHeightScaler, FogHeightDensity,
FogZScalar, EndFogColorDistance and FogStartOffset are 0 on every row, SunFogAngle and SunFogStrength are
1.0 on every row, MainFogCoefficients and HeightDensityFogCoeff are all-zero, FogHeightCoefficients is
non-zero on 81 rows. LightParams 1.60.1 (layout A7F31923) is the WotLK column set. So the fog noggit
renders from FogEnd / FogScaler and the band colours is the fog this data describes; the fifteen scalars
are the only new lighting inputs and their meaning needs the client binary (the beta executable is
packed, see doc 41), so they stay unmapped -- ranges recorded here for when it can be read.

## 21. "Issue with rendering trees" (Darkshire): not reproduced in the bench

User screenshot: Darkshire trees drawn as tall spikes with scattered leaf cards. Bench, build 43, Forever
project, camera (18300, 140, 27540) yaw 45 pitch 15 (pitch positive looks down), 150 and 600 frames:
every tree normal (`hz/forever_darkshire3_rgb.png`). The files: `duskwoodtree07` beta = MD21 v272, 263
vertices, 1 bone, bone flags 0, NO animation data (0 sequences in every track), all vertices weight 255 on
bone 0, bounding box identical to the Era v274 file (141 vertices); its single skin matches. So neither
the static geometry nor a bone track can stretch these trees; whatever stretched them in the user's
session is runtime state, and the bench needs that session's camera and time of day to chase it. From 520
units up the Duskwood fog swallows the ground entirely (uniform fog colour), which limits how far a
top-down bench camera can see there.

## 22. Shredded HD characters: M2SkinSection.Level is a high word (2026-09-16, eleventh pass)

User screenshot: a night elf NPC (purple skin, blue braids, `character/nightelf/male/nightelfmale_hd.m2`,
five loads in that session) with intact legs, a shredded torso and head, and a heap of body pieces at its
feet; the log right after the load says `Skipping invalid model render pass 0`. Data, skin 974391 (LOD 0):
41,543 vertices, 150,468 triangle indices, 130 sections -- 46 at Level 0 (the base body), 56 at Level 1 and
28 at Level 2 (the customization geosets 507..5103). wowdev .skin: "Level: (level << 16) is added to
startTriangle and alike to avoid having to increase those fields to uint32s". Test over the 84 leveled
sections: with the raw `indexStart`, 0 of their 83,655 triangle vertex references fall inside the
section's own vertex range; with `indexStart | (Level << 16)`, all 83,655 do, and the shifted ranges tile
the index list contiguously to exactly 150,468. `vertexStart` is NOT shifted in this file (the vertex list
is below 65,536, section 46 starts at vertex 20,951 raw). noggit's `initRenderPasses` copied the raw
start, so every customization geoset drew the body's first triangles with its own vertex window -- the
torso and head became a jumble of the legs' geometry, and the leftover pieces piled at the origin. The
same holds for `humanmale_hd.m2` (147,966 indices, 66 leveled sections); plain WotLK-era skins stay under
65,535 and carry Level 0. The bone-combo remap was checked on the way and is NOT involved: with the M2's
`bone_lookup_table` (header 0x78) it agrees with the vertex bone indices on 100% of vertices for the night
elf HD, the plain night elf and the earth elemental.

Fix, first attempt (build 46): `ModelRender::initRenderPasses` and the loader's per-section vertex loop
shift `istart` by `d2 << 16` when the skin's triangle list exceeds 65,535 entries, and `vstart` when its
vertex list does. The user's next screenshot (a human NPC: arms, hands and hair drawn, torso, legs, feet
and face missing, a heap at the feet) showed it had not taken. Chased through everything the file could
be wrong about -- bone matrices (215 bones, no degenerate determinant, no NaN, sane translations), bone
combos, sequence storage (the stand keys are in the M2 under flag 0x820), interpolation types, weights,
the geoset selection (0, 401, 501, 1301, 2001, 2201, 3202 all enabled), the bake (the HD bake
`creaturedisplayextra-02603_hd.blp` is a 512x256 opaque DXT1, the HD skin 1024x512), the compositor --
all clean. Differential runs on Barkeep Hann in the Darkshire inn: bake suppressed -> same defect; the
classic model (fidelity setting = Classic) -> whole. The missing parts were exactly the leveled
sections (2201 torso at shifted start 123,210; 2001 feet at 114,954; 1301 at 88,650), the drawn ones the
Level-0 sections. Cause: `ModelRenderPass::index_start / vertex_start / vertex_end` were `uint16_t`, so the
shifted 32-bit start was truncated straight back to the raw value on assignment and the draw call never
saw the fix. **Real fix (build 48): those fields are `uint32_t`.** The skipped pass 0 is batch 0 of the
same skin: 6 textures, shader 0x801C, on geoset 1507 -- drawn with its first two textures since build 47.

Verification (build 48, bench, Forever, Barkeep Hann, camera (18222, 30.5, 27583) yaw 180 pitch 8): the HD
human draws whole -- hair, face, vest, shirt, arms, belt, pants (`hz/forever_hann_fixed_zoom.png`); the
same camera on build 47 showed head and arms only (`hz/forever_hannN.png`). Bench camera convention, for
the record: yaw 0 looks along +z, 90 along +x, 180 along -z, 270 along -x; positive pitch looks down; NPC
positions come from tw_world `creature` as noggit x = 17066.67 - y, z = 17066.67 - x.

## 23. Live SD / HD character models: what the beta's data offers (2026-09-16, eleventh pass)

User: "we need the SD/HD toggle to be a toggle in setting that takes effect live. thats how the beta
client works, you can change the models live." What the tables hold for that switch:

- `CreatureDisplayInfo` (14,097 rows, decoded from the two plain BLTE frames; the two TACT frames hold
  48 + 12 bytes of unreleased rows): `ConditionalCreatureModelID` is 0 on every row. Player displays 49..52
  point at `humanmale.m2`, `humanfemale.m2`, `orcmale.m2`, `orcfemale.m2` (the plain files); 8,403 NPC
  displays sit on `_hd` files, 521 on plain ones.
- `ChrModel` has 18 rows = the 18 plain player models (layout 1, 1024x1024), `ChrRaceXChrModel` maps race
  and sex onto them; no HD ChrModel exists.
- `CreatureModelData` carries rows for both files of 18 pairs (`humanmale.m2` 49 / 14837 / 15894,
  `humanmale_hd.m2` 7661 / 16754, ...). Nothing in the tables links a pair; the file name does.
- `CreatureDisplayInfoExtra` (8,915 rows, 7 columns) has `BakeMaterialResourcesID` AND
  `HDBakeMaterialResourcesID`: 8,568 extras carry both bakes, 342 the HD bake only, 2 the classic only.

So the client's live option can only be: draw the display's model or its file-name sibling, with the
bake of the same fidelity. Implemented (build 47): the CreatureModelData adapter appends column 28
(`FidelitySibling`, the paired row or 0), the CreatureDisplayInfoExtra adapter appends column 21 (the HD
bake path); `World::choose_creature_model` / `creature_bake_for` pick the row and bake under Settings >
Paths > "Character models (modern clients)" = As authored / Classic (SD) / HD (`render/
character_model_fidelity`); `MapView::paintGL` polls the value every 30 frames and re-resolves the spawns
(`refreshCreatureSpawnOverlay(true)`) when it changes, so the switch is live for loaded spawns. Displays
without a sibling keep their authored model. The texture-variant box of section 18 stays a project-open
setting: CascLib resolves the root once, and the beta client's texture quality is likewise a restart
option, not the live one.

Also in this build: 12.x batches with more than two textures (the HD cloak geoset 1507: 6 textures, shader
0x801C Guild_Opaque) draw their first two instead of being skipped -- "Skipping invalid model render pass
0" fired on every HD NPC. Not addressed: 206 external sequences per HD character without an AFID entry
and without the alias flag (their variations of the in-file sequences), logged as "tracks left empty".

## 24. The beta's water shaders: where the reference is (2026-09-16, eleventh pass, open)

User: "new water is completly new water. we are still doing the 2d flat plane water with sliding
texture ... new water shader with new shaders in dx12 ... with tesselation and displacement." That is
right about noggit: sections 19's translation only recolours the WotLK plane. The PBR surface needs the
client's shader as the reference, and this is what the beta ships:

- Shader containers are `GXSH` files (`HSXG` magic, version 14): words at 0x10 = permutation entries,
  offset of a blob-offset table, blob count, data start; every blob is plain zlib (no encryption) holding
  `03 00 00 00 <size>` and then one or more `DXBC` programs. Target tag at 0x08: `05XD` = SM 5.0 DXBC
  (fxc `/dumpbin` disassembles it), `06XD` = DXIL (dxc), `11TM` = Metal. Extraction script in the
  session scratchpad (`modern/shaders*/`).
- The named `procwater` family (retail names, present locally): `shaders/vertex/dx_5_0/procwater.bls` = 3
  vs_5_0 programs of ~110 instructions, no tessellation; `pixel/dx_5_0/procwaterabove.bls` = 36 ps_5_0
  programs, the full ones 166 instructions with 6..12 declared textures and 2 samples, no `sincos`. This
  is the 8.x procedural water of the classic liquid rows (`ProceduralRiverDepthTex`), not the PBR one.
- The PBR water rows' files are unnamed in the community listfile, so the shaders were hunted through the
  root: 128 unnamed Windows-only files, 115 of them GXSH (54 SM5, 55 DXIL, 6 Metal), 141 SM5 programs
  disassembled: 21 pixel, 41 compute, 6 vertex, NO hull/domain in the SM5 set. Candidates in the same id
  range as the six `12_water` foam textures (7475833..7660147): fdid **7552026** (SM5 pixel, 9 textures,
  305 instructions), **7552031** (SM5 pixel, 7 textures, 192), **7552027** and **7552032** (DXIL) and
  **7674323** (DXIL). The 41 compute programs are where a wave simulation would live; tessellation and
  displacement, if present, sit in the DXIL containers (DX12 path), which dxc `/dumpbin` can read.
- The six foam textures are 1024x1024 uncompressed BGRA masks (white with alpha); there is no normal or
  height map among the row's textures, so displacement is procedural.

Next step: disassemble 7552026 / 7552031 (SM5) and the DXIL of 7552027 / 7552032 / 7674323, map their
constant buffers onto LiquidType Float[38] / Coefficient[4] / Color[3], and build the GL 4.3
tessellation + displacement water from that. Not started.

## 25. Model diagnostics switch (2026-09-17)

`NOGGIT_M2_DEBUG_BONES=<substring of a model path>` makes the headless bench log, at error level: every
bone's posed matrix once per model (`[BONE-DEBUG]`: parent, pivot, determinant, translation, NaN), the
render pass table (`[PASS-DEBUG]`: geoset, Level, ranges, textures, shader), the creature geoset selection
(`[GEOSET-DEBUG]`: visible ids, controlled families, the selection trace) and the resolved texture
overrides (`[TEXTURE-DEBUG]`). The log is written from several threads, so lines can interleave; match on
the tag and re-run when a line is torn. Cameras for the Darkshire and Blasted Lands NPCs used here are in
the bench folder (`vk_cam_hannN.txt`, `vk_cam_lynnore2.txt`).

## 26. HD characters: eyes and the death knight glow (2026-09-17)

User: "eyes are missing and instead got these blue flames coming out of them". Two facts in the HD human
file: geoset group 17 is the eye glow, and its `1701` is the death knight glow itself -- 60 additive
vertices textured by the hardcoded `character/human/male/deathknighteyegloweffect.blp` (TXID 3537040),
with `1702..1705` the other glow colours -- where the classic file's 1701 is an 8-vertex nothing. The
legacy default table shows xx01 for every family, so every HD NPC wore the glow. The real eyes are geoset
`3301`, textured by M2 texture type **19** (character eyes, Shadowlands+), which the customization chain
names for the display (`character/human/eyes00_12_3484655.blp`, ChrModelTextureLayer target 25) and
noggit never filled. Build 49: the eye-glow family is hidden on modern projects (the chain never carries a
type-17 geoset) and the chain's type-19 texture becomes the instance's texture-19 override.

## 27. HD blood elves in bind pose: SKID and the .skel file (2026-09-17)

User: "bloode elf models tpose". `character/bloodelf/female/bloodelffemale_hd.m2` (v274) has 0 bones and 0
sequences of its own; a `SKID` chunk names skeleton file 1838505 (`bloodelfmale_hd`: 1838675). The .skel
is a chunk stream -- SKL1 (16 B), SKS1 {4 global loops, 386 sequences, 683 lookups}, SKB1 {245 bones, 291
key-bone lookups; 22.8 MB because it holds every in-file track}, SKA1 {43 attachments, 75 lookups}, AFID
(54 entries), BFID (20) -- with every M2Array offset relative to its own chunk's data start (the stand's
272 tracks read plausibly that way; no SKPD parent). noggit read the empty header and drew the bind pose.
Build 49: `Model::graftSkeleton` appends the SKS1 / SKB1 / SKA1 payloads to the MD21 payload, rebases
every array (bones and attachments carry tracks of per-sequence arrays), points the header's global
loop, sequence, lookup, bone, key-bone and attachment arrays at them and merges the skeleton's AFID, so
the unchanged MD20 parser sees a complete model.

## 28. The "broken" SD blood elf is a texture layout, not bones (2026-09-17)

User: "their SD version is complely broken bones" (display 25879 under the Classic setting). Verified on
Bloodmage Lynnore (Blasted Lands, tw_world guid 2685) with the bone diagnostic: 109 of 119 posed bones
match an offline evaluation of the file within 0.02, the ten that differ are the spherical-billboard
bones (flag 0x8) noggit turns to the camera; the beta's re-exported classic file keeps 90 of 119 bones
parent-less with absolute tracks, and an offline skinning of its body stretches 1% of edges -- the pose
is sound. What the close capture shows is a robed body with the FACE across the torso: display 25879's
extra (17502) has `BakeMaterialResourcesID` 0 and only an HD bake (210817), the beta ships no classic body
skin for the race at all, and the SD file keeps the classic UV layout (the whole square, like the SD
human), so the Classic swap put a layout-2 bake on layout-1 UVs. The client keeps such displays on the
model their bake fits. Build 49: the swap only happens when the display's extra carries the bake of the
wanted fidelity (342 extras are HD-only, 2 classic-only).

## 29. Maps that crashed on open: the sky band fallback threw inside its catch (2026-09-17)

User: "some maps crash when trying to load like caverns of time or development land". The crash dump
(`noggit_crashdumps/noggit.exe.21860.dmp`, 01:20, parsed offline with dbghelp and the PDB) ends in
`DBCFile::NotFound` raised by `DBCFile::getByID` on `gLightIntBandDB` from `SkyParam::SkyParam`'s catch
handler `catch$12`; the bench reproduces it on map 269 and 451 with "there was an error with getting an
entry in LightIntBand DBC (0)" right before the throw. `SkyParam` reads 18 colour bands
(`paramId * 18 - 17 + i`) and 6 float bands; when one is missing its catch handler fell back to
`getByID(i)` -- band id `i` itself, which is 0 for the first band and never exists -- and a throw out of a
catch handler is `std::terminate`. WotLK data has no gaps, so that handler never ran before. The Forever
Beta ships 827 LightParams rows but LightData rows for 815 of them (5,375 rows), so a map whose Light row
names one of the 12 empty params has no synthesised bands. Build 51: both handlers fall back to param 1's
band (`1 + i`, the WotLK default light) only when it exists and otherwise leave the band empty.

Verification (build 51, isolated GL bench, 120 frames each): map 269 and 451 both reach the IMAGE line with
no exception; the per-band "error with getting an entry in LightIntBand DBC (n)" lines still print (24 per
empty param, 4 such params on 451) but are now informational. Cameras over real tiles (tile lists read from
the WDT MAIN chunks offline, WDT fdids 829736 / 857684): Caverns of Time tile 28/28 (`15200 130 15200`,
yaw 45, pitch 15; ground -37..181 from its root ADT 830633) draws terrain, trees and the map's own dense
fog (a camera 350 units up sees only fog -- the bench's FLAT check, not a defect); Development Land tile
33/45 (`17867 400 24267`) draws its flat height-0 ground. Captures `hz/forever_map269_tile2.png`,
`hz/forever_map451_tile.png`.

## 9. Verification (2026-09-16, build 19, OpenGL, isolated bench copy)

| Camera | Before (build 17: loader fixes only) | After (build 19: modern era) |
|---|---|---|
| Northshire, facing south (17216, 230, 25700) | canopies drawn as dark cube-like cards, black where the leaf texture is transparent | trees with keyed leaves, abbey and far tree line intact |
| Elwynn / Westfall border (15747, 129, 28007), the user's own pose | every tree a dark slab | proper trees; the far canopy line reads as a fogged silhouette |

Both runs: exit 0, 90 frames, 0 SEH / VEH exceptions, no new shader-uniform errors (the pre-existing
`alpha_key_classic does not exist` line stays: that uniform is unused by the shader since the doc 40 revert).
Captures: scratchpad `hz/era_fix_south.png` / `hz/era_mod_south.png`, `hz/era_fix_their3.png` /
`hz/era_mod_their3.png`.

What the change does NOT alter: WotLK-era files (v264) and classic files (v256/257) keep their code paths
untouched -- every modern branch is gated on `Model::renderEra() == Modern`, which only MD21 containers
report.
