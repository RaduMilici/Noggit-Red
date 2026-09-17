# 41 — Modern (CASC) client support: what the files look like and what noggit has to change

2026-09-15. Research only, no loader code written yet. Everything below was measured on the
user's shared modern install `F:\World of Warcraft` (products 1.15.9 Classic Era and 2.5.6
Anniversary), on files pulled out of it with `full_data/twmoa_toolkit/casc/cascread.py`, and on
noggit's own sources. Reference sources used where the wiki is unreachable (Cloudflare):
wowdev/WoWDBDefs (DB2 layouts), wowdev/DBCD (WDC5 reader), ladislav-zezula/CascLib (root
parser), Kruithne/wow.export (chunk dispatch), and the project's own 1.14 converters
(`full_data/wow1142_tools`, RE_notes 21/27/30/46/48 — they already WRITE every modern format
we now need to READ).

Extracted samples live in the session scratchpad (`scratchpad/modern/era`, `modern/anniv`);
`scratchpad/wdc5_probe.py` is the throw-away WDC5 decoder that validated section 3.

---

## 1. The store

`F:\World of Warcraft\.build.info` (one row per product, `Active=1` on all three):

| Product | Version | exe |
|---|---|---|
| `wow_classic_era` | 1.15.9.69722 | `_classic_era_\WowClassic.exe` |
| `wow_classic_era_ptr` | 2.5.6.69110 | `_classic_era_ptr_\WowClassicT.exe` |
| `wow_anniversary` | 2.5.6.69795 | `_anniversary_\WowClassic.exe` |

All three share ONE local archive set: `Data\data\data.000..007` (7.1 GB) + 16 bucket `.idx`
files (`Data\data\<bucket><ver>.idx`). `Data\config\xx\yy\<hash>` holds the build/CDN configs.
`Data\wow` and `Data\wow_classic` are leftovers of retired products (per-product `.idx` dirs,
no data files). `Data\indices\*.index` (1469 files) are CDN archive indices for streaming and
are not needed locally. The product code selects the row in `.build.info`, hence the build
config, hence the ENCODING and ROOT manifests; the data archives are common.

**Root manifest = TSFM version 2** (header `{magic, headerSize=24, version=2, total, named}`,
block header `{nFiles, localeFlags, contentFlags1, contentFlags2, contentFlags3:u8}` = 17 bytes,
then fdid deltas, then 16-byte ckeys, then 8-byte name hashes unless the no-name-hash flag).
Both products: 1.15.9 = 236,259 records / 159,749 fdids; 2.5.6 = 289,547 / 190,763.

**The CascLib noggit links cannot read it.** `cmake/FindCascLib.cmake` fetches a PREBUILT
`CascLibRAS.lib` from `gitlab.com/prophecy-rp/dependencies` (tag `dep-casclib`, header says
`CASCLIB_VERSION 2.1`, built from a 2022-era "CascLib-master"). Tested with a 40-line program
linked against exactly that binary (`scratchpad/casctest`):

```
product=wow_classic_era build=69722     <- product selection by szCodeName WORKS
features=0x5                             <- no CASC_FEATURE_FILE_DATA_IDS: root not parsed
fdid 1375580: OPEN FAILED err=2          <- every fdid open fails (ERROR_FILE_NOT_FOUND)
```

