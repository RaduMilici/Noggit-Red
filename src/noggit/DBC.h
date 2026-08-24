// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/DBCFile.h>

#include <string>

class AreaDB : public DBCFile
{
public:
  AreaDB() :
    DBCFile("DBFilesClient\\AreaTable.dbc")
  { }

  /// Fields
  static const size_t AreaID = 0;    // uint
  static const size_t Continent = 1;  // uint
  static const size_t Region = 2;    // uint [AreaID]
  static const size_t AreaBit = 3;    // uint 
  static const size_t Flags = 4;    // bit field
  static const size_t SoundProviderPreferences = 5;    // uint 
  static const size_t UnderwaterSoundProviderPreferences = 6;    // uint 
  static const size_t SoundAmbience = 7;    // uint 
  static const size_t ZoneMusic = 8;    // uint 
  static const size_t ZoneIntroMusicTable = 9;    // uint 
  static const size_t ExplorationLevel = 10;    // int
  static const size_t Name = 11;    // localisation string
  static const size_t FactionGroup = 28;    // uint
  static const size_t LiquidType = 29;    // uint[4]
  static const size_t MinElevation = 33;    // float
  static const size_t AmbientMultiplier = 34;    // float
  static const size_t LightId = 35;    // int

  static std::string getAreaName(int pAreaID);
  static std::uint32_t get_area_parent(int area_id);
  static std::uint32_t get_new_areabit();
};

class MapDB : public DBCFile
{
public:
  MapDB() :
    DBCFile("DBFilesClient\\Map.dbc")
  { }

  /// Fields
  static const size_t MapID = 0;        // uint
  static const size_t InternalName = 1;    // string
  static const size_t AreaType = 2;      // uint
  static const size_t IsBattleground = 4;    // uint
  static const size_t Name = 5;        // loc

  static const size_t LoadingScreen = 57;    // uint [LoadingScreen]
  // Minutes 0..1439, or -1 = none. CLIENT-CANON (wow335a.exe FUN_004f8410, ".\WorldFrame.cpp"):
  // when != -1 the client FORCES the day-night clock to this minute on the whole map, every frame
  // ("Invalid time of day override in Map ID %d. TOD = %d." when out of range). Stock uses it on
  // OrgrimmarArena (720); Ascension also on Uldum(1080)/Azzarfaire(1260)/nerubianarena(240)/etc.
  static const size_t TimeOfDayOverride = 62; // int, minutes, -1 = none
  static std::string getMapName(int pMapID);
  static int findMapName(const std::string& map_name);
};

class LoadingScreensDB : public DBCFile
{
public:
  LoadingScreensDB() :
    DBCFile("DBFilesClient\\LoadingScreens.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t Name = 1;      // string
  static const size_t Path = 2;      // string
};

class LightDB : public DBCFile
{
public:
  LightDB() :
    DBCFile("DBFilesClient\\Light.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t Map = 1;      // uint
  static const size_t PositionX = 2;    // float
  static const size_t PositionY = 3;    // float
  static const size_t PositionZ = 4;    // float
  static const size_t RadiusInner = 5;  // float
  static const size_t RadiusOuter = 6;  // float
  static const size_t DataIDs = 7;    // uint[8]
};

class LightParamsDB : public DBCFile{
public:
  LightParamsDB() :
    DBCFile("DBFilesClient\\LightParams.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t highlightSky = 1;// bool
  static const size_t skybox = 2;      // uint ref to LightSkyBox
  static const size_t cloudTypeID = 3; // uint
  static const size_t glow = 4;        // uint
  static const size_t water_shallow_alpha = 5;
  static const size_t water_deep_alpha = 6;
  static const size_t ocean_shallow_alpha = 7;
  static const size_t ocean_deep_alpha = 8;
  static const size_t flags = 9;
};

class LightSkyboxDB : public DBCFile
{
public:
  LightSkyboxDB() :
    DBCFile("DBFilesClient\\LightSkybox.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t filename = 1;    // string
  static const size_t flags = 2;      // uint
};

class LightIntBandDB : public DBCFile
{
public:
  LightIntBandDB() :
    DBCFile("DBFilesClient\\LightIntBand.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t Entries = 1;    // uint
  static const size_t Times = 2;      // uint
  static const size_t Values = 18;    // uint
};

class LightFloatBandDB : public DBCFile
{
public:
  LightFloatBandDB() :
    DBCFile("DBFilesClient\\LightFloatBand.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t Entries = 1;    // uint
  static const size_t Times = 2;      // uint
  static const size_t Values = 18;    // float
};

class GroundEffectTextureDB : public DBCFile
{
public:
  GroundEffectTextureDB() :
    DBCFile("DBFilesClient\\GroundEffectTexture.dbc")
  { }

