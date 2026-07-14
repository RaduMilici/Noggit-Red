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

  /// Fields (1.12 layout, VERIFIED: 7 columns, NO separate weight array -- repeated doodad ids in
  /// the 4 slots are the weighting. Amount/density at field 5, sound/terrain-type at 6.)
  static const size_t ID = 0;         // uint
  static const size_t Doodads = 1;    // uint[4] (0 / 0xFFFFFFFF = empty slot)
  static const size_t Amount = 5;     // uint (density; how many to scatter per subcell)
  static const size_t TerrainType = 6; // uint
};

class GroundEffectDoodadDB : public DBCFile
{
public:
  GroundEffectDoodadDB() :
    DBCFile("DBFilesClient\\GroundEffectDoodad.dbc")
  { }

  /// Fields (1.12 layout, VERIFIED: field 1 is a sequential index, the FILENAME string is field 2.
  /// Reading field 1 as the string gave truncated garbage -> grass never loaded.)
  static const size_t ID = 0;         // uint
  static const size_t Filename = 2;   // string  (world\nodxt\detail\*.mdl)
  static const size_t Flags = 1;      // uint
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

    static const size_t ID = 0;
    static const size_t ModelName1 = 1;
    static const size_t ModelName2 = 2;
    static const size_t ModelTexture1 = 3;
    static const size_t ModelTexture2 = 4;
    static const size_t GeosetGroup1 = 6;
    static const size_t GeosetGroup2 = 7;
    static const size_t GeosetGroup3 = 8;
    static const size_t HelmetGeosetVis1 = 12;
    static const size_t HelmetGeosetVis2 = 13;
    static const size_t TextureUpperArm = 14;
    static const size_t TextureLowerArm = 15;
    static const size_t TextureHands = 16;
    static const size_t TextureUpperChest = 17;
    static const size_t TextureLowerChest = 18;
    static const size_t TextureUpperLeg = 19;
    static const size_t TextureLowerLeg = 20;
    static const size_t TextureFoot = 21;
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
extern WMOAreaTableDB gWMOAreaTableDB;
extern GameObjectDisplayInfoDB gGameObjectDisplayInfoDB;
extern SpellVisualDB gSpellVisualDB;
extern SpellVisualKitDB gSpellVisualKitDB;
extern SpellVisualEffectNameDB gSpellVisualEffectNameDB;
extern SpellIconDB gSpellIconDB;
extern FactionTemplateDB gFactionTemplateDB;
extern FactionDB gFactionDB;
extern SpellDurationDB gSpellDurationDB;
extern SpellRadiusDB gSpellRadiusDB;
extern SpellRangeDB gSpellRangeDB;
extern SpellCastTimesDB gSpellCastTimesDB;
