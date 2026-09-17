// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class DBCFile;

namespace Noggit::DB2
{
  // Modern (CASC) clients ship WDC5 .db2 tables; noggit's map view reads the fixed-column WotLK DBCs
  // through DBCFile (gLightDB, gMapDB, ...: 150+ call sites). Instead of rewriting those, every table
  // the editor needs is read from the client's DB2 and re-emitted in the WotLK column layout, so the
  // consumers stay untouched. Field order comes from the WoWDBDefs definitions (the "definitions" folder),
  // matched by the layout hash the file carries, then by client build.
  // Measured layouts / mapping rules: docs/client_re/41_modern_casc_client_support_research.md section 3.

  // Client build string ("1.15.9.69722") used when a table's layout hash is unknown to the definitions.
  void setClientBuild(std::string const& build);

  // Fill `dbc` (a DBCFile created for a WotLK "DBFilesClient\X.dbc" name) from the modern client.
  // Returns false when no adapter exists for that table (the DBCFile is left empty).
  bool fillDBC(DBCFile& dbc, std::string const& dbc_filename);

  // MH2O: a liquid_object_or_lvf value >= 42 is a LiquidObject id, not a vertex format. Resolves
  // LiquidObject.LiquidTypeID -> LiquidType.MaterialID -> LiquidMaterial.LVF (0..3). Returns 0 when unknown.
  // MH2O `liquid_object_or_lvf` >= 42 -> liquid vertex format (0 height+depth, 1 height+uv, 2 depth only,
  // 3 height+uv+depth). `liquid_type` is the instance's own liquid type, used when the LiquidObject row is
  // missing (Classic Era / Anniversary ship no row 42 although every ocean layer references it).
  int liquidObjectVertexFormat(std::uint32_t liquid_object_id, std::uint32_t liquid_type);

  // Item model / texture variants (docs/client_re/42 sec 13). ItemDisplayInfo names RESOURCES; ModelFileData and
  // TextureFileData list one fileDataID per race / gender / side variant and ComponentModelFileData /
  // ComponentTextureFileData say which wearer each file is for. `race` / `sex` are ChrRaces id and 0 male /
  // 1 female; `position` is the item's side (0 = model 1, 1 = model 2) or -1. Returns the listfile path (or a
  // registered synthetic one) of the best match, "" when the resource is unknown.
  std::string itemModelPathForVariant(std::uint32_t model_resources_id, std::uint32_t race, std::uint32_t sex, int position);
  std::string itemTexturePathForVariant(std::uint32_t material_resources_id, std::uint32_t sex);
  // "modelres:<id>:<pos>" / "matres:<id>" pseudo-names the adapters emit; resolves them, passes anything else through.
  std::string resolveItemVariantName(std::string const& name, std::uint32_t race, std::uint32_t sex);

  // Humanoid NPC customization (docs/client_re/42 sec 14). 1.15.9 has no CharSections and its
  // CharacterFacialHairStyles rows carry race / sex / variation 0, so an NPC's look is the Shadowlands-style
  // chain: CreatureDisplayInfoOption rows (parent = CreatureDisplayInfoExtra id) name the chosen
  // ChrCustomizationChoice per option; the choice's ChrCustomizationElement rows (those whose
  // RelatedChrCustomizationChoiceID is 0 or also chosen -- hair textures hang on the STYLE choice related to
  // the COLOUR choice) carry ChrCustomizationGeoset {GeosetType, GeosetID} (= geoset Type*100+ID, type 0 id
  // 0 = no hair) and ChrCustomizationMaterial {ChrModelTextureTargetID, MaterialResourcesID}; the race/sex
  // ChrModel's CharComponentTextureLayoutID selects the ChrModelTextureLayer rows that map a target to an
  // M2 texture type (1 = the body composite, baked for NPCs; 6 = hair; 8 = extra skin).
  struct CreatureCustomizationTexture
  {
    std::uint32_t texture_type = 0;   // M2 texture type the material feeds (1 body, 6 hair, 8 extra skin)
    std::uint32_t target = 0;         // ChrModelTextureTargetID (1 base skin, 4/5 face, 7/8 facial, 10 hair, 11/12 scalp, 13/14 pelvis/torso, 2 extra)
    std::string path;                 // TextureFileData path (listfile or registered fdid/<id>.blp)
  };
  struct CreatureCustomization
  {
    bool has_options = false;                                          // CreatureDisplayInfoOption rows exist
    std::vector<std::pair<std::uint32_t, std::uint32_t>> geosets;       // {GeosetType, GeosetID}, first element per type
    std::vector<CreatureCustomizationTexture> textures;
  };
  CreatureCustomization creatureCustomization(std::uint32_t creature_display_info_extra_id, std::uint32_t race, std::uint32_t sex);

  // Drop cached tables (project switch).
  void reset();
}