  /// VERSION-GATED. The record grew from 7 columns in 1.12 to 11 in 3.3.5a, and everything after the
  /// doodad slots moved:
  ///   1.12   : ID(0) Doodads[4](1-4) Amount(5) TerrainType(6)
  ///   3.3.5a : ID(0) Doodads[4](1-4) Weights[4](5-8) Amount(9) TerrainType(10)
  /// So on WotLK data the 1.12 indices read Weights[0] as the density -- which is 1 on effectively
  /// every row, against authored densities of 1..123 (mode 8). That is the "grass far too sparse".
  /// Proven from the shipped DBCs: of the 734 effect rows present in BOTH datasets, 3.3.5a field 9
  /// equals the 1.12 Amount on 94.3%, field 10 on only 9%.
  static const size_t ID = 0;         // uint
  static const size_t Doodads = 1;    // uint[4] (0 / 0xFFFFFFFF = empty slot) -- same index both versions
  static size_t Weights();            // uint[4], WotLK only (returns 0 on 1.12 -- no such column)
  static size_t Amount();             // uint (density; how many to scatter per subcell)
  static size_t TerrainType();        // uint
};

class GroundEffectDoodadDB : public DBCFile
{
public:
  GroundEffectDoodadDB() :
    DBCFile("DBFilesClient\\GroundEffectDoodad.dbc")
  { }

  /// VERSION-GATED. Both versions have 3 columns but the last two are SWAPPED:
  ///   1.12   : ID(0) Flags(1)    Filename(2)
  ///   3.3.5a : ID(0) Filename(1) Flags(2)
  /// Verified against the shipped DBCs. In 1.12, field 1 is a sequential index that happens to land
  /// mid-string when read as an offset ("lwFlo01.mdl", "wFlo01.mdl") -- the truncated garbage that
  /// originally hid the grass. In 3.3.5a it is the other way round: field 1 holds the real offsets
  /// (1, 14, 27, 40 -> "ElwFlo01.mdl", "ElwFlo02.mdl", ...) and field 2 is 0 on every row. Reading
  /// the 1.12 index on WotLK data therefore resolves EVERY doodad to the empty string, so nothing
  /// loads at all -- that is the "no grass".
  static const size_t ID = 0;         // uint
  static size_t Filename();           // string  (world\nodxt\detail\*.mdl)
  static size_t Flags();              // uint
};

class LiquidTypeDB : public DBCFile
{
public:
  LiquidTypeDB() :
    DBCFile("DBFilesClient\\LiquidType.dbc")
  { }

  /// Fields
  static const size_t ID = 0;        // uint
  static const size_t Name = 1;      // string
  static const size_t Type = 3;      // uint
  static const size_t ShaderType = 14;  // uint
  static const size_t TextureFilenames = 15;    // string[6]
  static const size_t TextureTilesPerBlock = 23;  // uint
  static const size_t Rotation = 24;  // uint
  static const size_t AnimationX = 23;  // uint
  static const size_t AnimationY = 24;  // uint

