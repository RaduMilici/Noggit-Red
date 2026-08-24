// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/Misc.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/application/NoggitApplication.hpp> // clientDataShared() for the lazy Spell.dbc open
#include <blizzard-archive-library/include/ClientData.hpp>
#include <string>

namespace
{
  std::string classicMapNameFromField(MapDB::Record const& rec, std::size_t field)
  {
    if (field > 0)
    {
      auto shifted_preferred = std::string(rec.getLocalizedString(field - 1, 0));
      if (!shifted_preferred.empty())
        return shifted_preferred;

      auto shifted_fallback = std::string(rec.getLocalizedString(field - 1));
      if (!shifted_fallback.empty())
        return shifted_fallback;
    }

    auto preferred = std::string(rec.getLocalizedString(field, 0));
    if (!preferred.empty())
      return preferred;

    return std::string(rec.getLocalizedString(field));
  }
}

AreaDB gAreaDB;
MapDB gMapDB;
LoadingScreensDB gLoadingScreensDB;
LightDB gLightDB;
LightParamsDB gLightParamsDB;
LightSkyboxDB gLightSkyboxDB;
LightIntBandDB gLightIntBandDB;
LightFloatBandDB gLightFloatBandDB;
GroundEffectDoodadDB gGroundEffectDoodadDB;
GroundEffectTextureDB gGroundEffectTextureDB;
LiquidTypeDB gLiquidTypeDB;
SoundProviderPreferencesDB gSoundProviderPreferencesDB;
SoundAmbienceDB gSoundAmbienceDB;
ZoneMusicDB gZoneMusicDB;
ZoneIntroMusicTableDB gZoneIntroMusicTableDB;
SoundEntriesDB gSoundEntriesDB;
CreatureDisplayInfoDB gCreatureDisplayInfoDB;
CreatureDisplayInfoExtraDB gCreatureDisplayInfoExtraDB;
CreatureModelDataDB gCreatureModelDataDB;
ItemDisplayInfoDB gItemDisplayInfoDB;
CharacterFacialHairStylesDB gCharacterFacialHairStylesDB;
CharacterHairGeosetsDB gCharacterHairGeosetsDB;
HelmetGeosetVisDataDB gHelmetGeosetVisDataDB;
ParticleColorDB gParticleColorDB;
CharacterSectionsDB gCharacterSectionsDB;
WMOAreaTableDB gWMOAreaTableDB;
GameObjectDisplayInfoDB gGameObjectDisplayInfoDB;
SpellVisualDB gSpellVisualDB;
SpellVisualKitDB gSpellVisualKitDB;
SpellVisualEffectNameDB gSpellVisualEffectNameDB;
SpellIconDB gSpellIconDB;
FactionTemplateDB gFactionTemplateDB;
FactionDB gFactionDB;
// NOT opened in OpenDBs(): Spell.dbc is large (3.3.5a ships ~50k rows x 936 B = ~46 MB) and only the
// creature-info panel needs it, so it is opened on first use there instead. See CreatureInfoPanel.cpp.
SpellDB gSpellDB;
SpellDurationDB gSpellDurationDB;
SpellRadiusDB gSpellRadiusDB;
SpellRangeDB gSpellRangeDB;
SpellCastTimesDB gSpellCastTimesDB;

namespace
{
  // Several DBCs were re-laid-out between Vanilla/Classic (1.12) and WotLK (3.3.5a). CLASSIC (Turtle/
  // Vanilla) keeps the old layout; every other supported client uses the WotLK layout. Guarded on
  // CurrentProject so a null/unloaded project falls back to the CLASSIC (unchanged) layout. Shared by
  // every version-gated column accessor below.
  bool wotlkDbcLayout()
  {
    auto const* project = Noggit::Project::CurrentProject::get();
    return project && project->projectVersion != Noggit::Project::ProjectVersion::CLASSIC;
  }
}

// CreatureDisplayInfoExtra.dbc BakeName column: 20 in WotLK (21-field), 18 in Vanilla/Classic
// (19-field) -- see DBC.h. Item slots 8..17 didn't move, so only this string column needs gating.
size_t CreatureDisplayInfoExtraDB::BakedTexture() { return wotlkDbcLayout() ? 20 : 18; }
// The cape slot WotLK added at 18; Classic has no such column (18 is BakeName there). See DBC.h.
size_t CreatureDisplayInfoExtraDB::CapeDisplayID() { return wotlkDbcLayout() ? 18 : 0; }
size_t CreatureDisplayInfoDB::ParticleColorID() { return wotlkDbcLayout() ? 12 : 0; }