The same program linked against upstream master (`ladislav-zezula/CascLib` commit 2a280f5a,
2026-08-22, `CASCLIB_VERSION 3.0`, built static with `CASC_BUILD_STATIC_LIB=ON
CASC_BUILD_SHARED_LIB=OFF CASC_UNICODE=OFF`; its `cmake_minimum_required(VERSION 3.2)` needs
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` or a one-line patch):

```
product=wow_classic_era build=69722   features=0x1f6   total files=231640
fdid 1375580: size=154569 magic=WDC5   (dbfilesclient/lightdata.db2)
fdid 1308501: size=784    magic=WDC5   (dbfilesclient/lightskybox.db2)
fdid 778497 : size=313436 magic=REVM   (world/maps/azeroth/azeroth_35_20.adt)
fdid 775970 : size=814882 magic=REVM   (world/maps/azeroth/azeroth.wdl)
fdid 494582 : size=1840   magic=SKIN
product=wow_anniversary build=69795   features=0x1f6   total files=285064
fdid 1375580: size=195091 magic=WDC5   (same id, DIFFERENT file: product selection proven)
```

Sizes match the Python extraction byte for byte. So: **upgrade CascLib to upstream master,
built from source** — that is phase 0 and it is already proven.

noggit side (`src/external/blizzard-archive-library/src/CASCArchive.cpp`):
`args.szCodeName = "wow"` is hardcoded (that product is not installed here → open fails),
`args.szLocalPath = <project ClientPath>` must be the directory holding `.build.info`,
`dwLocaleMask = 0` (CascLib then uses the storage default; both products are enUS-only
installs: tags `enUS speech / enUS text`). Files are opened only by fdid
(`CascOpenFile(..., CASC_FILE_DATA_ID(id), 0, 3, ...)`); a path key goes through
`Listfile::getFileDataID`, fed from `<project>/listfile.csv` (`initFromCSV`, `id;path` lines).
The community listfile is at `full_data/twmoa_toolkit/casc/community-listfile.csv` (2,210,265
lines, 149 MB). `ClientData` derives the storage type from the version enum:
`_storage_type = (version >= ClientVersion::SL) ? CASC : MPQ` (`ClientData.cpp:35`) and
`validateLocale` refuses `Locale::AUTO` for CASC.

---

## 2. Files worth knowing about that we do NOT need

Per map, MPHD names `<map>_lgt.wdt` (lights), `_occ.wdt` (occlusion), `_fogs.wdt`,
`_mpv.wdt` (particulate volumes), `<map>.tex` (texture streaming) and the WDL. Only the WDL is
consumed by noggit (horizon). `_lod.adt` files are absent for every tile in both roots (MAID
slot 4 is 0 everywhere).

---

## 3. DB2 = WDC5

Every `dbfilesclient/*.db2` in both products is **WDC5** (`schemaVer=5`, schema string
`WOWSTATIC_1_15_9_68185` / `WOWSTATIC_2_5_6_68184`). Layout vs the WDC3 the library already
reads (`blizzard-database-library`, `readers/wdc3`):

```
WDC5 header = "WDC5" + u32 SchemaVersion + char[128] SchemaString   (132 extra bytes)
            + the unchanged WDC3 body: RecordsCount FieldsCount RecordSize StringTableSize
              TableHash LayoutHash MinIndex MaxIndex Locale Flags:u16 IdFieldIndex:u16
              TotalFieldsCount PackedDataOffset LookupColumnCount ColumnMetaSize
              CommonDataSize PalletDataSize SectionsCount
            + SectionHeader[SectionsCount]  (40 B: u64 tactKey, fileOffset, numRecords,
              stringTableSize, offsetRecordsEnd, indexDataSize, parentLookupSize,
              offsetMapIdCount, copyTableCount)
            + FieldMeta[Fields] (i16 bits, u16 offset) + ColumnMeta[Fields] (24 B)
            + pallet blobs (Pallet=3 / PalletArray=4 columns) + common blobs (Common=2)
            + WDC4/5 ONLY: for each section with tactKey != 0: i32 count + i32[count]
              encrypted ids
            + per section: records, string table, id list, copy table (dst,src u32 pairs),
              sparse entries, reference (parent) data {n, min, max, {id, index}[n]},
              sparse id list
Compression enum: 0 None, 1 Immediate(bitpacked), 2 Common, 3 Pallet, 4 PalletArray,
                  5 SignedImmediate.  String fields: offset relative to the field's own
                  position, string table follows the section's records.
```

Validated with `scratchpad/wdc5_probe.py` on every extracted table (row count == RecordsCount
+ copy entries, ids inside MinIndex..MaxIndex, LightSkybox names decode, LiquidObject id 5960
resolves — see section 5).

Tables noggit needs, with the layout hash the files carry (identical in 1.15.9 and 2.5.6, both
builds are listed in WoWDBDefs; the DBD dir noggit ships, `bin/Release/definitions`, is
9.2.0-era with 996 files and must be refreshed from the 1320-file WoWDBDefs checkout):

| Table | Layout | Fields / recsize | Notes |
|---|---|---|---|
| Light | 5F16BC84 | 5 / 40 | `GameCoords[3] GameFalloffStart GameFalloffEnd ContinentID<16> LightParamsID<u16>[8]` — same meaning as WotLK cols 1..7; 8 param slots (WotLK has 8 too) |
| LightParams | 51C96BAD | 17 / 28 | `OverrideCelestialSphere[3] OverrideSunPosition[3] $id HighlightSky<u8> LightSkyboxID<u16> CloudTypeID<u8> Glow WaterShallowAlpha WaterDeepAlpha OceanShallowAlpha OceanDeepAlpha Flags<8> SsaoSettingsID SunPolar SunAzimuth SunAttenuationStart SunAttenuationEnd`; `IdFieldIndex=2`, id is INLINE (flags 0x0) |
| LightData | 70C6CD80 | 46 / 73 | one row per (LightParamID, Time); LightParamID is the parent (reference data); 18 colours + FogEnd/FogScaler/CloudDensity/FogDensity/…; **replaces LightIntBand + LightFloatBand** |
| LightSkybox | 9D4956FF | 4 / 8 | `Name Flags<32> SkyboxFileDataID CelestialSkyboxFileDataID` — the skybox M2 is an fdid, not a path (1.15.9 has 9 rows, ids 1..6, 329, 655, 670) |
| Map | B47C6F05 | 23 / 36 | `Directory MapName_lang MapDescription0/1_lang PvpShort/Long_lang MapType InstanceType ExpansionID AreaTableID LoadingScreenID TimeOfDayOverride ParentMapID CosmeticParentMapID TimeOffset MinimapIconScale RaidOffset CorpseMapID MaxPlayers WindSettingsID ZmpFileDataID Field_1_15_4_56400_021 Flags[3]`; **no WdtFileDataID** → the WDT is opened by path `world/maps/<Directory>/<Directory>.wdt` through the listfile |
| AreaTable | 705C911D | 23 / 24 | `ZoneName AreaName_lang ContinentID ParentAreaID AreaBit … LightID is NOT in this layout (classic uses zone light polygons / Light.db2 only) … Flags[2] LiquidTypeID[4]` |
| LiquidType | D1ECEEC9 | 21 / 36 | `Name Texture[6] Flags SoundBank SoundID SpellID MaxDarkenDepth FogDarkenIntensity AmbDarkenIntensity DirDarkenIntensity LightID ParticleScale ParticleMovement ParticleTexSlots MaterialID MinimapStaticCol FrameCountTexture[6] Color[3] Float[38] Int[4] Coefficient[4]` — textures are paths with `%d` frame patterns, unchanged |
| LiquidObject | CB0D39E8 | 5 / 2 | `FlowDirection FlowSpeed LiquidTypeID<32> Fishable Reflection` — needed for MH2O (section 5) |
| LiquidMaterial | 98E5D7AA | 2 / 1 | `LVF Flags` — needed for MH2O |
| GroundEffectTexture | E77A386D | 4 / 3 | `Density Sound DoodadID<u16>[4] DoodadWeight<8>[4]`; 313 records + 1631 copy-table rows |
| GroundEffectDoodad | 8394ED59 | 4 / 4 | `ModelFileID Flags Animscale Pushscale` — the doodad is an fdid, no path |

Library gaps found by reading `readers/wdc3/*.cpp`:
- `LoadTableStructure` rejects anything but the `WDC3` magic; add WDC4/WDC5 (skip the 132-byte
  schema prefix, read the encrypted-id lists between the common blobs and the first section).
- The common-data loader loops over `CommonData.size()` (always 0) → Common-compressed columns
  always read the default. LightParams (common=592 B) and Map are affected.
- Array columns (`arrLength > 0`) are read and discarded (`//TODO`), and `LightParamsID[8]`,
  `Flags[3]`, `LiquidTypeID[4]` are arrays we need.
- Rows are returned as `std::string` values per column name (`BlizzardDatabaseRow`), which
  is why only 15 call sites use it; 156 call sites use the fixed-column `DBCFile` API
  (`gLightDB.getByID(id).getFloat(LightDB::PositionX)` etc.). Plan: keep the `DBCFile` API and
  synthesise WotLK-shaped in-memory tables from the DB2 rows (per-table adapters), so
  `Sky.cpp`, `World.cpp`, `MapChunk.cpp`, `liquid_layer.cpp`, the ground-effect code and the
  UI keep working unchanged. LightData → LightIntBand/LightFloatBand is the only non-trivial
  adapter (18 colour bands + fog/glow floats per param, keyed `paramId*18+band` like WotLK).

`Utils.hpp::readFileAsIMemStream` and `BlizzardDatabase::LoadTable` compose
`DBFilesClient\<Table>.dbc`; modern needs `.db2` (the listfile has `dbfilesclient/<table>.db2`).

---

## 4. WDT

`world/maps/azeroth/azeroth.wdt` (1.15.9 163,908 B; 2.5.6 294,988 B):

```
MVER 18
MPHD 32 B: flags=0x248 (0x8 doodad refs sorted by size, 0x40, 0x200 wdt_has_maid; big-alpha
           0x4 NOT set → 4-bit MCAL), then fdids lgt=1100798 occ=1101056 fogs=1668812
           mpv=2488184 tex=775969 wdl=775970 pd4=0
MAIN 4096 x {flags u32, pad u32}: flags 1 = tile present, 2 = no tile (687 / 3409 on Azeroth;
           expansion01: 0 / 1 — both encodings occur, treat "bit 1 set" as present)
MAID 4096 x 8 u32 = 32 B per tile: root, obj0, obj1, tex0, lod, mapTexture, mapTextureN,
           minimapTexture.  Azeroth tile 35,20 = (778497, 778498, 778499, 778500, 0, 0, 0, 204587)
           → 204587 = world/minimaps/azeroth/map35_20.blp.  Columns 4..6 are 0 on every tile.
MAI2 (2.5.6 only) same shape; only tiles 32,48 / 32,49 carry a root id (7771414 / 7792205 =
           unkmaps/world/maps/azeroth/azeroth_32_48_unk0.bin) and those ids are NOT in the
           local root → unknown WDT chunks must be skipped, never asserted on.
No MWMO/MODF (no global WMO on these maps; keep supporting them when MPHD flag 0x1 is set).
```

noggit today (`map_index.cpp:99-170`) asserts the MVER/MPHD/MAIN sequence and composes tile
names `World\Maps\<map>\<map>_<x>_<y>.adt` (`map_index.cpp:142, 446, 942, 1188`;
`World.cpp:2898-2999` for the WDT itself). Modern tiles must be opened by the MAID ids
(`FileKey(fdid)`), one `ClientFile` per part.

---

## 5. ADT: one tile = four files

Measured on `azeroth_32_48*.adt` (1.15.9 and 2.5.6 are byte-identical for this tile) and
`expansion01_31_30*.adt` (2.5.6):

```
root  (327,605 B)  MVER MHDR(64, all offsets 0 except mh2o) [MH2O] MCNK x256
      MCNK = 128-byte WotLK header + MCVT(580) + MCNR(448, true size) + MCSE(0)
      header: flags |= 0x18000 (two undocumented bits every Blizzard chunk carries, 0x1 = has
      MCSH); ofsHeight/Normal/Layer/Refs/Alpha/Shadow/MCCV/MCLV = 0; ofsSndEmitters = ofsLiquid
      = 1180 (= chunk payload size); areaid, holes, position[3] as in WotLK. MCCV / MCLV / MCBB
      / MCDD may appear on other tiles (none on this one).
tex0  (1,434,304 B) MVER MAMP(4, value 0) MDID(u32 fdid per texture) MHID(height-texture fdids,
      0 here) MCNK x256 — these MCNKs have NO 128-byte header: MCLY(16 B per layer, same
      flags: 0x100 use alpha, 0x200 compressed, effectId) + [MCSH(512)] + [MCAL] straight away
      MCLY[0]: tex=0 flags=0 ofsAlpha=0 effect=993 / MCLY[2]: tex=2 flags=0x100 ofsAlpha=2048
      MCAL = 2048 B per alpha layer (4-bit, MPHD 0x4 clear) — identical decoder to WotLK
obj0  (41,048 B)   MVER MDDF MODF MCNK x256 (no header): MCRD (u32 MDDF indices) + MCRW (u32
      MODF indices) — the split of WotLK's MCRF; nothing draws without them
      MDDF 36 B: nameId=FDID, uniqueId, pos[3] f, rot[3] f, scale u16, flags u16 at +0x22;
      flags seen: 0x40 (605) and 0x240 (180) — 0x40 = entry_is_filedata_id (MANDATORY)
      MODF 64 B: nameId=FDID, flags 0xC = 0x8 entry_is_filedata_id | 0x4 has_scale, scale
      u16 = 1024 (1.0), doodadSet, nameSet; e.g. 107243 = stormwind.wmo
      No MMDX/MMID/MWMO/MWID anywhere.
obj1  (54,032 B)   MVER MLFD MLDD MLDX MLDL MLMD MLMX — LOD doodads/WMOs; ignore (no MCNKs)
```

**MH2O differs in one field.** Header (12 B per chunk: ofsInstances, layerCount, ofsAttributes)
and instance (24 B) are the WotLK layout, but the second u16 is `liquid_object_or_lvf`:
values < 42 are the vertex format, values ≥ 42 are **LiquidObject ids**. Tile 32_48 chunk 13:
`liquidType=5, liquid_object_or_lvf=5960, minH=maxH=143.99, 8x8, ofsExists, ofsVertex`;
LiquidObject 5960 = `{FlowDirection 0, FlowSpeed 0, LiquidTypeID 5, Fishable 1, Reflection 0}`.
The vertex format then comes from `LiquidType.MaterialID → LiquidMaterial.LVF`.
**Column order matters**: the 1.15.9 / 2.5.6 layout (98E5D7AA) is `ID, Flags, LVF`, so
LiquidMaterial 1 = {Flags 1, LVF 0} (water: float height + u8 depth, 5 bytes per vertex in every
river layer of azeroth_30..33_47..50) and 2 = {Flags 0, LVF 1} (magma/slime: height + uv). An
earlier draft of this note had the two swapped.

**Ocean = LiquidObject 42, and the row does not exist.** Every ocean layer of the converted maps
carries `liquid_object_or_lvf = 42` (azeroth_29_48, Stormwind harbour: 187 layers, all
liquidType 2; 156 without vertex data, 81 with exactly 1 byte per vertex = depth only). The WotLK
build of the same tile encodes them as LVF 2 (101 + 92 layers), so the converter mapped WotLK
LVF 2 to the retail ocean LiquidObject 42 -- but LiquidObject.db2 of both 1.15.9 (904 rows) and
2.5.6 (655 rows) starts at id 5921, and neither `DBCache.bin` carries a hotfix for it. The rule the
client-derived reader (WoWViewerCpp `LiquidMaterialManager` / `LiquidDataGetters.h`) applies:
`liquidObjectId = value > 41 ? value : liquid_type`, the LiquidObject row is looked up with the
instance's liquid type as the fallback, `LiquidType 2 -> LVF 2` unconditionally, `id 42 -> ocean
object`, and `!offset_vertex_data && liquid_type != 2 -> LVF 2`. noggit's `ChunkWater.cpp` reads
`MH2O_Information.liquid_vertex_format` raw and `liquid_layer.cpp` clamps unknown liquid ids
through `resolve_liquid_id` — both need the LiquidObject step.

noggit today (`MapTile.cpp:345-520`) parses MVER→MHDR→MCIN→MTEX→MMDX→MMID→MWMO→MWID→MDDF→
MODF→MFBO from ONE file and `MapChunk.cpp`/`texture_set.cpp` read MCVT/MCNR/MCLY/MCAL/MCSH/
MCRF/MCCV/MCLV/MCSE through the MCNK header offsets. The split loader has to: open root by
fdid, read MCNK by walking sub-chunks (offsets are zero), open tex0 and pair its 256 headerless
MCNKs with the root's by index, resolve MDID → `FileKey(fdid)` textures, open obj0, resolve
MDDF/MODF fdids → `ModelInstance(FileKey(fdid))` / `WMOInstance(FileKey(fdid))`, and build
per-chunk ref lists from MCRD/MCRW. `blp_texture`, `scoped_model_reference`, `ModelInstance`
and `wmo_doodad_instance` already take a `FileKey` (`TextureManager.h:46`,
`ModelManager.h:34`, `ModelInstance.h:69/263`), so instances need no new plumbing.

---

## 6. WMO

Root `world/wmo/azeroth/buildings/human_farm/farm.wmo` (7,360 B, identical in both products):

```
MVER 17  MOHD(64: nTex=14 nGroups=2 nPortals=1 nLights=0 nDoodadNames=84 nDoodadDefs=139
nDoodadSets=6 ambColor wmoID=58 flags numLod)  MOMT(64 B x 14)  MOGN MOGI MOSB MOPV MOPT MOPR
MOVV MOVB MOLT MODS MODD(40 B x 132) MFOG MODI(u32 x 140) GFID(u32 x 2)
```

- **MOTX is gone**; `MOMT.texture_1/2/3` hold fdids (`130065 =
  dungeons/textures/walls/mm_strmwnd_wall_03.blp`), the 64-byte record layout is unchanged.
- **MODN is gone**; `MODI` is an fdid array and `MODD.nameIndex` (24-bit) is an INDEX into
  MODI, not a byte offset (`MODD[0].nameIndex=0 → MODI[0]=199563 = barrel01.m2`).
- **GFID** lists the group files (`106966, 106967` = `farm_000.wmo`, `farm_001.wmo`).
- `MOSB` (skybox name) is empty here; modern also has MOSI (skybox fdid) when set.
- Group files are unchanged: `MVER 17 MOGP(68-byte header, flags 0x809, flags2 0x317)` +
  `MOPY MOVI MOVT MONR MOTV MOBA MODR MOBN MOBR` (+ MLIQ/MOCV/MOLR when present).

noggit today (`WMO.cpp:50-380`) asserts the exact WotLK chunk order including MOTX and MODN,
opens the root by `_file_key.filepath()` (`WMO.cpp:36`) and groups by `<name>_%03d.wmo`
(`WMO.cpp:699`). Modern needs: chunk-by-name dispatch for the root, texture keys from MOMT
fdids, doodad keys from MODI, group keys from GFID.

---

## 7. M2

`creature/rabbit/rabbit.m2` (1.15.9: 176,754 B, 18 sequences all embedded; 2.5.6: 61,906 B,
7 sequences — different data, same format) and `elwynntreecanopy02.m2`:

```
MD21 chunk (payload = the MD20 header + data; ALL header offsets are relative to the payload
     start, i.e. file offset + 8) version 274, globalFlags 0x202081 / 0x202080
TXAC 2 B per (material + particle emitter)   LDV1 16 B   SFID u32 per skin profile (+ lod
skins)   TXID u32 per texture (0 = runtime-composed slot, type != 0)   AFID {animId u16,
subAnimId u16, fdid u32} for sequences without flag 0x20 (external .anim)   BFID bones
PFID physics  SKID skeleton  (+ EXP2 PABC PADC PSBC PEDC PGD1 RPID GPID … skip)
```

Header differences vs the v264 `ModelHeader` noggit parses (`ModelHeaders.h:22`): the 304-byte
header is identical (RE_notes/30 §M2InitAll); **M2Camera is 116 B** (fov became a track;
noggit's `ModelCameraDef` is 100 B), **M2ParticleEmitter is 492 B** (noggit's is 476 B),
texture entries carry EMPTY filenames (the name is TXID), `.skin` header is 56 B (`SKIN`,
5 array pairs, `boneCountMax`, `nShadowBatches`, `ofsShadowBatches`; rabbit has 1 shadow
batch; batch record 24 B unchanged with `shader_id=16`). Skin files are named by SFID, not by
`<model>00.skin` (`Model.cpp:1476`), and `Model::finishLoading` opens by
`_file_key.filepath()` (`Model.cpp:608`) and rejects anything that is not `MD20` at offset 0
(`Model.cpp:619`).

BLP2 is unchanged (`rabbitskin.blp`: BLP2 type 1, DXT, 128x128, mips) — the existing loader
works, only the KEY is an fdid.

---

## 8. Where noggit branches today

| Concern | Where | Now | Needed |
|---|---|---|---|
| project version | `ApplicationProject.h:38-72`, `ApplicationProjectReader.cpp:97-110`, `NoggitProjectCreationDialog.cpp:30`, `.ui:383` | strings "Vanilla"/"Turtle WoW", "Wrath Of The Lich King", "Shadowlands" → `ProjectVersion` | new entries (Classic Era 1.15, Anniversary 2.5 — one "modern CASC" family with a product code) |
| product | `CASCArchive.cpp` `szCodeName = "wow"` | hardcoded | from project json; creation dialog lists the `Product` column of `<ClientPath>\.build.info` |
| storage type | `ClientData.cpp:35/57` `version >= SL` | enum order decides | explicit `StorageType` from the project |
| locale | `ClientData::validateLocale` | AUTO refused for CASC; `ApplicationProject.h:289` sets enUS for SL | same for the new versions |
| listfile | `ClientData.cpp:269` `<project>/listfile.csv` | 2.2 M lines loaded into two maps | ship/point at the community listfile; consider filtering to `world/`, `dbfilesclient/`, `tileset/`, `creature/`, `character/`, `item/`, `spells/`, `environments/`, `interface/glues` |
| DB2 | `blizzard-database-library`, `DBCFile.cpp:32`, `Utils.hpp` | WDBC + WDC3, `.dbc` names, 9.2 DBDs | WDC5, `.db2` names, refreshed DBDs, adapters to `DBCFile` |
| WDT/ADT | `map_index.cpp`, `MapTile.cpp`, `MapChunk.cpp`, `texture_set.cpp`, `ChunkWater.cpp` | monolithic ADT by path | MAID + split files by fdid, MH2O LiquidObject |
| M2 | `Model.cpp` | MD20 at offset 0, path-derived skins/textures | MD21 walk, SFID/TXID/AFID, 116/492-byte structs |
| WMO | `WMO.cpp` | MOTX/MODN, path-derived groups | MOMT fdids, MODI, GFID |
| skybox | `Sky.cpp` `SkyParam` ctor | `LightSkyboxDB::filename` | `SkyboxFileDataID` |
| ground effects | `GroundEffectDoodad` path column | path | `ModelFileID` |

---

## 9. Implementation plan (loading first; saving is a separate phase)

0. **CascLib upgrade** (proven above): `cmake/FindCascLib.cmake` → FetchContent from
   `https://github.com/ladislav-zezula/CascLib.git`, build the static lib in-tree
   (`CASC_BUILD_STATIC_LIB=ON`, `CASC_BUILD_SHARED_LIB=OFF`, `CASC_UNICODE=OFF`,
   `CMAKE_POLICY_VERSION_MINIMUM=3.5`), keep `CASCLIB_NO_AUTO_LINK_LIBRARY`. Also drop the
   `remove_definitions(-D_DLL)` hack. The Linux/mac prebuilt paths go away with it.
1. **Project/product plumbing**: `ProjectVersion::{CLASSIC_ERA, ANNIVERSARY}` (+ generic
   `MODERN_CASC` handling), `project.json` `Client.Product`, creation dialog product combo
   read from `.build.info`, `CASCArchive` takes the product code, `ClientData` takes an explicit
   `StorageType`, locale forced enUS, listfile path setting in the Settings UI.
2. **DB2**: WDC4/WDC5 in `blizzard-database-library` (magic gate, schema prefix, encrypted-id
   lists, common-data fix, array columns), `.db2` file names, DBD refresh, and per-table
   adapters that materialise `DBCFile`-shaped tables (Light, LightParams, LightData→Int/Float
   bands, LightSkybox, Map, AreaTable, LiquidType, LiquidObject/LiquidMaterial, GroundEffect*)
   so the 156 `gXxxDB` call sites and the Sky/zone-light code stay untouched.
3. **WDT + split ADT**: MAID-driven tile keys, root/tex0/obj0 readers (obj1 ignored),
   MH2O LiquidObject → LVF, MDID/MHID textures, MDDF/MODF fdid instances, MCRD/MCRW refs,
   unknown chunks skipped by name.
4. **M2**: MD21 chunk walk with payload-relative offsets, version-conditional camera/particle
   structs, SFID skins, TXID textures (0 = composed), AFID external anims, TXAC ignored.
5. **WMO**: root chunk dispatch by name, MOMT fdid textures, MODI doodads, GFID groups, MOSI.
6. **Render check** on the harness: Elwynn tile 32_48 (both products) and expansion01 31_30
   against the client (apitrace on the modern client is a separate rig; first pass = does the
   terrain, water, farm WMO and trees appear where the 3.3.5a project puts them).
7. **Saving** (later): writing split ADTs, MAID, minted fdids + listfile rows for new files —
   the 1.14 converters (`convert_terrain_1142.py`, `m2_to_md21.py`, `convert_wmo_1142.py`)
   already encode every write-side rule (RE_notes/46/48, memory `twmoa-1142-*`).

Known limits of the two installed products: 1.15.9 has no Northrend (0 Light rows on map 571,
Map.db2 = 59 rows) and 2.5.6 is TBC (83 maps, Outland present). Both are enUS-only.

---

## 10. Implementation (2026-09-15, same day) — what was built, where

Phases 0-5 of the plan are implemented (loading only; saving to a modern client is still out).
All of it is staged, nothing committed. Where each piece lives:

| Piece | Files |
|---|---|
| CascLib from upstream source (3.0) | `cmake/FindCascLib.cmake` (the `cmake` submodule): FetchContent of `ladislav-zezula/CascLib` @ 2a280f5a, static/ANSI, no tests; `CascLib` stays an INTERFACE target wrapping `casc_static` |
| Product + listfile plumbing | `blizzard-archive-library`: `ClientVersion::CLASSIC_ERA/ANNIVERSARY` (>= SL = CASC), `ClientData(..., casc_product, listfile_path)`, `CASCArchive(path, cache, product, ...)` passes `szCodeName`, maps the locale mask, logs product/build/features and REFUSES a storage whose root gave no fileDataIDs; `ClientFile::setBuffer` rewinds so a synthesized stream is readable |
| Project model | `ProjectVersion::CLASSIC_ERA / ANNIVERSARY` ("Classic Era" / "Anniversary" in `.noggitproj`), `NoggitProject::ClientProduct` (json `Client.Product`), `isModernCascVersion()`, `usesSynthesizedDbc()`; `loadProject` resolves the storage root (`.build.info` up to 3 levels up), reads the product's `Version` from `.build.info` for the DBD build, forces enUS, picks the listfile (`<project>/listfile.csv` > Settings `casc/listfile_path` > app `ApplicationListFilePath`) — `noggit/casc/BuildInfo.*` |
| UI | project creation dialog: "Classic Era" / "Anniversary" versions + a **CASC product** combo filled from the client folder's `.build.info`; Settings > Paths: **CASC listfile** field (QSettings `casc/listfile_path`) |
| DB2 | `noggit/db2/WDC5File.*` (WDC3/4/5 reader: all compressions, id list, copy table, reference data, string offsets), `noggit/db2/ModernDBC.*` (column names from the DBD matched by LAYOUT hash then build; re-emits AreaTable, Map, Light, LightParams, LightSkybox(+SkyboxFileDataID column 3), LightIntBand/LightFloatBand from LightData, LiquidType, GroundEffectTexture, GroundEffectDoodad in the WotLK layouts; `liquidObjectVertexFormat()`); hook in `DBCFile::open` + `DBCFile::loadSynthesized`; `dist/definitions` refreshed from WoWDBDefs (996 -> 1320 files, covers 1.15.9.69722 / 2.5.6.69795) |
| Map list / wizard | `BuildMapListComponent`, `NoggitWindow::loadMap`, `MapCreationWizard` go through `gMapDB` for these projects (the DatabaseLib route cannot read WDC5); Map.db2 row editing is refused with a message |
| WDT / ADT | `MapIndex`: trailing WDT chunks by name (MWMO/MODF/MAID, unknown skipped), `MapTileEntry::file_ids`; `MapTile::setModernFiles` + `MapTileModern.cpp`: reads root/tex0/obj0 by id, MH2O/MFBO from the root, MDID -> listfile paths (or `fdid:N`), MDDF/MODF ids -> instances, then rebuilds ONE WotLK-shaped MCNK stream (offsets recomputed, high-res holes folded to the 4x4 mask, MCLY/MCSH/MCAL spliced from tex0) fed to the unchanged `MapChunk` ctor; `ChunkWater::fromFile` maps `liquid_object_or_lvf >= 42` through LiquidObject/LiquidType/LiquidMaterial |
| M2 | `Model::unwrapMD21` (chunk walk: MD21 payload rebased into the ClientFile, SFID/TXID/AFID kept), `modelSkinKey`, TXID textures (`fdid:N` when the listfile lacks the path), AFID .anim files, 492-byte v274 emitter stride, every path heuristic through `modelPath()` ("" for id-only keys) |
| WMO | `WMO::finishLoading` is a chunk-dispatch loop: MOTX optional (else MOMT texture fields are ids), MODD resolved after the loop through MODN or MODI, GFID group ids (`WMOGroup::load`), MOSI skybox id |
| Textures | `TextureManager`: `fdid:<n>` pseudo names open by fileDataID (`texture_file_key`), every cache stays string-keyed |
| Sky | `SkyParam` uses `LightSkyboxDB::skyboxFileDataID` when the synthesized table carries it |

Test project: `tools/noggit_dev/modern_era/ClassicEra.noggitproj` (ClientPath `F:/World of Warcraft`,
Product `wow_classic_era`, project-local copy of the community listfile). Harness cameras
`vk_cam_northshire*.txt` (tile 32_48, the Northshire valley).

### 10a. Bugs met on the first runs (all fixed, all staged)

| Symptom | Cause | Fix |
|---|---|---|
| `definition definitions\Map.dbd has no layouts`, empty map list, then a crash in the map list (`invalid map<K,T> key`) | (1) the WoWDBDefs checkout was CRLF: the DBD parser never sees an empty line, treats `LAYOUT ...` as a column, gives up. (2) Even with LF, `Map.dbd` has `int NavigationMaxDistance? // default/if <= 0: 999` -- the parser took the `<` in the COMMENT for a foreign key, the type substring spanned the line, invalid -> empty definition. (3) `BuildMapListComponent` unloaded a DatabaseLib table it never loaded on the gMapDB route | definitions converted to LF; `DatabaseDefinition.cpp` parses the declaration part only (`database-library` submodule); unload guarded |
| every M2 failed: `CascOpenFile(706753) failed, error 2 for '...elwynntreecanopy03.m2'` while the id in the listfile is 189929 | the community listfile also carries the legacy `.mdx` rows (own ids, 706xxx). `normalizeFilenameInternal` rewrites `.mdx` -> `.m2`, so the MDX row landed on the SAME map key and, coming later, overwrote the real id. Only models were hit (textures/WMOs have no legacy twin) | `Listfile::initFromCSV`: a real `.m2` row always wins, an `.mdx`/`.mdl` row only fills an unclaimed key. Independently, `AsyncObjectMultimap::emplace` now builds the object from the FULL key (`Model(FileKey)`), so ids out of MDDF/MODI/SFID never go through the listfile at all |
| heap corruption (0xC0000374) on longer runs, `Broken header in WMO MD_CRYPT_D_000` / `MOPY has 128 entries for 488 triangles` | modern group files insert `MOGX`, `MOBS`, `MFVR`, `MDAL` and replace `MOPY` by `MPY2` ({u16 flags, u16 material}); the fixed-order group parser read MOGX as MOPY and MPY2 bytes as MOVI indices | `WMOGroup::load` is a chunk-dispatch loop (MPY2 folded into the MOPY table, unknown chunks skipped, second MOTV/MOCV by occurrence + header flags) |
| `Async load failed ... could not be loaded` with no reason | the `FileReadFailedError` catch never logged the exception text | `AsyncLoader` logs the failing FILE; `CASCArchive::openFile` logs the first 64 unresolvable paths / failed ids with the CascLib error |
| once models loaded: `bad allocation` in random loads a few seconds in, then 0xC0000374 (heap corruption) | a sequence whose keys are external (no flag 0x20) but whose `.anim` is not available carries .anim-RELATIVE track offsets; `Animated.h` read them out of the M2 unchecked -> garbage counts -> multi-GB `push_back` loops, and `ClientFile::read` with a pointer past the end underflowed its byte count -> giant memcpy | inline sub-arrays are bounds-checked like external ones (empty track instead), `ClientFile::read` clamps, MD21 models log how many external sequences had no loadable AFID entry |
| `texture_coord_combo_index out of range` for every MD21 model | modern M2s ship an EMPTY texture-unit lookup (`tex_unit_lookup_table` is "unused" since Cata); the t1/t2 fallback is the right answer | warning gated on `!isModernMD21()` |
| ground-effect doodads `world/nodxt/detail/world/nodxt/detail/x.m2` | WotLK `Doodadpath` is a bare file name that `MapChunk::detailDoodads` prefixes | the adapter strips the prefix |
| SIGSEGV in `ParticleSystem::update` (user's GUI session froze then crashed a few seconds after the map opened) | the 492-byte emitter record (476 + 16 bytes of `multiTextureParam0/1`) was keyed on global flag 0x200, but Classic Era models carry that flag CLEAR on 492-byte records (generaltorch01, azr_tree01, bfd_walllight03); reading at stride 476 produced garbage track headers (thousands of keys, out-of-range global-sequence ids) and `Animated::getValue` dereferenced a null `_globalSequences[id]` | `Model::emitter_tracks_sane()` picks 492 or 476 from the sanity of the FIRST record's track headers instead of the flag; `Animated.h` drops a global-sequence id that has no table behind it |
| a fog-coloured, screen-aligned rectangle covering a quadrant / half of the OpenGL view at most camera angles around Northshire (Vulkan was clean, the replayed apitrace frame was not) | apitrace of one GL frame: the block is painted by ONE `glDrawArraysInstanced(count=384, instancecount=187)` -- the water batch of azeroth_29_48 (Stormwind harbour). 43 of its 187 chunk-layers had NaN / 1e38 heights in the vertex-data texture. The harbour's ocean layers reference LiquidObject 42, absent from the Classic tables, so the adapter returned LVF 0 and `liquid_layer` read 81 floats out of an 81-byte depth-only array (section 5) | `liquidObjectVertexFormat(id, liquid_type)`: missing row -> the layer's own liquid type; LiquidType 2 -> LVF 2; no vertex data -> LVF 2; LiquidMaterial columns read by name from the `ID, Flags, LVF` layout |
| loader-thread access violation `noggit.exe+0x21745E (read of address 0x0)` while loading `metalcup04`, `westfalllamppost02`, `stormwindmageportal01` (crash-guarded in the harness, a hard crash in the GUI) | `ModelRender::fixShaderIDLayer` indexed `_texture_unit_lookup[...]` unguarded; MD21 models ship that lookup EMPTY (vector data pointer = null) | out-of-range entries read as -1, so the WotLK layer-merge state machine stays inert for such a model (its skin `shader_id` is authored) |