  static int getLiquidType(int pID);
  static std::string getLiquidName(int pID);
};

class SoundProviderPreferencesDB : public DBCFile
{
public:
    SoundProviderPreferencesDB() :
        DBCFile("DBFilesClient\\SoundProviderPreferences.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t Description = 1;    // string
};

class SoundAmbienceDB : public DBCFile
{
public:
    SoundAmbienceDB() :
        DBCFile("DBFilesClient\\SoundAmbience.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t SoundEntry_day = 1;        // uint
    static const size_t SoundEntry_night = 2;        // uint
};

class ZoneMusicDB : public DBCFile
{
public:
    ZoneMusicDB() :
        DBCFile("DBFilesClient\\ZoneMusic.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t Name = 1;    // string
    static const size_t SilenceIntervalMinDay = 2;        // uint
    static const size_t SilenceIntervalMinNight = 3;        // uint
    static const size_t SilenceIntervalMaxDay = 4;        // uint
    static const size_t SilenceIntervalMaxNight = 5;        // uint
    static const size_t DayMusic = 6;        // uint [soundEntries]
    static const size_t NightMusic = 7;        // uint [soundEntries]
};

class ZoneIntroMusicTableDB : public DBCFile
{
public:
    ZoneIntroMusicTableDB() :
        DBCFile("DBFilesClient\\ZoneIntroMusicTable.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t Name = 1;    // string
    static const size_t SoundId = 2;        // uint
    static const size_t Priority = 3;        // uint
    static const size_t MinDelayMinutes = 4;        // uint
};

class SoundEntriesDB : public DBCFile
{
public:
    SoundEntriesDB() :
        DBCFile("DBFilesClient\\SoundEntries.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t SoundType = 1;        // uint
    static const size_t Name = 2;    // string
    static const size_t Filenames = 3;        // string[10]
    static const size_t Freq = 13;        // uint[10)
    static const size_t FilePath = 23;        // string[10)
    static const size_t Volume = 24;        // float
    static const size_t Flags = 25;        // int
    static const size_t minDistance = 26;        // float
    static const size_t distanceCutoff = 27;        // float
    static const size_t EAXDef = 28;        // int
    static const size_t soundEntriesAdvancedID = 29;        // int
};

  class CreatureDisplayInfoDB : public DBCFile
  {
  public:
    CreatureDisplayInfoDB() :
      DBCFile("DBFilesClient\\CreatureDisplayInfo.dbc")
    { }

    static const size_t ID = 0;
    static const size_t ModelID = 1;
    static const size_t ExtendedDisplayInfoID = 3;
    // WotLK-only column (16-field layout): ParticleColor.dbc row id for recolored creature variants.
    // Returns 0 on classic layouts (no such column) -- gate on the value like CapeDisplayID().
    static size_t ParticleColorID();
    static const size_t CreatureModelScale = 4;
    static const size_t CreatureModelAlpha = 5;
    static const size_t TextureVariation1 = 6;
    static const size_t TextureVariation2 = 7;
    static const size_t TextureVariation3 = 8;
  };

  class CreatureDisplayInfoExtraDB : public DBCFile
  {
  public:
    CreatureDisplayInfoExtraDB() :
      DBCFile("DBFilesClient\\CreatureDisplayInfoExtra.dbc")
    { }

    static const size_t ID = 0;
    static const size_t DisplayRaceID = 1;
    static const size_t DisplaySexID = 2;
    static const size_t SkinID = 3;
    static const size_t FaceID = 4;
    static const size_t HairStyleID = 5;
    static const size_t HairColorID = 6;
    static const size_t FacialHairID = 7;
    static const size_t HeadDisplayID = 8;
    static const size_t ShouldersDisplayID = 9;
    static const size_t ShirtDisplayID = 10;
    static const size_t ChestDisplayID = 11;
    static const size_t BeltDisplayID = 12;
    static const size_t LegsDisplayID = 13;
    static const size_t BootsDisplayID = 14;
    static const size_t BracersDisplayID = 15;
    static const size_t GlovesDisplayID = 16;
    static const size_t TabardDisplayID = 17;
    // WotLK ONLY: the 11th NPCItemDisplay slot noted below, holding the CAPE. Returns 0 on Classic,
    // meaning "no such column" -- 0 is never a real slot because field 0 is the record ID, so
    // callers just test it. Verified against the shipped DBCs: every non-zero value in 3.3.5a
    // field 18 is a valid ItemDisplayInfo id (1280 rows) and the items it points at are Cape_*
    // (Cape_BloodKnight_A_01, Cape_Mage_A_01Black, Cape_Plate_PVPAlliance_Plate_A_01Gold). In
    // Turtle the same field parses as a string on 9161 of 9177 rows, i.e. it is BakeName there.
    static size_t CapeDisplayID();
    // BakeName (string) = baked composite NPC texture. WotLK inserted 2 fields before it (an 11th
    // NPCItemDisplay/cape slot + Flags), so it sits at column 18 in the 19-field Vanilla/Classic DBC
    // but column 20 in the 21-field WotLK DBC. Verified empirically: the SAME NPC (ID 23/36) has
    // identical item slots 8..17 in both, with the BakeName string at 18 (Turtle) vs 20 (WotLK).
    // Reading the wrong column in WotLK returns a numeric 0 -> empty baked texture. Resolved per the
    // loaded project's client version (Turtle byte-identical). Defined in DBC.cpp.
    static size_t BakedTexture();
  };