// GroundEffectTexture.dbc grew a 4-column weight array in WotLK (7 columns -> 11), pushing the
// density and terrain-type columns back by 4. Reading the 1.12 index on WotLK data lands on
// Weights[0], which is 1 almost everywhere -- grass came out ~8x too sparse. See DBC.h.
size_t GroundEffectTextureDB::Weights()     { return wotlkDbcLayout() ? 5 : 0; }
size_t GroundEffectTextureDB::Amount()      { return wotlkDbcLayout() ? 9 : 5; }
size_t GroundEffectTextureDB::TerrainType() { return wotlkDbcLayout() ? 10 : 6; }

// GroundEffectDoodad.dbc keeps 3 columns but swaps the last two between versions, so the 1.12 index
// resolves every WotLK doodad to the empty string and no detail model loads. See DBC.h.
size_t GroundEffectDoodadDB::Filename() { return wotlkDbcLayout() ? 1 : 2; }
size_t GroundEffectDoodadDB::Flags()    { return wotlkDbcLayout() ? 2 : 1; }

// CharSections.dbc reordered Type (VariationIndex) / Color (ColorIndex) behind the texture names + flags
// in WotLK (see DBC.h for the empirically-verified layouts).
size_t CharacterSectionsDB::VariationIndex() { return wotlkDbcLayout() ? 8 : 4; }
size_t CharacterSectionsDB::ColorIndex()     { return wotlkDbcLayout() ? 9 : 5; }
size_t CharacterSectionsDB::TextureName1()   { return wotlkDbcLayout() ? 4 : 6; }
size_t CharacterSectionsDB::TextureName2()   { return wotlkDbcLayout() ? 5 : 7; }
size_t CharacterSectionsDB::TextureName3()   { return wotlkDbcLayout() ? 6 : 8; }
size_t CharacterSectionsDB::Flags()          { return wotlkDbcLayout() ? 7 : 9; }

// ItemDisplayInfo.dbc: WotLK inserts a 2nd inventory-icon column at index 6, so every column from the
// geoset groups onward is +1 vs the 1.12 layout (see DBC.h). Columns 0..5 (ID/model/texture names) are
// unchanged and stay as constants.
size_t ItemDisplayInfoDB::GeosetGroup1()      { return wotlkDbcLayout() ? 7  : 6; }
size_t ItemDisplayInfoDB::GeosetGroup2()      { return wotlkDbcLayout() ? 8  : 7; }
size_t ItemDisplayInfoDB::GeosetGroup3()      { return wotlkDbcLayout() ? 9  : 8; }
size_t ItemDisplayInfoDB::HelmetGeosetVis1()  { return wotlkDbcLayout() ? 13 : 12; }
size_t ItemDisplayInfoDB::HelmetGeosetVis2()  { return wotlkDbcLayout() ? 14 : 13; }
size_t ItemDisplayInfoDB::TextureUpperArm()   { return wotlkDbcLayout() ? 15 : 14; }
size_t ItemDisplayInfoDB::TextureLowerArm()   { return wotlkDbcLayout() ? 16 : 15; }
size_t ItemDisplayInfoDB::TextureHands()      { return wotlkDbcLayout() ? 17 : 16; }
size_t ItemDisplayInfoDB::TextureUpperChest() { return wotlkDbcLayout() ? 18 : 17; }
size_t ItemDisplayInfoDB::TextureLowerChest() { return wotlkDbcLayout() ? 19 : 18; }
size_t ItemDisplayInfoDB::TextureUpperLeg()   { return wotlkDbcLayout() ? 20 : 19; }
size_t ItemDisplayInfoDB::TextureLowerLeg()   { return wotlkDbcLayout() ? 21 : 20; }
size_t ItemDisplayInfoDB::TextureFoot()       { return wotlkDbcLayout() ? 22 : 21; }