  class CreatureModelDataDB : public DBCFile
  {
  public:
    CreatureModelDataDB() :
      DBCFile("DBFilesClient\\CreatureModelData.dbc")
    { }

    static const size_t ID = 0;
    static const size_t Flags = 1;
    static const size_t ModelName = 2;
    static const size_t SizeClass = 3;
    static const size_t ModelScale = 4;
  };

  class ItemDisplayInfoDB : public DBCFile
  {
  public:
    ItemDisplayInfoDB() :
      DBCFile("DBFilesClient\\ItemDisplayInfo.dbc")
    { }

    // ID + model/texture names sit BEFORE the layout divergence, so they are stable constants.
    static const size_t ID = 0;
    static const size_t ModelName1 = 1;
    static const size_t ModelName2 = 2;
    static const size_t ModelTexture1 = 3;
    static const size_t ModelTexture2 = 4;
    // WotLK ItemDisplayInfo.dbc has 25 fields vs Vanilla/Classic's 23: it inserts a 2nd inventory-icon
    // column at index 6, shifting EVERY column from the geoset groups onward by +1 (empirically verified
    // -- item 15676 HelmetGeosetVis 248,306 sits at 12,13 in Turtle vs 13,14 in WotLK). Reading the fixed
    // 1.12 indices on a WotLK client mis-read equipment geosets + helmet/hair hiding. Version-gated in
    // DBC.cpp like CreatureDisplayInfoExtra::BakedTexture / CharacterSections.
    static size_t GeosetGroup1();
    static size_t GeosetGroup2();
    static size_t GeosetGroup3();
    static size_t HelmetGeosetVis1();
    static size_t HelmetGeosetVis2();
    static size_t TextureUpperArm();
    static size_t TextureLowerArm();
    static size_t TextureHands();
    static size_t TextureUpperChest();
    static size_t TextureLowerChest();
    static size_t TextureUpperLeg();
    static size_t TextureLowerLeg();
    static size_t TextureFoot();
  };

  class CharacterFacialHairStylesDB : public DBCFile
  {
  public:
    CharacterFacialHairStylesDB() :
      DBCFile("DBFilesClient\\CharacterFacialHairStyles.dbc")
    { }

    static const size_t RaceID = 0;
    static const size_t SexID = 1;
    static const size_t VariationID = 2;
    static const size_t BeardGeoset = 3;
    static const size_t MoustacheGeoset = 4;
    static const size_t SideburnGeoset = 5;
  };

  class CharacterHairGeosetsDB : public DBCFile
  {
  public:
    CharacterHairGeosetsDB() :
      DBCFile("DBFilesClient\\CharHairGeosets.dbc")
    { }

    static const size_t ID = 0;
    static const size_t RaceID = 1;
    static const size_t SexID = 2;
    static const size_t VariationID = 3;
    static const size_t GeosetID = 4;
    static const size_t ShowsScalp = 5;
  };

  // HelmetGeosetVisData.dbc: a head item's ItemDisplayInfo.HelmetGeosetVis[sex] indexes this table, whose
  // per-geoset-group masks say WHICH of the wearer's geosets a helm hides. field[1] (HairFlags) is the hair
  // mask: 0 = KEEP hair (bandanas/circlets/open helms that only cover the face), non-zero = HIDE hair (full
  // helms). Empirically verified against the stock rows: the Defias bandana (row 247) has HairFlags=0 (keeps
  // hair) but non-zero facial masks (covers the mouth). 1.12 = 6 fields (ID + 5 masks), WotLK = 8 (2 extra
  // masks appended), so the hair/facial column indices are version-stable.
  class HelmetGeosetVisDataDB : public DBCFile
  {
  public:
    HelmetGeosetVisDataDB() :
      DBCFile("DBFilesClient\\HelmetGeosetVisData.dbc")
    { }

    static const size_t ID = 0;
    static const size_t HairFlags = 1;
    static const size_t Facial1Flags = 2;
    static const size_t Facial2Flags = 3;
    static const size_t Facial3Flags = 4;
  };

  // WotLK-only (absent from 1.12 data; open() no-ops there and the record count stays 0).
  // Row = id + 3 colour-sets x {start, mid, end} packed 0xRRGGBB ints; an emitter authored with
  // particleColorIndex 11/12/13 takes set 1/2/3, selected per creature by
  // CreatureDisplayInfo.particleColorID (checklist 12.10).
  class ParticleColorDB : public DBCFile
  {
  public:
    ParticleColorDB() :
      DBCFile("DBFilesClient\\ParticleColor.dbc")
    { }

    static const size_t ID = 0;
    static const size_t Start1 = 1;
    static const size_t Mid1 = 2;
    static const size_t End1 = 3;
    static const size_t Start2 = 4;
    static const size_t Mid2 = 5;
    static const size_t End2 = 6;
    static const size_t Start3 = 7;
    static const size_t Mid3 = 8;
    static const size_t End3 = 9;
  };

  class CharacterSectionsDB : public DBCFile
  {
  public:
    CharacterSectionsDB() :
      DBCFile("DBFilesClient\\CharSections.dbc")
    { }

    // Columns that DON'T move between client versions.
    static const size_t ID = 0;
    static const size_t RaceID = 1;
    static const size_t SexID = 2;
    static const size_t BaseSection = 3;

    // CharSections.dbc was REORDERED between Vanilla/Classic and WotLK: WotLK moved the Type
    // (VariationIndex) and Color (ColorIndex) columns to AFTER the three texture names + flags.
    // The record has the same field COUNT, so a header scan can't tell them apart -- hardcoding one
    // order makes the other client read body textures from the wrong columns. Verified empirically
    // against the stock DBCs (the skin ColorIndex increments 0,1,2 in lockstep with the skin texture
    // changing Skin00_00 -> 00_01 -> 00_02 in BOTH files):
    //   CLASSIC/Turtle: Var=4 Color=5 Tex=6,7,8 Flags=9
    //   WotLK:          Tex=4,5,6 Flags=7 Var=8 Color=9
    // These resolve to the correct physical column for the CURRENTLY loaded project's client version,
    // so a CLASSIC (Turtle/Vanilla) project reads the exact same columns as before while WotLK
    // projects read the right ones. (Defined in DBC.cpp so this header stays project-include-free.)
    static size_t VariationIndex();
    static size_t ColorIndex();
    static size_t TextureName1();
    static size_t TextureName2();
    static size_t TextureName3();
    static size_t Flags();
  };

class WMOAreaTableDB : public DBCFile
{
public:
    WMOAreaTableDB() :
        DBCFile("DBFilesClient\\WMOAreaTable.dbc")
    { }

    /// Fields
    static const size_t ID = 0;    // uint
    static const size_t WmoId = 1;  // uint
    static const size_t NameSetId = 2;    // uint [AreaID]
    static const size_t WMOGroupID= 3;    // uint 
    static const size_t SoundProviderPreferences = 4;    // uint 
    static const size_t UnderwaterSoundProviderPreferences = 5;    // uint 
    static const size_t SoundAmbience = 6;    // uint 
    static const size_t ZoneMusic = 7;    // uint 
    static const size_t ZoneIntroMusicTable = 8;    // uint 
    static const size_t Flags = 9;    // int CWorldMap::QueryOutdoors: rec.flags & 4 || rec.flags & 2. &0x18: Minimap::s_singleExterior = true unless groupRec::flags & 0x20
    static const size_t AreaTableRefId = 10;    // uint
    static const size_t Name = 11;    // localisation string