void OpenDBs(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  gAreaDB.open(clientData);
  gMapDB.open(clientData);
  gLoadingScreensDB.open(clientData);
  gLightDB.open(clientData);
  gLightParamsDB.open(clientData);
  gLightSkyboxDB.open(clientData);
  gLightIntBandDB.open(clientData);
  gLightFloatBandDB.open(clientData);
  gGroundEffectDoodadDB.open(clientData);
  gGroundEffectTextureDB.open(clientData);
  gLiquidTypeDB.open(clientData);
  gSoundProviderPreferencesDB.open(clientData);
  gSoundAmbienceDB.open(clientData);
  gZoneMusicDB.open(clientData);
  gZoneIntroMusicTableDB.open(clientData);
  gSoundEntriesDB.open(clientData);
  gCreatureDisplayInfoDB.open(clientData);
  try
  {
    gCreatureDisplayInfoExtraDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open CreatureDisplayInfoExtra.dbc: " << e.what() << std::endl;
  }
  gCreatureModelDataDB.open(clientData);
  try
  {
    gItemDisplayInfoDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open ItemDisplayInfo.dbc: " << e.what() << std::endl;
  }
  try
  {
    gCharacterFacialHairStylesDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open CharacterFacialHairStyles.dbc: " << e.what() << std::endl;
  }
  try
  {
    gCharacterHairGeosetsDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open CharacterHairGeosets.dbc: " << e.what() << std::endl;
  }
  try
  {
    gHelmetGeosetVisDataDB.open(clientData);
    gParticleColorDB.open(clientData); // wotlk-only file; logs + stays empty on 1.12 data
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open HelmetGeosetVisData.dbc: " << e.what() << std::endl;
  }
  try
  {
    gCharacterSectionsDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open CharSections.dbc: " << e.what() << std::endl;
  }
  gWMOAreaTableDB.open(clientData);
  try
  {
    gGameObjectDisplayInfoDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open GameObjectDisplayInfo.dbc: " << e.what() << std::endl;
  }
  try
  {
    gSpellVisualDB.open(clientData);
    gSpellVisualKitDB.open(clientData);
    gSpellVisualEffectNameDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open spell visual DBCs (creature aura visuals disabled): " << e.what() << std::endl;
  }
  try
  {
    gSpellIconDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open SpellIcon.dbc: " << e.what() << std::endl;
  }
  try
  {
    gFactionTemplateDB.open(clientData);
    gFactionDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open Faction/FactionTemplate.dbc: " << e.what() << std::endl;
  }
  try
  {
    gSpellDurationDB.open(clientData);
    gSpellRadiusDB.open(clientData);
    gSpellRangeDB.open(clientData);
    gSpellCastTimesDB.open(clientData);
  }
  catch (std::exception const& e)
  {
    LogError << "Failed to open SpellDuration/SpellRadius/SpellRange/SpellCastTimes.dbc: " << e.what() << std::endl;
  }
}



bool lookupSpellDbcInfo(std::uint32_t spell_id, SpellDbcInfo& out)
{
  if (!spell_id)
  {
    return false;
  }

  // Opened on FIRST USE: Spell.dbc is ~46 MB at 3.3.5a's ~50k rows and is only needed by projects whose
  // world DB lacks spell_template, so OpenDBs() skips it. One attempt only -- a failure is not retried.
  static bool attempted = false;
  static bool usable = false;
  if (!attempted)
  {
    attempted = true;
    try
    {
      gSpellDB.open(Noggit::Application::NoggitApplication::instance()->clientDataShared());
      usable = gSpellDB.getRecordCount() > 0;
      LogDebug << "Spell.dbc opened for spell fallback: " << gSpellDB.getRecordCount()
               << " rows, " << gSpellDB.getFieldCount() << " fields" << std::endl;
    }
    catch (std::exception const& e)
    {
      LogError << "Spell.dbc unavailable (spell names/visuals fall back to raw ids): " << e.what() << std::endl;
    }
  }

  if (!usable)
  {
    return false;
  }

  try
  {
    auto record = gSpellDB.getByID(spell_id);
    auto const& fields = gSpellDB.layout();

    out = SpellDbcInfo{};
    out.entry = spell_id;
    out.name = record.getString(fields.name);
    out.description = record.getString(fields.description);
    out.icon_id = record.getUInt(fields.icon_id);
    out.spell_visual = record.getUInt(fields.spell_visual);
    out.mana_cost = record.getUInt(fields.mana_cost);
    out.power_type = record.getUInt(fields.power_type);
    out.range_index = record.getUInt(fields.range_index);
    out.casting_time_index = record.getUInt(fields.casting_time_index);
    out.duration_index = record.getUInt(fields.duration_index);
    out.proc_chance = record.getUInt(fields.proc_chance);
    out.proc_charges = record.getUInt(fields.proc_charges);
    out.stack_amount = record.getUInt(fields.stack_amount);

    for (size_t i = 0; i < 3; ++i)
    {
      out.effect_base_points[i] = record.getInt(fields.effect_base_points + i);
      out.effect_die_sides[i] = record.getInt(fields.effect_die_sides + i);
      out.effect_amplitude[i] = record.getInt(fields.effect_amplitude + i);
      out.effect_chain_target[i] = record.getInt(fields.effect_chain_target + i);
      out.effect_radius_index[i] = record.getInt(fields.effect_radius_index + i);
      out.effect_multiple_value[i] = record.getFloat(fields.effect_multiple_value + i);
    }

    return true;
  }
  catch (DBCFile::NotFound const&)
  {
    return false;
  }
}