    static std::string getWMOAreaName(int WMOId, int namesetId);
    static std::vector<std::string> getWMOAreaNames(int WMOId);
};

class GameObjectDisplayInfoDB : public DBCFile
{
public:
    GameObjectDisplayInfoDB() :
        DBCFile("DBFilesClient\\GameObjectDisplayInfo.dbc")
    { }

    /// Fields
    static const size_t ID = 0;        // uint
    static const size_t ModelName = 1;        // string
    static const size_t Sounds = 2;    // int[10]
    static const size_t GeoBoxMinX = 12;        // float
    static const size_t GeoBoxMinY = 13;        // float
    static const size_t GeoBoxMinZ = 14;        // float
    static const size_t GeoBoxMaxX = 15;        // float
    static const size_t GeoBoxMaxY = 16;        // float
    static const size_t GeoBoxMaxZ = 17;        // float
    static const size_t ObjectEffectPackageID = 18;        // int
};

// ---- Spell visual DBCs (creature aura visuals) --------------------------------------------------
// Used to resolve a creature's permanent auras (creature_template.auras) to the effect models the
// client attaches while the aura is active: Spell -> SpellVisual -> state kit -> head/chest/base
// effect -> SpellVisualEffectName model path. Field indices below are the vanilla(1.12) layout; the
// first five SpellVisual fields and the kit/effect fields used here are identical in 3.3.5.
class SpellVisualDB : public DBCFile
{
public:
  SpellVisualDB() :
    DBCFile("DBFilesClient\\SpellVisual.dbc")
  { }

  static const size_t ID = 0;
  static const size_t PrecastKit = 1;
  static const size_t CastKit = 2;
  static const size_t ImpactKit = 3;
  static const size_t StateKit = 4;
};

class SpellVisualKitDB : public DBCFile
{
public:
  SpellVisualKitDB() :
    DBCFile("DBFilesClient\\SpellVisualKit.dbc")
  { }

  static const size_t ID = 0;
  static const size_t StartAnimID = 1;
  static const size_t AnimID = 2;
  static const size_t HeadEffect = 3;
  static const size_t ChestEffect = 4;
  static const size_t BaseEffect = 5;
  // Char procs: model-wide effects applied while the kit's aura is active. Layout verified
  // empirically across kit 989 (Ghost Visual: proc 1 = tint, param ARGB 0x4B0CB9FD + proc 14 =
  // transparency, param 0.5f), kit 312 (Stealth: proc 14 = 0.3f) and kit 3450 (proc 14 = 0.5f):
  // the proc TYPE ids sit at fields 15..18 and their params (raw u32 -- reinterpret as float or
  // ARGB depending on type) at fields 19..22. 0xFFFFFFFF/0 = unused slot.
  static const size_t CharProc0 = 15;
  static const size_t CharParam0 = 19;
  static const size_t CharProcCount = 4;
  static const size_t CharProcTransparency = 14;
  static const size_t CharProcTint = 1;
};

class SpellVisualEffectNameDB : public DBCFile
{
public:
  SpellVisualEffectNameDB() :
    DBCFile("DBFilesClient\\SpellVisualEffectName.dbc")
  { }

  static const size_t ID = 0;
  static const size_t Name = 1;
  static const size_t FileName = 2;
};

// Spell icon paths for the creature-info UI (icon id -> Interface\Icons\... path).
class SpellIconDB : public DBCFile
{
public:
  SpellIconDB() :
    DBCFile("DBFilesClient\\SpellIcon.dbc")
  { }

  static const size_t ID = 0;
  static const size_t TextureFilename = 1;
};

// Spell.dbc -- the CLIENT's spell data, used when the world DB has no spell_template table.
// Turtle/vmangos ship spell_template, but the 3.3.5a cores (CMaNGOS/AzerothCore) do not: they read the
// client DBC instead, so creature-info aura tooltips there had nothing to resolve and fell back to a bare
// "Spell ID N (no spell_template row)".
//
// Field indices differ between the 1.12 (173 field) and 3.3.5a (234 field) layouts -- vanilla carries extra
// EffectBaseDice/DicePerLevel blocks that WotLK dropped, which shifts the whole effect region. Both sets were
// located EMPIRICALLY against known values (spell 1784 Stealth: icon 250, visual 184, durationIndex 21,
// basePoints -1/4/-51, auras 36/16/33) and cross-checked (field 117/133 resolves to a valid SpellIcon.dbc id
// for 100% of sampled rows). Pick the set with layout() -- do NOT assume one layout.
class SpellDB : public DBCFile
{
public:
  SpellDB() :
    DBCFile("DBFilesClient\\Spell.dbc")
  { }