std::string AreaDB::getAreaName(int pAreaID)
{
  if (!pAreaID || pAreaID == -1)
  {
    return "Unknown location";
  }    

  unsigned int regionID = 0;
  std::string areaName = "";
  try
  {
    AreaDB::Record rec = gAreaDB.getByID(pAreaID);
    areaName = rec.getLocalizedString(AreaDB::Name);
    regionID = rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    areaName = "Unknown location";
  }
  if (regionID != 0)
  {
    try
    {
      AreaDB::Record rec = gAreaDB.getByID(regionID);
      areaName = std::string(rec.getLocalizedString(AreaDB::Name)) + std::string(": ") + areaName;
    }
    catch (AreaDB::NotFound)
    {
      areaName = "Unknown location";
    }
  }

  return areaName;
}

std::uint32_t AreaDB::get_area_parent(int area_id)
{
  // todo: differentiate between no parent and error ?
  if (!area_id || area_id == -1)
  {
    return 0;
  }

  try
  {
    AreaDB::Record rec = gAreaDB.getByID(area_id);
    return rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    return 0;
  }
}

std::uint32_t AreaDB::get_new_areabit()
{
    unsigned int areabit = 0;

    for (Iterator i = gAreaDB.begin(); i != gAreaDB.end(); ++i)
    {
        areabit = std::max(i->getUInt(AreaDB::AreaBit), areabit);
    }

    return static_cast<int>(++areabit);
}

std::string MapDB::getMapName(int pMapID)
{
  if (pMapID<0) return "Unknown map";
  std::string mapName = "";
  try
  {
    MapDB::Record rec = gMapDB.getByID(pMapID);
    mapName = classicMapNameFromField(rec, MapDB::Name);
    if (mapName.empty())
    {
      mapName = std::string(rec.getString(MapDB::InternalName));
    }
  }
  catch (MapDB::NotFound)
  {
    mapName = "Unknown map";
  }

  return mapName;
}

int MapDB::findMapName(const std::string &map_name)
{
  for (Iterator i = gMapDB.begin(); i != gMapDB.end(); ++i)
  {
    if (i->getString(MapDB::InternalName) == map_name)
    {
      return static_cast<int>(i->getUInt(MapDB::MapID));
    }
  }

  return -1;
}

const char * getGroundEffectDoodad(unsigned int effectID, int DoodadNum)
{
  try
  {
    unsigned int doodadId = gGroundEffectTextureDB.getByID(effectID).getUInt(GroundEffectTextureDB::Doodads + DoodadNum);
    return gGroundEffectDoodadDB.getByID(doodadId).getString(GroundEffectDoodadDB::Filename());
  }
  catch (DBCFile::NotFound)
  {
    LogError << "Tried to get a not existing row in GroundEffectTextureDB or GroundEffectDoodadDB ( effectID = " << effectID << ", DoodadNum = " << DoodadNum << " )!" << std::endl;
    return 0;
  }
}

int LiquidTypeDB::getLiquidType(int pID)
{
  int type = 0;
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    type = gLiquidTypeDB.getFieldCount() > LiquidTypeDB::Type ? rec.getUInt(LiquidTypeDB::Type) : 0;
  }
  catch (DBCFile::NotFound const&)
  {
    type = 0;
  }
  return type;
}

std::string  LiquidTypeDB::getLiquidName(int pID)
{
  std::string type = "Unknown type";
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    std::string name = rec.getString(LiquidTypeDB::Name);
    if (!name.empty())
    {
      type = name;
    }
  }
  catch (DBCFile::NotFound const&)
  {
    type = "Unknown type";
  }

  return type;
}

std::string WMOAreaTableDB::getWMOAreaName(int WMOId, int namesetId)
{
    if (WMOId == -1)
    {
        return "Unknown location";
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getUInt(WMOAreaTableDB::NameSetId) == namesetId && i->getUInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            // wmoareatableid = i->getUInt(WMOAreaTableDB::ID);
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                return areaName;
            else
            {   // get name from area instead
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    auto rec = gAreaDB.getByID(areatableid);
                    return rec.getLocalizedString(AreaDB::Name);
                }
                else
                    return "Unknown location"; // nullptr? need to get it from terrain
            }
        }
    }
    throw NotFound();
}

std::vector<std::string> WMOAreaTableDB::getWMOAreaNames(int WMOId)
{
    std::vector<std::string> areanamesvect;

    if (WMOId == -1)
    {
        return areanamesvect;
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getUInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            // wmoareatableid = i->getUInt(WMOAreaTableDB::ID);
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                areanamesvect.push_back(areaName);
            else
            {   // get name from area instead
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    auto rec = gAreaDB.getByID(areatableid);
                    areanamesvect.push_back(rec.getLocalizedString(AreaDB::Name));
                }
                else
                    areanamesvect.push_back(""); // nullptr? need to get it from terrain
            }
        }
        // could optimise and break when iterator WmoId is higher than the Wmodid, but this wouldn't support unordered DBCs.
    }
    return areanamesvect;
}