  struct Layout
  {
    size_t casting_time_index;
    size_t proc_chance;
    size_t proc_charges;
    size_t duration_index;
    size_t power_type;
    size_t mana_cost;
    size_t range_index;
    size_t stack_amount;
    size_t effect_die_sides;      // [3]
    size_t effect_base_points;    // [3]
    size_t effect_radius_index;   // [3]
    size_t effect_apply_aura;     // [3]
    size_t effect_amplitude;      // [3]
    size_t effect_multiple_value; // [3]
    size_t effect_chain_target;   // [3]
    size_t spell_visual;
    size_t icon_id;
    size_t name;
    size_t description;
  };

  static constexpr Layout Vanilla // 1.12, 173 fields
  {
    18, 25, 26, 30, 31, 32, 36, 39,
    64, 76, 88, 91, 94, 97, 100,
    115, 117, 120, 138
  };

  static constexpr Layout WotLK // 3.3.5a, 234 fields
  {
    28, 35, 36, 40, 41, 42, 46, 49,
    74, 80, 92, 95, 98, 101, 104,
    131, 133, 136, 170
  };

  static const size_t ID = 0;

  // Chosen by field count: anything at or beyond the WotLK width uses the WotLK offsets.
  Layout const& layout() { return getFieldCount() >= 200 ? WotLK : Vanilla; }
};

// Spell durations for resolving the $d macro in spell descriptions (creature-info tooltips).
class SpellDurationDB : public DBCFile
{
public:
  SpellDurationDB() :
    DBCFile("DBFilesClient\\SpellDuration.dbc")
  { }

  static const size_t ID = 0;
  static const size_t Duration = 1; // base duration in milliseconds
};

// Spell effect radii for the $a macro (yards).
class SpellRadiusDB : public DBCFile
{
public:
  SpellRadiusDB() :
    DBCFile("DBFilesClient\\SpellRadius.dbc")
  { }

  static const size_t ID = 0;
  static const size_t Radius = 1; // float, yards
};

// Spell range ("30 yd range") for the Blizzard-style tooltip header.
class SpellRangeDB : public DBCFile
{
public:
  SpellRangeDB() :
    DBCFile("DBFilesClient\\SpellRange.dbc")
  { }

  static const size_t ID = 0;
  static const size_t MinRange = 1; // float
  static const size_t MaxRange = 2; // float
};

// Spell cast time ("1.5 sec cast" / "Instant") for the Blizzard-style tooltip header.
class SpellCastTimesDB : public DBCFile
{
public:
  SpellCastTimesDB() :
    DBCFile("DBFilesClient\\SpellCastTimes.dbc")
  { }

  static const size_t ID = 0;
  static const size_t CastTime = 1; // milliseconds
};

// Faction resolution for the creature-info UI: creature_template.faction -> FactionTemplate ->
// Faction name + Alliance/Horde reaction (from the friendly/hostile masks).
class FactionTemplateDB : public DBCFile
{
public:
  FactionTemplateDB() :
    DBCFile("DBFilesClient\\FactionTemplate.dbc")
  { }

  static const size_t ID = 0;
  static const size_t Faction = 1;
  static const size_t Flags = 2;
  static const size_t OurMask = 3;
  static const size_t FriendlyMask = 4;
  static const size_t HostileMask = 5;
};

class FactionDB : public DBCFile
{
public:
  FactionDB() :
    DBCFile("DBFilesClient\\Faction.dbc")
  { }

  static const size_t ID = 0;
  static const size_t Team = 18;
  static const size_t Name = 19; // localized
};

void OpenDBs(std::shared_ptr<BlizzardArchive::ClientData> clientData);

const char * getGroundEffectDoodad(unsigned int effectID, int DoodadNum);

extern AreaDB gAreaDB;
extern MapDB gMapDB;
extern LoadingScreensDB gLoadingScreensDB;
extern LightDB gLightDB;
extern LightParamsDB gLightParamsDB;
extern LightSkyboxDB gLightSkyboxDB;
extern LightIntBandDB gLightIntBandDB;
extern LightFloatBandDB gLightFloatBandDB;
extern GroundEffectDoodadDB gGroundEffectDoodadDB;
extern GroundEffectTextureDB gGroundEffectTextureDB;
extern LiquidTypeDB gLiquidTypeDB;
extern SoundProviderPreferencesDB gSoundProviderPreferencesDB;
extern SoundAmbienceDB gSoundAmbienceDB;
extern ZoneMusicDB gZoneMusicDB;
extern ZoneIntroMusicTableDB gZoneIntroMusicTableDB;
extern SoundEntriesDB gSoundEntriesDB;
extern CreatureDisplayInfoDB gCreatureDisplayInfoDB;
extern CreatureDisplayInfoExtraDB gCreatureDisplayInfoExtraDB;
extern CreatureModelDataDB gCreatureModelDataDB;
extern ItemDisplayInfoDB gItemDisplayInfoDB;
extern CharacterFacialHairStylesDB gCharacterFacialHairStylesDB;
extern CharacterHairGeosetsDB gCharacterHairGeosetsDB;
extern CharacterSectionsDB gCharacterSectionsDB;
extern HelmetGeosetVisDataDB gHelmetGeosetVisDataDB;
extern ParticleColorDB gParticleColorDB;
extern WMOAreaTableDB gWMOAreaTableDB;
extern GameObjectDisplayInfoDB gGameObjectDisplayInfoDB;
extern SpellVisualDB gSpellVisualDB;
extern SpellVisualKitDB gSpellVisualKitDB;
extern SpellVisualEffectNameDB gSpellVisualEffectNameDB;
extern SpellIconDB gSpellIconDB;
extern FactionTemplateDB gFactionTemplateDB;
extern FactionDB gFactionDB;
extern SpellDB gSpellDB; // opened LAZILY by lookupSpellDbcInfo(), NOT by OpenDBs()

// Spell data read from the CLIENT's Spell.dbc, for world DBs that have no spell_template table (the 3.3.5a
// cores read the DBC instead). Deliberately free of any MySQL dependency so both the creature-info panel
// and World's aura-visual resolution can use it.
struct SpellDbcInfo
{
  std::uint32_t entry = 0;
  std::uint32_t spell_visual = 0;
  std::uint32_t icon_id = 0;
  std::string name;
  std::string description;
  std::int32_t effect_base_points[3] = { 0, 0, 0 };
  std::int32_t effect_die_sides[3] = { 0, 0, 0 };
  std::int32_t effect_amplitude[3] = { 0, 0, 0 };
  std::int32_t effect_chain_target[3] = { 0, 0, 0 };
  std::int32_t effect_radius_index[3] = { 0, 0, 0 };
  float effect_multiple_value[3] = { 0.0f, 0.0f, 0.0f };
  std::uint32_t duration_index = 0;
  std::uint32_t stack_amount = 0;
  std::uint32_t proc_charges = 0;
  std::uint32_t proc_chance = 0;
  std::uint32_t mana_cost = 0;
  std::uint32_t power_type = 0;
  std::uint32_t range_index = 0;
  std::uint32_t casting_time_index = 0;
};

// Look one spell up in Spell.dbc, opening the DBC on FIRST USE (it is ~46 MB at 3.3.5a's ~50k rows, so
// OpenDBs() deliberately skips it). Returns false when the DBC is unavailable or has no such row.
bool lookupSpellDbcInfo(std::uint32_t spell_id, SpellDbcInfo& out);
extern SpellDurationDB gSpellDurationDB;
extern SpellRadiusDB gSpellRadiusDB;
extern SpellRangeDB gSpellRangeDB;
extern SpellCastTimesDB gSpellCastTimesDB;
