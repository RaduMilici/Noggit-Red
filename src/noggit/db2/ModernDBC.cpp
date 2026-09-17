// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/db2/ModernDBC.hpp>
#include <noggit/db2/WDC5File.hpp>
#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/application/NoggitApplication.hpp>

#include <blizzard-archive-library/include/ClientData.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>
#include <blizzard-database-library/include/DatabaseDefinition.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Noggit::DB2
{
  namespace
  {
    std::string g_client_build;
    std::mutex g_mutex;

    // ---------------------------------------------------------------------------------------- tables
    struct Table
    {
      WDC5File file;
      std::vector<std::string> field_names;                 // inline fields, in record order
      std::unordered_map<std::string, std::size_t> by_name;
      std::string name;
      bool loaded = false;
      bool ok = false;

      [[nodiscard]] bool has(char const* column) const { return by_name.count(column) != 0; }
      [[nodiscard]] std::size_t field(char const* column) const
      {
        auto const it = by_name.find(column);
        return it == by_name.end() ? static_cast<std::size_t>(-1) : it->second;
      }
      [[nodiscard]] std::uint32_t u32(std::size_t record, char const* column, std::size_t index = 0) const
      {
        auto const f = field(column);
        return f == static_cast<std::size_t>(-1) ? 0u : file.getUInt(record, f, index);
      }
      [[nodiscard]] std::int32_t i32(std::size_t record, char const* column, std::size_t index = 0) const
      {
        auto const f = field(column);
        return f == static_cast<std::size_t>(-1) ? 0 : file.getInt(record, f, index);
      }
      [[nodiscard]] float f32(std::size_t record, char const* column, std::size_t index = 0) const
      {
        auto const f = field(column);
        return f == static_cast<std::size_t>(-1) ? 0.f : file.getFloat(record, f, index);
      }
      [[nodiscard]] std::string str(std::size_t record, char const* column, std::size_t index = 0) const
      {
        auto const f = field(column);
        return f == static_cast<std::size_t>(-1) ? std::string() : file.getString(record, f, index);
      }
    };

    std::map<std::string, std::shared_ptr<Table>> g_tables;

    std::string lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    }

    std::string hex8(std::uint32_t value)
    {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "%08X", value);
      return buffer;
    }

    // Column names for the layout the file carries. Matched by LAYOUT hash first (robust across patches
    // that keep the layout), then by the client build, then -- as a last resort -- the newest layout.
    bool resolveFields(Table& table, std::string const& definitions_dir)
    {
      std::filesystem::path const dbd = std::filesystem::path(definitions_dir) / (table.name + ".dbd");
      if (!std::filesystem::exists(dbd))
      {
        LogError << "DB2 -> DBC: no definition file " << dbd.string() << std::endl;
        return false;
      }

      BlizzardDatabaseLib::DatabaseDefinition definition(dbd.string());
      auto const db_definition = definition.Read();
      if (db_definition.versionDefinitions.empty())
      {
        LogError << "DB2 -> DBC: definition " << dbd.string() << " has no layouts" << std::endl;
        return false;
      }

      std::string const layout = hex8(table.file.layoutHash());
      BlizzardDatabaseLib::Structures::VersionDefinitions const* chosen = nullptr;
      char const* how = "layout hash";
      for (auto const& version : db_definition.versionDefinitions)
      {
        for (auto const& hash : version.layoutHashes)
        {
          std::string trimmed = hash;
          trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isspace(c); }), trimmed.end());
          std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
          if (trimmed == layout)
          {
            chosen = &version;
            break;
          }
        }
        if (chosen) break;
      }
      if (!chosen && !g_client_build.empty())
      {
        try
        {
          BlizzardDatabaseLib::Structures::Build const build(g_client_build);
          for (auto const& version : db_definition.versionDefinitions)
          {
            if (std::find(version.builds.begin(), version.builds.end(), build) != version.builds.end())
            {
              chosen = &version;
              how = "client build";
              break;
            }
            for (auto range : version.buildRanges) // Contains() is non-const in the library
            {
              if (range.Contains(build))
              {
                chosen = &version;
                how = "client build range";
                break;
              }
            }
            if (chosen) break;
          }
        }
        catch (...)
        {
        }
      }
      if (!chosen)
      {
        LogError << "DB2 -> DBC: " << table.name << " layout " << layout << " (build '" << g_client_build
                 << "') is not in " << dbd.string() << "; refresh the definitions folder from WoWDBDefs." << std::endl;
        return false;
      }

      table.field_names.clear();
      table.by_name.clear();
      for (auto const& def : chosen->definitions)
      {
        if (!def.IsInline)
        {
          continue; // $noninline$ id lives in the id list, a $noninline,relation$ in the reference data
        }
        std::string name = def.name;
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        table.by_name.emplace(name, table.field_names.size());
        table.field_names.push_back(name);
      }

      if (table.field_names.size() != table.file.fieldCount())
      {
        LogError << "DB2 -> DBC: " << table.name << " layout " << layout << " (" << how << ") lists "
                 << table.field_names.size() << " inline columns but the file has " << table.file.fieldCount()
                 << " fields; column names may be shifted." << std::endl;
      }
      else
      {
        LogDebug << "DB2 -> DBC: " << table.name << " layout " << layout << " matched by " << how << " ("
                 << table.file.recordCount() << " records, " << table.file.fieldCount() << " fields)" << std::endl;
      }
      return true;
    }

    std::shared_ptr<Table> loadTable(std::string const& name)
    {
      std::lock_guard<std::mutex> const lock(g_mutex);
      auto it = g_tables.find(name);
      if (it != g_tables.end())
      {
        return it->second;
      }

      auto table = std::make_shared<Table>();
      table->name = name;
      table->loaded = true;
      g_tables.emplace(name, table);

      auto* app = Noggit::Application::NoggitApplication::instance();
      auto* client_data = app ? app->clientData() : nullptr;
      if (!client_data)
      {
        return table;
      }

      std::string const path = "dbfilesclient/" + lower(name) + ".db2";
      try
      {
        BlizzardArchive::ClientFile file(BlizzardArchive::Listfile::FileKey(path), client_data);
        if (file.isEof() || file.getSize() < 8)
        {
          LogError << "DB2 -> DBC: could not open " << path << std::endl;
          return table;
        }
        std::vector<char> data(file.getBuffer(), file.getBuffer() + file.getSize());
        std::string error;
        if (!table->file.load(std::move(data), &error))
        {
          LogError << "DB2 -> DBC: " << path << ": " << error << std::endl;
          return table;
        }
      }
      catch (std::exception const& e)
      {
        LogError << "DB2 -> DBC: could not open " << path << ": " << e.what() << std::endl;
        return table;
      }

      std::string const definitions = app->getConfiguration() ? app->getConfiguration()->ApplicationDatabaseDefinitionsPath : std::string("definitions");
      table->ok = resolveFields(*table, definitions);
      return table;
    }

    // ---------------------------------------------------------------------------------------- output
    // Builds a WotLK WDBC record block (4-byte columns) + string table for DBCFile::loadSynthesized.
    struct DbcBuilder
    {
      explicit DbcBuilder(std::uint32_t fields) : field_count(fields) { strings.push_back('\0'); }

      void begin()
      {
        row.assign(field_count, 0u);
      }
      void end()
      {
        auto const* bytes = reinterpret_cast<unsigned char const*>(row.data());
        data.insert(data.end(), bytes, bytes + row.size() * 4);
        ++records;
      }
      void u32(std::size_t field, std::uint32_t value) { if (field < field_count) row[field] = value; }
      void i32(std::size_t field, std::int32_t value) { u32(field, static_cast<std::uint32_t>(value)); }
      void f32(std::size_t field, float value)
      {
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        u32(field, bits);
      }
      void str(std::size_t field, std::string const& value)
      {
        if (value.empty())
        {
          u32(field, 0);
          return;
        }
        u32(field, static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), value.begin(), value.end());
        strings.push_back('\0');
      }
      // WotLK locstring: 16 locale npc_item_slots + flags mask. The DB2 carries the install locale's text only;
      // it goes into slot 0 (enUS) which is also where noggit looks first.
      void loc(std::size_t field, std::string const& value)
      {
        str(field, value);
        u32(field + 16, value.empty() ? 0u : 0x1u);
      }

      std::uint32_t field_count;
      std::uint32_t records = 0;
      std::vector<std::uint32_t> row;
      std::vector<unsigned char> data;
      std::vector<char> strings;
    };

    std::string pathForFileDataId(std::uint32_t fdid)
    {
      if (!fdid) return {};
      auto* app = Noggit::Application::NoggitApplication::instance();
      auto* client_data = app ? app->clientData() : nullptr;
      if (!client_data) return {};
      std::string path = client_data->listfile()->getPath(fdid);
      if (path.empty())
      {
        // TextureManager / the model loaders understand this pseudo name (opened by fileDataID)
        return "fdid:" + std::to_string(fdid);
      }
      return path;
    }

    // ---------------------------------------------------------------------------------------- item variants
    struct ComponentInfo { std::uint8_t gender = 2; std::uint8_t cls = 0; std::uint8_t race = 0; std::uint8_t position = 255; bool known = false; };
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_model_files_by_resource;   // ModelResourcesID -> fdids
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_texture_files_by_material; // MaterialResourcesID -> fdids
    std::unordered_map<std::uint32_t, ComponentInfo> g_component_model;                        // fdid -> wearer
    std::unordered_map<std::uint32_t, ComponentInfo> g_component_texture;
    bool g_item_variants_loaded = false;

    void loadItemVariants()
    {
      if (g_item_variants_loaded) return;
      g_item_variants_loaded = true;
      auto models = loadTable("ModelFileData");
      if (models->ok)
      {
        for (std::size_t r = 0; r < models->file.recordCount(); ++r)
        {
          std::uint32_t const parent = models->file.parentId(r);
          if (parent) g_model_files_by_resource[parent].push_back(models->file.id(r));
        }
      }
      auto textures = loadTable("TextureFileData");
      if (textures->ok)
      {
        for (std::size_t r = 0; r < textures->file.recordCount(); ++r)
        {
          std::uint32_t const parent = textures->file.parentId(r);
          if (parent) g_texture_files_by_material[parent].push_back(textures->file.id(r));
        }
      }
      auto cm = loadTable("ComponentModelFileData");
      if (cm->ok)
      {
        for (std::size_t r = 0; r < cm->file.recordCount(); ++r)
        {
          ComponentInfo info;
          info.gender = static_cast<std::uint8_t>(cm->u32(r, "GenderIndex"));
          info.cls = static_cast<std::uint8_t>(cm->u32(r, "ClassID"));
          info.race = static_cast<std::uint8_t>(cm->u32(r, "RaceID"));
          info.position = static_cast<std::uint8_t>(cm->u32(r, "PositionIndex"));
          info.known = true;
          g_component_model[cm->file.id(r)] = info;
        }
      }
      auto ct = loadTable("ComponentTextureFileData");
      if (ct->ok)
      {
        for (std::size_t r = 0; r < ct->file.recordCount(); ++r)
        {
          ComponentInfo info;
          info.gender = static_cast<std::uint8_t>(ct->u32(r, "GenderIndex"));
          info.cls = static_cast<std::uint8_t>(ct->u32(r, "ClassID"));
          info.race = static_cast<std::uint8_t>(ct->u32(r, "RaceID"));
          info.known = true;
          g_component_texture[ct->file.id(r)] = info;
        }
      }
      LogDebug << "DB2 -> DBC: item variants: " << g_model_files_by_resource.size() << " model resources, "
               << g_texture_files_by_material.size() << " material resources, " << g_component_model.size()
               << " model wearer rows, " << g_component_texture.size() << " texture wearer rows" << std::endl;
    }

    // ---------------------------------------------------------------------------------------- fdid helpers
    // A fileDataID as a path the string-keyed WotLK code can open: the listfile name, else a synthetic
    // "fdid/<id>.<ext>" registered with the listfile (Listfile::registerPath), so FileKey(path) resolves.
    std::string pathForFileDataID(std::uint32_t fdid, char const* ext)
    {
      if (!fdid)
      {
        return {};
      }
      auto* app = Noggit::Application::NoggitApplication::instance();
      auto* listfile = (app && app->clientData()) ? app->clientData()->listfile() : nullptr;
      if (!listfile)
      {
        return "fdid/" + std::to_string(fdid) + "." + ext;
      }
      std::string path = listfile->getPath(fdid);
      if (path.empty())
      {
        path = "fdid/" + std::to_string(fdid) + "." + ext;
        listfile->registerPath(fdid, path);
      }
      return path;
    }

    // ModelFileData / TextureFileData: rows keyed by the fdid, parented (relation) by the resource id.
    // Returns fdid -> resource id maps; first fdid per resource wins (the tables list one per LOD / usage).
    std::unordered_map<std::uint32_t, std::uint32_t> firstFileDataIDByParent(std::string const& table)
    {
      std::unordered_map<std::uint32_t, std::uint32_t> out;
      auto t = loadTable(table);
      if (!t->ok) return out;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const parent = t->file.parentId(r);
        if (parent && !out.count(parent))
        {
          out[parent] = t->file.id(r);
        }
      }
      return out;
    }

    // ---------------------------------------------------------------------------------------- NPC customization
    // docs/client_re/42 sec 14. Tables measured on 1.15.9.69722: CreatureDisplayInfoOption 35,409 rows for
    // 7,283 of 7,287 extras (5 per NPC: skin, face, hair style, hair colour, facial hair); ChrCustomizationElement
    // 6,134 (5,577 material, 557 geoset, 4,838 with a related choice); ChrCustomizationGeoset 890 (types 0 hair,
    // 1/2/3 facial, 16/33 troll); ChrCustomizationMaterial 7,186 (targets 1 4 5 7 8 10 11 12 13 14 2);
    // ChrModelTextureLayer 24 rows for layouts 1 (512x512) and 2 (1024x512): target 10 -> type 6, 2 -> 8, all
    // others -> type 1 composite sections. Legacy CreatureDisplayInfoExtra.HairStyleID agrees with the chain on
    // 6,979 extras and disagrees on 247 (428 chain NPCs are bald: type 0 id 0), CharacterFacialHairStyles has
    // race/sex/variation 0 on all 136 rows -> the chain is what the client draws.
    struct CustomizationElement { std::uint32_t related = 0; std::uint32_t geoset = 0; std::uint32_t material = 0; };
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_choices_by_extra;                     // extra -> chosen choice ids (row order)
    std::unordered_map<std::uint32_t, std::vector<CustomizationElement>> g_elements_by_choice;
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> g_custom_geosets;        // id -> {type, geoset id}
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> g_custom_materials;      // id -> {target, MaterialResourcesID}
    std::unordered_map<std::uint32_t, std::uint32_t> g_layout_by_race_sex;                                // race * 2 + sex -> CharComponentTextureLayoutID
    std::unordered_map<std::uint64_t, std::uint32_t> g_texture_type_by_layout_target;                     // layout << 32 | target -> M2 texture type
    bool g_customization_loaded = false;
    std::mutex g_customization_mutex;

    void loadCustomization()
    {
      if (g_customization_loaded) return;
      g_customization_loaded = true;
      auto options = loadTable("CreatureDisplayInfoOption");
      if (options->ok)
      {
        for (std::size_t r = 0; r < options->file.recordCount(); ++r)
        {
          std::uint32_t const extra = options->file.parentId(r);
          std::uint32_t const choice = options->u32(r, "ChrCustomizationChoiceID");
          if (extra && choice) g_choices_by_extra[extra].push_back(choice);
        }
      }
      auto elements = loadTable("ChrCustomizationElement");
      if (elements->ok)
      {
        for (std::size_t r = 0; r < elements->file.recordCount(); ++r)
        {
          CustomizationElement e;
          e.related = elements->u32(r, "RelatedChrCustomizationChoiceID");
          e.geoset = elements->u32(r, "ChrCustomizationGeosetID");
          e.material = elements->u32(r, "ChrCustomizationMaterialID");
          if (e.geoset || e.material) g_elements_by_choice[elements->u32(r, "ChrCustomizationChoiceID")].push_back(e);
        }
      }
      auto geosets = loadTable("ChrCustomizationGeoset");
      if (geosets->ok)
      {
        for (std::size_t r = 0; r < geosets->file.recordCount(); ++r)
        {
          g_custom_geosets[geosets->file.id(r)] = {geosets->u32(r, "GeosetType"), geosets->u32(r, "GeosetID")};
        }
      }
      auto materials = loadTable("ChrCustomizationMaterial");
      if (materials->ok)
      {
        for (std::size_t r = 0; r < materials->file.recordCount(); ++r)
        {
          g_custom_materials[materials->file.id(r)] = {materials->u32(r, "ChrModelTextureTargetID"), materials->u32(r, "MaterialResourcesID")};
        }
      }
      std::unordered_map<std::uint32_t, std::uint32_t> layout_by_model;
      auto chr_models = loadTable("ChrModel");
      if (chr_models->ok)
      {
        for (std::size_t r = 0; r < chr_models->file.recordCount(); ++r)
        {
          layout_by_model[chr_models->file.id(r)] = chr_models->u32(r, "CharComponentTextureLayoutID");
        }
      }
      auto race_models = loadTable("ChrRaceXChrModel");
      if (race_models->ok)
      {
        for (std::size_t r = 0; r < race_models->file.recordCount(); ++r)
        {
          std::uint32_t const race = race_models->has("ChrRacesID") ? race_models->u32(r, "ChrRacesID") : race_models->file.parentId(r);
          auto const layout = layout_by_model.find(race_models->u32(r, "ChrModelID"));
          if (race && layout != layout_by_model.end())
          {
            g_layout_by_race_sex[race * 2 + (race_models->u32(r, "Sex") & 1)] = layout->second;
          }
        }
      }
      auto layers = loadTable("ChrModelTextureLayer");
      if (layers->ok)
      {
        for (std::size_t r = 0; r < layers->file.recordCount(); ++r)
        {
          std::uint32_t const layout = layers->has("CharComponentTextureLayoutsID") ? layers->u32(r, "CharComponentTextureLayoutsID") : layers->file.parentId(r);
          std::uint32_t const type = layers->u32(r, "TextureType");
          for (std::size_t k = 0; k < 2; ++k)
          {
            std::uint32_t const target = layers->u32(r, "ChrModelTextureTargetID", k);
            if (target) g_texture_type_by_layout_target[(static_cast<std::uint64_t>(layout) << 32) | target] = type;
          }
        }
      }
      LogDebug << "DB2 -> DBC: NPC customization: " << g_choices_by_extra.size() << " extras with options, "
               << g_elements_by_choice.size() << " choices with elements, " << g_custom_geosets.size() << " geosets, "
               << g_custom_materials.size() << " materials, " << g_texture_type_by_layout_target.size() << " layer targets" << std::endl;
    }

    // HelmetGeosetVisData 3.3.5 (6 used columns): ID HideGeoset[hair facial1 facial2 facial3 ears], each a race
    // bitmask (bit race-1). 1.15.9 has no such table; HelmetGeosetData (103B3B37: RaceID HideGeosetGroup
    // RaceBitSelection, parent = HelmetGeosetVisDataID -- "values equal to the old HelmetGeosetVisDataRec.m_ID",
    // still what ItemDisplayInfo.HelmetGeosetVis[2] indexes: 15 of the 16 vis ids used by 33,332 items) lists one
    // row per race per hidden geoset GROUP (0 hair, 1..3 facial, 7 ears -- the geoset family number). The Defias
    // bandana row 247 hides facial 1/2/3 and keeps the hair, as in 3.3.5a. RaceBitSelection is 0 on every row.
    bool buildHelmetGeosetVisData(DBCFile& dbc)
    {
      auto t = loadTable("HelmetGeosetData");
      if (!t->ok) return false;
      std::map<std::uint32_t, std::array<std::uint32_t, 5>> masks;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const vis = t->has("HelmetGeosetVisDataID") ? t->u32(r, "HelmetGeosetVisDataID") : t->file.parentId(r);
        std::uint32_t const race = t->u32(r, "RaceID");
        std::uint32_t const group = t->u32(r, "HideGeosetGroup");
        if (!vis || !race || race > 32) continue;
        std::size_t const column = group <= 3 ? group : group == 7 ? 4 : static_cast<std::size_t>(-1);
        if (column == static_cast<std::size_t>(-1)) continue;
        masks[vis][column] |= 1u << (race - 1);
      }
      DbcBuilder out(6);
      for (auto const& [vis, mask] : masks)
      {
        out.begin();
        out.u32(0, vis);
        for (std::size_t k = 0; k < 5; ++k) out.u32(1 + k, mask[k]);
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ---------------------------------------------------------------------------------------- adapters
    // CreatureModelData 3.3.5 (28 columns): ID Flags ModelName SizeClass ModelScale BloodID FootprintTextureID
    // FootprintTextureLength FootprintTextureWidth FootprintParticleScale FoleyMaterialID FootstepShakeSize
    // DeathThudShakeSize SoundID CollisionWidth CollisionHeight MountHeight GeoBoxMin[3] GeoBoxMax[3]
    // WorldEffectScale AttachedEffectScale MissileCollisionRadius MissileCollisionPush MissileCollisionRaise.
    // 1.15.9 (layout 3FFEE408) has FileDataID instead of the path.
    bool buildCreatureModelData(DBCFile& dbc)
    {
      auto t = loadTable("CreatureModelData");
      if (!t->ok) return false;
      // Column 28 (noggit-only, beyond the 28 WotLK columns): the plain <-> HD sibling row of a character
      // model. The Forever Beta ships 23 `<race><sex>_hd.m2` files beside their plain files and BOTH have
      // rows here (humanmale.m2 = 49, humanmale_hd.m2 = 7661; 18 pairs), while 8,568 of the 8,915 display
      // extras carry a classic AND an HD bake. Nothing else links the two (CreatureDisplayInfo's
      // ConditionalCreatureModelID is 0 on all 14,097 rows, ChrModel lists the 18 plain player models
      // only), so the client's live "character models" option can only be the file-name pairing plus the
      // bake pair; World.cpp swaps both under Settings > "Character models". docs/client_re/42 sec 23.
      std::vector<std::string> paths(t->file.recordCount());
      std::unordered_map<std::string, std::uint32_t> first_row_by_path;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        paths[r] = pathForFileDataID(t->u32(r, "FileDataID"), "m2");
        std::string key = paths[r];
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        first_row_by_path.emplace(std::move(key), t->file.id(r));
      }
      auto const sibling_of = [&](std::string path) -> std::uint32_t
      {
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static std::string const hd_suffix = "_hd.m2";
        std::string other;
        if (path.size() > hd_suffix.size() && path.compare(path.size() - hd_suffix.size(), hd_suffix.size(), hd_suffix) == 0)
        {
          other = path.substr(0, path.size() - hd_suffix.size()) + ".m2";
        }
        else if (path.size() > 3 && path.compare(path.size() - 3, 3, ".m2") == 0 && path.rfind("character/", 0) == 0)
        {
          other = path.substr(0, path.size() - 3) + hd_suffix;
        }
        if (other.empty()) return 0u;
        auto const it = first_row_by_path.find(other);
        return it == first_row_by_path.end() ? 0u : it->second;
      };
      DbcBuilder out(29);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "Flags"));
        out.str(2, paths[r]);
        out.u32(3, t->u32(r, "SizeClass"));
        out.f32(4, t->f32(r, "ModelScale"));
        out.u32(5, t->u32(r, "BloodID"));
        out.u32(6, t->u32(r, "FootprintTextureID"));
        out.f32(7, t->f32(r, "FootprintTextureLength"));
        out.f32(8, t->f32(r, "FootprintTextureWidth"));
        out.f32(9, t->f32(r, "FootprintParticleScale"));
        out.u32(10, t->u32(r, "FoleyMaterialID"));
        out.u32(11, t->u32(r, "FootstepCameraEffectID"));
        out.u32(12, t->u32(r, "DeathThudCameraEffectID"));
        out.u32(13, t->u32(r, "SoundID"));
        out.f32(14, t->f32(r, "CollisionWidth"));
        out.f32(15, t->f32(r, "CollisionHeight"));
        out.f32(16, t->f32(r, "MountHeight"));
        for (std::size_t k = 0; k < 6; ++k) out.f32(17 + k, t->f32(r, "GeoBox", k));
        out.f32(23, t->f32(r, "WorldEffectScale"));
        out.f32(24, t->f32(r, "AttachedEffectScale"));
        out.f32(25, t->f32(r, "MissileCollisionRadius"));
        out.f32(26, t->f32(r, "MissileCollisionPush"));
        out.f32(27, t->f32(r, "MissileCollisionRaise"));
        out.u32(28, sibling_of(paths[r]));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // CreatureDisplayInfo 3.3.5 (16): ID ModelID SoundID ExtendedDisplayInfoID CreatureModelScale
    // CreatureModelAlpha TextureVariation[3] PortraitTextureName BloodLevel BloodID NPCSoundID
    // ParticleColorID CreatureGeosetData ObjectEffectPackageID. 1.15.9 (F75F884C) names the variation
    // textures by fdid (TextureVariationFileDataID[4]); World.cpp keeps a variation as-is when it holds a '/'.
    bool buildCreatureDisplayInfo(DBCFile& dbc)
    {
      auto t = loadTable("CreatureDisplayInfo");
      if (!t->ok) return false;
      DbcBuilder out(16);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "ModelID"));
        out.u32(2, t->u32(r, "SoundID"));
        out.u32(3, t->u32(r, "ExtendedDisplayInfoID"));
        out.f32(4, t->f32(r, "CreatureModelScale"));
        out.u32(5, t->u32(r, "CreatureModelAlpha"));
        for (std::size_t k = 0; k < 3; ++k)
        {
          out.str(6 + k, pathForFileDataID(t->u32(r, "TextureVariationFileDataID", k), "blp"));
        }
        out.str(9, pathForFileDataID(t->u32(r, "PortraitTextureFileDataID"), "blp"));
        out.u32(10, 0);
        out.u32(11, t->u32(r, "BloodID"));
        // noggit's CreatureDisplayInfoDB::ParticleColorID() reads column 12 for the WotLK layout while the
        // wowdev 3.3.5 layout keeps NPCSoundID there and ParticleColorID at 13; NPCSoundID is not read by
        // noggit (Sound = column 2), so the particle colour goes to both.
        out.u32(12, t->u32(r, "ParticleColorID"));
        out.u32(13, t->u32(r, "ParticleColorID"));
        out.u32(14, 0);
        out.u32(15, t->u32(r, "ObjectEffectPackageID"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // CreatureDisplayInfoExtra 3.3.5 (21): ID DisplayRaceID DisplaySexID SkinID FaceID HairStyleID
    // HairColorID FacialHairID NPCItemDisplay[11] (head shoulder shirt chest belt legs boots bracers gloves
    // tabard cape) Flags BakeName. 1.15.9 (F5E49981) keeps the customization ids and moves the item npc_item_slots
    // to NPCModelItemSlotDisplayInfo (parent = this id, ItemSlot 0..10 in the same order) and the baked
    // texture to BakeMaterialResourcesID -> TextureFileData.
    bool buildCreatureDisplayInfoExtra(DBCFile& dbc)
    {
      auto t = loadTable("CreatureDisplayInfoExtra");
      if (!t->ok) return false;
      std::unordered_map<std::uint32_t, std::array<std::uint32_t, 11>> npc_item_slots;
      {
        auto s = loadTable("NPCModelItemSlotDisplayInfo");
        if (s->ok)
        {
          for (std::size_t r = 0; r < s->file.recordCount(); ++r)
          {
            std::uint32_t const parent = s->file.parentId(r);
            std::uint32_t const slot = s->u32(r, "ItemSlot");
            if (parent && slot < 11)
            {
              auto& arr = npc_item_slots[parent];
              arr[slot] = s->u32(r, "ItemDisplayInfoID");
            }
          }
        }
      }
      auto const baked = firstFileDataIDByParent("TextureFileData");
      DbcBuilder out(22); // 21 WotLK columns + column 21: the HD bake (docs/client_re/42 sec 23)
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const id = t->file.id(r);
        out.begin();
        out.u32(0, id);
        out.u32(1, t->u32(r, "DisplayRaceID"));
        out.u32(2, t->u32(r, "DisplaySexID"));
        out.u32(3, t->u32(r, "SkinID"));
        out.u32(4, t->u32(r, "FaceID"));
        out.u32(5, t->u32(r, "HairStyleID"));
        out.u32(6, t->u32(r, "HairColorID"));
        out.u32(7, t->u32(r, "FacialHairID"));
        auto const it = npc_item_slots.find(id);
        for (std::size_t k = 0; k < 11; ++k)
        {
          out.u32(8 + k, it != npc_item_slots.end() ? it->second[k] : 0u);
        }
        out.u32(19, t->u32(r, "Flags"));
        auto const bake = baked.find(t->u32(r, "BakeMaterialResourcesID"));
        out.str(20, bake != baked.end() ? pathForFileDataID(bake->second, "blp") : std::string());
        auto const hd_bake = baked.find(t->has("HDBakeMaterialResourcesID") ? t->u32(r, "HDBakeMaterialResourcesID") : 0u);
        out.str(21, hd_bake != baked.end() ? pathForFileDataID(hd_bake->second, "blp") : std::string());
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // GameObjectDisplayInfo 3.3.5 (19): ID ModelName Sound[10] GeoBoxMin[3] GeoBoxMax[3]
    // ObjectEffectPackageID. 1.15.9 (7C5F0B90): FileDataID + GeoBox[6]; the sounds are gone.
    bool buildGameObjectDisplayInfo(DBCFile& dbc)
    {
      auto t = loadTable("GameObjectDisplayInfo");
      if (!t->ok) return false;
      DbcBuilder out(19);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        std::string path = pathForFileDataID(t->u32(r, "FileDataID"), "m2");
        if (path.empty() && t->has("ModelName")) path = t->str(r, "ModelName");
        out.str(1, path);
        for (std::size_t k = 0; k < 6; ++k) out.f32(12 + k, t->f32(r, "GeoBox", k));
        out.u32(18, t->u32(r, "ObjectEffectPackageID"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ItemDisplayInfo 3.3.5 (25): ID ModelName[2] ModelTexture[2] InventoryIcon[2] GeosetGroup[3] Flags
    // SpellVisualID GroupSoundIndex HelmetGeosetVis[2] Texture[8] ItemVisual ParticleColorID. 1.15.9
    // (9F3AB8A9): ModelResourcesID[2] -> ModelFileData (fdid rows parented by the resource id),
    // ModelMaterialResourcesID[2] -> TextureFileData, the eight component textures through
    // ItemDisplayInfoMaterialRes {ComponentSection 0..7, MaterialResourcesID} parented by the item display.
    bool buildItemDisplayInfo(DBCFile& dbc)
    {
      auto t = loadTable("ItemDisplayInfo");
      if (!t->ok) return false;
      loadItemVariants();
      // A resource with ONE file resolves to its path now; with several (race / gender / side variants,
      // docs/client_re/42 sec 13) the row carries a pseudo-name that World.cpp resolves with the wearer.
      auto const model_name = [&](std::uint32_t model_res, int position) -> std::string
      {
        auto const it = g_model_files_by_resource.find(model_res);
        if (it == g_model_files_by_resource.end() || it->second.empty()) return {};
        if (it->second.size() == 1) return pathForFileDataID(it->second.front(), "m2");
        return "modelres:" + std::to_string(model_res) + ":" + std::to_string(position);
      };
      auto const texture_path = [&](std::uint32_t material_res) -> std::string
      {
        auto const it = g_texture_files_by_material.find(material_res);
        if (it == g_texture_files_by_material.end() || it->second.empty()) return {};
        if (it->second.size() == 1) return pathForFileDataID(it->second.front(), "blp");
        return "matres:" + std::to_string(material_res);
      };
      std::unordered_map<std::uint32_t, std::array<std::uint32_t, 8>> components; // section -> MaterialResourcesID
      {
        auto c = loadTable("ItemDisplayInfoMaterialRes");
        if (c->ok)
        {
          for (std::size_t r = 0; r < c->file.recordCount(); ++r)
          {
            std::uint32_t const parent = c->file.parentId(r);
            std::uint32_t const section = c->u32(r, "ComponentSection");
            if (parent && section < 8)
            {
              components[parent][section] = c->u32(r, "MaterialResourcesID");
            }
          }
        }
      }
      DbcBuilder out(25);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const id = t->file.id(r);
        out.begin();
        out.u32(0, id);
        for (std::size_t k = 0; k < 2; ++k)
        {
          out.str(1 + k, model_name(t->u32(r, "ModelResourcesID", k), static_cast<int>(k)));
          out.str(3 + k, texture_path(t->u32(r, "ModelMaterialResourcesID", k)));
        }
        for (std::size_t k = 0; k < 3; ++k) out.u32(7 + k, t->u32(r, "GeosetGroup", k));
        out.u32(10, t->u32(r, "Flags"));
        out.u32(11, t->u32(r, "StateSpellVisualKitID"));
        out.u32(12, 0);
        for (std::size_t k = 0; k < 2; ++k) out.u32(13 + k, t->u32(r, "HelmetGeosetVis", k));
        auto const comp = components.find(id);
        for (std::size_t k = 0; k < 8; ++k)
        {
          out.str(15 + k, comp != components.end() && comp->second[k] ? texture_path(comp->second[k]) : std::string());
        }
        out.u32(23, t->u32(r, "ItemVisual"));
        out.u32(24, t->u32(r, "ParticleColorID"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // CharHairGeosets 3.3.5 (6): ID RaceID SexID VariationID GeosetID Showscalp. 1.15.9 (0DFA0F80): RaceID is
    // the relation column.
    bool buildCharHairGeosets(DBCFile& dbc)
    {
      auto t = loadTable("CharHairGeosets");
      if (!t->ok) return false;
      DbcBuilder out(6);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->has("RaceID") ? t->u32(r, "RaceID") : t->file.parentId(r));
        out.u32(2, t->u32(r, "SexID"));
        out.u32(3, t->u32(r, "VariationID"));
        out.u32(4, t->u32(r, "GeosetID"));
        out.u32(5, t->u32(r, "Showscalp"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // CharacterFacialHairStyles 3.3.5 (8): RaceID SexID VariationID Geoset[5] (no id column; noggit's
    // constants are 0..5). 1.15.9 (7D8B51FB): same data with a non-inline id.
    bool buildCharacterFacialHairStyles(DBCFile& dbc)
    {
      auto t = loadTable("CharacterFacialHairStyles");
      if (!t->ok) return false;
      DbcBuilder out(8);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->u32(r, "RaceID"));
        out.u32(1, t->u32(r, "SexID"));
        out.u32(2, t->u32(r, "VariationID"));
        for (std::size_t k = 0; k < 5; ++k) out.u32(3 + k, t->u32(r, "Geoset", k));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ParticleColor (10): ID Start[3] MID[3] End[3] -- unchanged.
    bool buildParticleColor(DBCFile& dbc)
    {
      auto t = loadTable("ParticleColor");
      if (!t->ok) return false;
      DbcBuilder out(10);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        for (std::size_t k = 0; k < 3; ++k) out.u32(1 + k, t->u32(r, "Start", k));
        for (std::size_t k = 0; k < 3; ++k) out.u32(4 + k, t->u32(r, "MID", k));
        for (std::size_t k = 0; k < 3; ++k) out.u32(7 + k, t->u32(r, "End", k));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ---------------------------------------------------------------------------------------- sound
    // SoundEntries 3.3.5 (30): ID SoundType Name File[10] Freq[10] DirectoryBase VolumeFloat Flags
    // MinDistance DistanceCutoff EAXDef SoundEntriesAdvancedID. Modern: SoundKit (2EB0F915: SoundType
    // VolumeFloat Flags MinDistance DistanceCutoff EAXDef SoundKitAdvancedID ...) + SoundKitEntry
    // (8F82FF7D: SoundKitID relation, FileDataID, Frequency, Volume). SoundKitName is gone from 1.15.
    // The players compose DirectoryBase + "\" + File[i], so the listfile path is split at its last '/'.
    // SoundKitEntry lists retail ids the Classic store does not ship (e.g. 566017), so entries are
    // filtered by existence.
    bool buildSoundEntries(DBCFile& dbc)
    {
      auto kits = loadTable("SoundKit");
      auto entries = loadTable("SoundKitEntry");
      if (!kits->ok || !entries->ok) return false;
      auto* app = Noggit::Application::NoggitApplication::instance();
      auto* client_data = app ? app->clientData() : nullptr;
      struct Entry { std::uint32_t fdid; std::uint32_t freq; };
      std::unordered_map<std::uint32_t, std::vector<Entry>> by_kit;
      std::size_t missing = 0;
      for (std::size_t r = 0; r < entries->file.recordCount(); ++r)
      {
        std::uint32_t const kit = entries->file.parentId(r);
        std::uint32_t const fdid = entries->u32(r, "FileDataID");
        if (!kit || !fdid) continue;
        if (client_data && !client_data->exists(BlizzardArchive::Listfile::FileKey(fdid)))
        {
          ++missing;
          continue;
        }
        by_kit[kit].push_back({fdid, entries->u32(r, "Frequency")});
      }
      DbcBuilder out(30);
      std::size_t rows = 0;
      for (std::size_t r = 0; r < kits->file.recordCount(); ++r)
      {
        std::uint32_t const id = kits->file.id(r);
        auto const it = by_kit.find(id);
        std::vector<Entry> const files = it != by_kit.end() ? it->second : std::vector<Entry>();
        out.begin();
        out.u32(0, id);
        out.u32(1, kits->u32(r, "SoundType"));
        std::string directory;
        std::string name;
        std::size_t slot = 0;
        for (auto const& e : files)
        {
          if (slot >= 10) break;
          std::string const path = pathForFileDataID(e.fdid, "ogg");
          auto const slash = path.rfind('/');
          std::string const dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
          std::string const base = slash == std::string::npos ? path : path.substr(slash + 1);
          if (directory.empty()) directory = dir;
          if (dir != directory) continue; // one DirectoryBase per row in the WotLK layout
          if (name.empty()) name = base.substr(0, base.rfind('.'));
          out.str(3 + slot, base);
          out.u32(13 + slot, e.freq);
          ++slot;
        }
        out.str(2, name.empty() ? "SoundKit " + std::to_string(id) : name);
        out.str(23, directory);
        out.f32(24, kits->f32(r, "VolumeFloat"));
        out.u32(25, kits->u32(r, "Flags"));
        out.f32(26, kits->f32(r, "MinDistance"));
        out.f32(27, kits->f32(r, "DistanceCutoff"));
        out.u32(28, kits->u32(r, "EAXDef"));
        out.u32(29, kits->u32(r, "SoundKitAdvancedID"));
        out.end();
        ++rows;
      }
      LogDebug << "DB2 -> DBC: SoundEntries from " << rows << " SoundKit rows, " << missing << " SoundKitEntry files not in this store" << std::endl;
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ZoneMusic (8): ID SetName SilenceIntervalMin[2] SilenceIntervalMax[2] Sounds[2] -- unchanged (72572D05).
    bool buildZoneMusic(DBCFile& dbc)
    {
      auto t = loadTable("ZoneMusic");
      if (!t->ok) return false;
      DbcBuilder out(8);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, t->str(r, "SetName"));
        for (std::size_t k = 0; k < 2; ++k) out.u32(2 + k, t->u32(r, "SilenceIntervalMin", k));
        for (std::size_t k = 0; k < 2; ++k) out.u32(4 + k, t->u32(r, "SilenceIntervalMax", k));
        for (std::size_t k = 0; k < 2; ++k) out.u32(6 + k, t->u32(r, "Sounds", k));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ZoneIntroMusicTable (5): ID Name SoundID Priority MinDelayMinutes -- unchanged (E1F93744).
    bool buildZoneIntroMusicTable(DBCFile& dbc)
    {
      auto t = loadTable("ZoneIntroMusicTable");
      if (!t->ok) return false;
      DbcBuilder out(5);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, t->str(r, "Name"));
        out.u32(2, t->u32(r, "SoundID"));
        out.u32(3, t->u32(r, "Priority"));
        out.u32(4, t->u32(r, "MinDelayMinutes"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // SoundAmbience (3): ID AmbienceID[2] (day, night). Modern (3FBF2710) adds Flags, SoundFilterID,
    // FlavorSoundFilterID, AmbienceStartID[2], AmbienceStopID[2].
    bool buildSoundAmbience(DBCFile& dbc)
    {
      auto t = loadTable("SoundAmbience");
      if (!t->ok) return false;
      DbcBuilder out(3);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        for (std::size_t k = 0; k < 2; ++k) out.u32(1 + k, t->u32(r, "AmbienceID", k));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // SoundProviderPreferences (24): ID Description Flags EAXEnvironmentSelection EAXDecayTime
    // EAX2EnvironmentSize EAX2EnvironmentDiffusion EAX2Room EAX2RoomHF EAX2DecayHFRatio EAX2Reflections
    // EAX2ReflectionsDelay EAX2Reverb EAX2ReverbDelay EAX2RoomRolloff EAX2AirAbsorption EAX3RoomLF
    // EAX3DecayLFRatio EAX3EchoTime EAX3EchoDepth EAX3ModulationTime EAX3ModulationDepth EAX3HFReference
    // EAX3LFReference -- same fields in 8FF22D98 (Flags moved last).
    bool buildSoundProviderPreferences(DBCFile& dbc)
    {
      auto t = loadTable("SoundProviderPreferences");
      if (!t->ok) return false;
      DbcBuilder out(24);
      char const* ints[] = { "EAXEnvironmentSelection", nullptr, nullptr, nullptr, "EAX2Room", "EAX2RoomHF", nullptr, "EAX2Reflections", nullptr, "EAX2Reverb", nullptr, nullptr, nullptr, "EAX3RoomLF" };
      char const* floats[] = { nullptr, "EAXDecayTime", "EAX2EnvironmentSize", "EAX2EnvironmentDiffusion", nullptr, nullptr, "EAX2DecayHFRatio", nullptr, "EAX2ReflectionsDelay", nullptr, "EAX2ReverbDelay", "EAX2RoomRolloff", "EAX2AirAbsorption", nullptr, "EAX3DecayLFRatio", "EAX3EchoTime", "EAX3EchoDepth", "EAX3ModulationTime", "EAX3ModulationDepth", "EAX3HFReference", "EAX3LFReference" };
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, t->str(r, "Description"));
        out.u32(2, t->u32(r, "Flags"));
        for (std::size_t k = 0; k < 14; ++k) if (ints[k]) out.u32(3 + k, t->u32(r, ints[k]));
        for (std::size_t k = 0; k < 21; ++k) if (floats[k]) out.f32(3 + k, t->f32(r, floats[k]));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // SoundWaterType (4): ID SoundType(liquid class) SoundSubtype(fluid speed 0/4/8) SoundID. The table is
    // gone from 1.15: LiquidType carries the loop directly (SoundBank 0 water 1 ocean 2 magma 3 slime,
    // SoundID). One row per (type, speed) so the WaterSoundPlayer's class+speed lookup always matches.
    bool buildSoundWaterType(DBCFile& dbc)
    {
      auto t = loadTable("LiquidType");
      if (!t->ok) return false;
      DbcBuilder out(4);
      std::uint32_t next_id = 1;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const sound = t->u32(r, "SoundID");
        if (!sound) continue;
        for (std::uint32_t speed : {0u, 4u, 8u})
        {
          out.begin();
          out.u32(0, next_id++);
          out.u32(1, t->u32(r, "SoundBank"));
          out.u32(2, speed);
          out.u32(3, sound);
          out.end();
        }
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // FootstepTerrainLookup (5): ID CreatureFootstepID TerrainSoundID SoundID SoundIDSplash -- unchanged (747D783F).
    bool buildFootstepTerrainLookup(DBCFile& dbc)
    {
      auto t = loadTable("FootstepTerrainLookup");
      if (!t->ok) return false;
      DbcBuilder out(5);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "CreatureFootstepID"));
        out.u32(2, t->u32(r, "TerrainSoundID"));
        out.u32(3, t->u32(r, "SoundID"));
        out.u32(4, t->u32(r, "SoundIDSplash"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // TerrainType (6): TerrainID TerrainDesc FootstepSprayRun FootstepSprayWalk SoundID Flags (AD4FFB9A: the
    // id is non-inline and TerrainID is a plain column).
    bool buildTerrainType(DBCFile& dbc)
    {
      auto t = loadTable("TerrainType");
      if (!t->ok) return false;
      DbcBuilder out(6);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, t->str(r, "TerrainDesc"));
        out.u32(2, t->u32(r, "FootstepSprayRun"));
        out.u32(3, t->u32(r, "FootstepSprayWalk"));
        out.u32(4, t->u32(r, "SoundID"));
        out.u32(5, t->u32(r, "Flags"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // CreatureSoundData 3.3.5 (38): the same named columns exist in E5EE765B.
    bool buildCreatureSoundData(DBCFile& dbc)
    {
      auto t = loadTable("CreatureSoundData");
      if (!t->ok) return false;
      char const* cols[] = { "SoundExertionID", "SoundExertionCriticalID", "SoundInjuryID", "SoundInjuryCriticalID",
        "SoundInjuryCrushingBlowID", "SoundDeathID", "SoundStunID", "SoundStandID", "SoundFootstepID", "SoundAggroID",
        "SoundWingFlapID", "SoundWingGlideID", "SoundAlertID" };
      char const* tail[] = { "NPCSoundID", "LoopSoundID", "CreatureImpactType", "SoundJumpStartID", "SoundJumpEndID",
        "SoundPetAttackID", "SoundPetOrderID", "SoundPetDismissID" };
      char const* tail2[] = { "BirthSoundID", "SpellCastDirectedSoundID", "SubmergeSoundID", "SubmergedSoundID", "CreatureSoundDataIDPet" };
      DbcBuilder out(38);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        for (std::size_t k = 0; k < 13; ++k) out.u32(1 + k, t->u32(r, cols[k]));
        for (std::size_t k = 0; k < 5; ++k) out.u32(14 + k, t->u32(r, "SoundFidget", k));
        for (std::size_t k = 0; k < 4; ++k) out.u32(19 + k, t->u32(r, "CustomAttack", k));
        for (std::size_t k = 0; k < 8; ++k) out.u32(23 + k, t->u32(r, tail[k]));
        out.f32(31, t->f32(r, "FidgetDelaySecondsMin"));
        out.f32(32, t->f32(r, "FidgetDelaySecondsMax"));
        for (std::size_t k = 0; k < 5; ++k) out.u32(33 + k, t->u32(r, tail2[k]));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // WMOAreaTable 3.3.5 (28): ID WMOID NameSetID WMOGroupID SoundProviderPref SoundProviderPrefUnderwater
    // AmbienceID ZoneMusic IntroSound Flags AreaTableID AreaName_lang(17). C5A7B977 keeps every column
    // (WMOID as the relation) and adds the underwater variants.
    bool buildWMOAreaTable(DBCFile& dbc)
    {
      auto t = loadTable("WMOAreaTable");
      if (!t->ok) return false;
      DbcBuilder out(28);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->has("WMOID") ? t->u32(r, "WMOID") : t->file.parentId(r));
        out.u32(2, t->u32(r, "NameSetID"));
        out.u32(3, t->u32(r, "WMOGroupID"));
        out.u32(4, t->u32(r, "SoundProviderPref"));
        out.u32(5, t->u32(r, "SoundProviderPrefUnderwater"));
        out.u32(6, t->u32(r, "AmbienceID"));
        out.u32(7, t->u32(r, "ZoneMusic"));
        out.u32(8, t->u32(r, "IntroSound"));
        out.u32(9, t->u32(r, "Flags"));
        out.u32(10, t->u32(r, "AreaTableID"));
        out.loc(11, t->str(r, "AreaName_lang"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // LoadingScreens (4): ID Name FileName HasWideScreen. A28F3422: Narrow / Wide / Wide169 fdids.
    bool buildLoadingScreens(DBCFile& dbc)
    {
      auto t = loadTable("LoadingScreens");
      if (!t->ok) return false;
      DbcBuilder out(4);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const wide = t->u32(r, "WideScreenFileDataID");
        std::uint32_t const narrow = t->u32(r, "NarrowScreenFileDataID");
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, "LoadingScreen " + std::to_string(t->file.id(r)));
        out.str(2, pathForFileDataID(wide ? wide : narrow, "blp"));
        out.u32(3, wide ? 1u : 0u);
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    bool buildAreaTable(DBCFile& dbc)
    {
      auto t = loadTable("AreaTable");
      if (!t->ok) return false;
      DbcBuilder out(36);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "ContinentID"));
        out.u32(2, t->u32(r, "ParentAreaID"));
        out.u32(3, t->u32(r, "AreaBit"));
        out.u32(4, t->u32(r, "Flags", 0));
        out.u32(5, t->u32(r, "SoundProviderPref"));
        out.u32(6, t->u32(r, "SoundProviderPrefUnderwater"));
        out.u32(7, t->u32(r, "AmbienceID"));
        out.u32(8, t->u32(r, "ZoneMusic"));
        out.u32(9, t->u32(r, "IntroSound"));
        out.i32(10, t->i32(r, "ExplorationLevel"));
        out.loc(11, t->str(r, "AreaName_lang"));
        out.u32(28, t->u32(r, "FactionGroupMask"));
        for (std::size_t k = 0; k < 4; ++k) out.u32(29 + k, t->u32(r, "LiquidTypeID", k));
        out.f32(33, t->f32(r, "MinElevation"));
        out.f32(34, t->f32(r, "Ambient_multiplier"));
        out.u32(35, t->has("LightID") ? t->u32(r, "LightID") : 0u); // gone from the classic-era layout
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    bool buildMap(DBCFile& dbc)
    {
      auto t = loadTable("Map");
      if (!t->ok) return false;
      DbcBuilder out(66);
      auto* app = Noggit::Application::NoggitApplication::instance();
      auto* listfile = (app && app->clientData()) ? app->clientData()->listfile() : nullptr;
      int registered = 0;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const instance_type = t->u32(r, "InstanceType");
        std::string const directory = t->str(r, "Directory");
        // Map.WdtFileDataID (8.1+): the WDT by file id. The map list tests `World\Maps\<Directory>\<Directory>.wdt`
        // through the listfile, and newer maps live in NUMBERED folders the community listfile has not named
        // (Classic Era "2856" = Scarlet Enclave had a root name hash; the Forever Beta roots carry none, so
        // 30 of its maps -- the new dungeons and raids among them -- fell out with "has no WDT file").
        // Register the path -> id pair when the listfile lacks it; the split ADT parts come from MAID.
        std::uint32_t const wdt_fdid = t->has("WdtFileDataID") ? t->u32(r, "WdtFileDataID") : 0u;
        if (listfile && wdt_fdid && !directory.empty())
        {
          std::string const path = "world/maps/" + directory + "/" + directory + ".wdt";
          // getFileDataID takes the listfile's own key form (normalizeFilenameInternal); the raw path
          // matched nothing and every map was re-registered (113 log lines on the beta).
          if (!listfile->getFileDataID(BlizzardArchive::ClientData::normalizeFilenameInternal(path)))
          {
            listfile->registerPath(wdt_fdid, path);
            ++registered;
            Log << "DB2 -> DBC: Map " << t->file.id(r) << " \"" << t->str(r, "MapName_lang") << "\" (folder " << directory
                << ") reached through WdtFileDataID " << wdt_fdid << std::endl;
          }
        }
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, directory);
        out.u32(2, instance_type);
        out.u32(3, t->u32(r, "Flags", 0));
        out.u32(4, t->has("PVP") ? t->u32(r, "PVP") : (instance_type == 3 ? 1u : 0u));
        out.loc(5, t->str(r, "MapName_lang"));
        out.u32(22, t->u32(r, "AreaTableID"));
        out.loc(23, t->str(r, "MapDescription0_lang"));
        out.loc(40, t->str(r, "MapDescription1_lang"));
        out.u32(57, t->u32(r, "LoadingScreenID"));
        out.f32(58, t->has("MinimapIconScale") ? t->f32(r, "MinimapIconScale") : 1.f);
        out.i32(59, t->has("CorpseMapID") ? t->i32(r, "CorpseMapID") : -1);
        out.f32(60, t->has("Corpse") ? t->f32(r, "Corpse", 0) : (t->has("CorpseX") ? t->f32(r, "CorpseX") : 0.f));
        out.f32(61, t->has("Corpse") ? t->f32(r, "Corpse", 1) : (t->has("CorpseY") ? t->f32(r, "CorpseY") : 0.f));
        out.i32(62, t->has("TimeOfDayOverride") ? t->i32(r, "TimeOfDayOverride") : -1);
        out.u32(63, t->u32(r, "ExpansionID"));
        out.u32(64, t->u32(r, "RaidOffset"));
        out.u32(65, t->u32(r, "MaxPlayers"));
        out.end();
      }
      if (registered)
      {
        LogDebug << "DB2 -> DBC: Map: registered " << registered << " WDT paths from WdtFileDataID" << std::endl;
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // WotLK Light.dbc positions are stored in "world" coordinates x 36: DBC X = (17066.67 - client y) * 36,
    // DBC Y = client z * 36, DBC Z = (17066.67 - client x) * 36 (verified: WotLK id 2 = (612096, 0, 998400)
    // vs modern GameCoords (-10666.7, 63.9, 0)). Modern GameCoords are plain client-space yards.
    bool buildLight(DBCFile& dbc)
    {
      auto t = loadTable("Light");
      if (!t->ok) return false;
      constexpr float MAP_HALF = 17066.66656f;
      constexpr float SKYMUL = 36.0f;
      DbcBuilder out(15);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        float const cx = t->f32(r, "GameCoords", 0);
        float const cy = t->f32(r, "GameCoords", 1);
        float const cz = t->f32(r, "GameCoords", 2);
        bool const global = cx == 0.f && cy == 0.f && cz == 0.f;
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "ContinentID"));
        out.f32(2, global ? 0.f : (MAP_HALF - cy) * SKYMUL);
        out.f32(3, global ? 0.f : cz * SKYMUL);
        out.f32(4, global ? 0.f : (MAP_HALF - cx) * SKYMUL);
        out.f32(5, t->f32(r, "GameFalloffStart") * SKYMUL);
        out.f32(6, t->f32(r, "GameFalloffEnd") * SKYMUL);
        for (std::size_t k = 0; k < 8; ++k) out.u32(7 + k, t->u32(r, "LightParamsID", k));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // noggit's LightParamsDB indices (the 3.3.5a client record: ID, HighlightSky, LightSkyboxID, CloudTypeID,
    // Glow, water/ocean alphas) plus a trailing Flags column.
    bool buildLightParams(DBCFile& dbc)
    {
      auto t = loadTable("LightParams");
      if (!t->ok) return false;
      DbcBuilder out(10);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        out.u32(1, t->u32(r, "HighlightSky"));
        out.u32(2, t->u32(r, "LightSkyboxID"));
        out.u32(3, t->u32(r, "CloudTypeID"));
        out.f32(4, t->f32(r, "Glow"));
        out.f32(5, t->f32(r, "WaterShallowAlpha"));
        out.f32(6, t->f32(r, "WaterDeepAlpha"));
        out.f32(7, t->f32(r, "OceanShallowAlpha"));
        out.f32(8, t->f32(r, "OceanDeepAlpha"));
        out.u32(9, t->u32(r, "Flags"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ID, Name (a path when the listfile knows the skybox model, else "fdid:N"), Flags, + SkyboxFileDataID
    bool buildLightSkybox(DBCFile& dbc)
    {
      auto t = loadTable("LightSkybox");
      if (!t->ok) return false;
      DbcBuilder out(4);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const fdid = t->u32(r, "SkyboxFileDataID");
        std::string name = pathForFileDataId(fdid);
        if (name.empty())
        {
          name = t->str(r, "Name");
        }
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, name);
        out.u32(2, t->u32(r, "Flags"));
        out.u32(3, fdid);
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // LightData (one row per LightParamID + Time) -> LightIntBand / LightFloatBand (18 / 6 rows per param,
    // id = param * 18 - 17 + band, 16 time npc_item_slots each). Band order = noggit's SkyColorNames /
    // SkyFloatParamsNames, i.e. the 3.3.5a band semantics; modern columns are matched by NAME.
    struct LightDataRows
    {
      std::map<std::uint32_t, std::vector<std::size_t>> by_param; // sorted by Time
      std::shared_ptr<Table> table;
    };

    LightDataRows lightDataRows()
    {
      LightDataRows rows;
      rows.table = loadTable("LightData");
      if (!rows.table->ok) return rows;
      auto const& t = *rows.table;
      for (std::size_t r = 0; r < t.file.recordCount(); ++r)
      {
        std::uint32_t param = t.u32(r, "LightParamID");
        if (!param) param = t.file.parentId(r);
        if (!param) continue;
        rows.by_param[param].push_back(r);
      }
      for (auto& [param, list] : rows.by_param)
      {
        std::sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) { return t.u32(a, "Time") < t.u32(b, "Time"); });
        if (list.size() > 16)
        {
          // WotLK bands hold 16 keys; keep an even spread including both ends
          std::vector<std::size_t> kept;
          for (std::size_t k = 0; k < 16; ++k)
          {
            kept.push_back(list[k * (list.size() - 1) / 15]);
          }
          list = kept;
        }
      }
      return rows;
    }

    char const* const INT_BAND_COLUMNS[18] = {
      "DirectColor", "AmbientColor", "SkyTopColor", "SkyMiddleColor", "SkyBand1Color", "SkyBand2Color",
      "SkySmogColor", "SkyFogColor", "ShadowOpacity", "SunColor", "CloudSunColor", "CloudEmissiveColor",
      "CloudLayer1AmbientColor", "CloudLayer2AmbientColor", "OceanCloseColor", "OceanFarColor",
      "RiverCloseColor", "RiverFarColor",
    };
    // FOG_DISTANCE, FOG_MULTIPLIER, CELESTIAL_FLOW (no modern equivalent), CLOUD_DENSITY, unknown x2
    char const* const FLOAT_BAND_COLUMNS[6] = { "FogEnd", "FogScaler", nullptr, "CloudDensity", nullptr, nullptr };

    bool buildLightBands(DBCFile& dbc, bool floats)
    {
      auto rows = lightDataRows();
      if (!rows.table->ok) return false;
      auto const& t = *rows.table;
      DbcBuilder out(34);
      int const band_count = floats ? 6 : 18;
      for (auto const& [param, list] : rows.by_param)
      {
        for (int band = 0; band < band_count; ++band)
        {
          char const* column = floats ? FLOAT_BAND_COLUMNS[band] : INT_BAND_COLUMNS[band];
          out.begin();
          out.u32(0, param * band_count - (band_count - 1) + band);
          std::uint32_t entries = column && t.has(column) ? static_cast<std::uint32_t>(list.size()) : 0u;
          out.u32(1, entries);
          for (std::uint32_t k = 0; k < entries; ++k)
          {
            // 1.60.1 (Forever Beta, LightData layout 360DA016) stores 0x10000 | half-minutes in every Time
            // value (5,375 of 5,375 rows; the low 16 bits are the same 0..2870 schedule the 1.15.9 rows
            // carry). The WotLK band tables interpolate over 0..2880, so the raw value put every key past
            // the end of the day: white terrain, yellow sky. docs/client_re/42 sec 17.2.
            out.u32(2 + k, t.u32(list[k], "Time") & 0xFFFFu);
            if (floats)
            {
              out.f32(18 + k, t.f32(list[k], column));
            }
            else
            {
              out.u32(18 + k, t.u32(list[k], column) & 0x00FFFFFFu);
            }
          }
          out.end();
        }
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    bool buildLiquidType(DBCFile& dbc)
    {
      auto t = loadTable("LiquidType");
      if (!t->ok) return false;
      DbcBuilder out(45);
      int pbr_rows = 0;
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        std::uint32_t const flags = t->u32(r, "Flags");
        std::uint32_t type = t->u32(r, "SoundBank");
        std::string texture0 = t->str(r, "Texture", 0);
        // 1.60.1 PBR water (LiquidMaterial 130, the "PBRWater - ..." rows 1235..1343): every MH2O layer of
        // the Forever Beta continents uses them (1250 "Generic - Ocean" on 604 of 900 tiles; 1240/1288/
        // 1297/1325/1343 lakes, rivers, swamps, springs). Texture[0..5] are the foam/rim maps of the new
        // shader (LiquidTypeXTexture: six fdids, no %d frame sequence) and SoundBank is 0 on every row,
        // the ocean included. The WotLK schema only knows the four classic families, so translate:
        // Flags 0x400 -- set on exactly the two "Ocean" rows and on none of the other 22 PBR rows --
        // becomes Type 1 (ocean) with the ocean_h frames, everything else Type 0 (water) with the lake_a
        // frames. Before this every PBR id had no texture profile ("Turtle water: missing liquid profile
        // 1240") and the (id-1)%4 class heuristic drew lake 1240 as slime and spring 1343 as magma.
        // docs/client_re/42 sec 19.
        bool const pbr = t->u32(r, "MaterialID") == 130u;
        if (pbr)
        {
          type = (flags & 0x400u) ? 1u : 0u;
          texture0 = type == 1u ? "XTextures\\ocean\\ocean_h.%d.blp" : "XTextures\\river\\lake_a.%d.blp";
          ++pbr_rows;
        }
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, t->str(r, "Name"));
        out.u32(2, flags);
        out.u32(3, type);
        out.u32(4, t->u32(r, "SoundID"));
        out.u32(5, t->u32(r, "SpellID"));
        out.f32(6, t->f32(r, "MaxDarkenDepth"));
        out.f32(7, t->f32(r, "FogDarkenIntensity"));
        out.f32(8, t->f32(r, "AmbDarkenIntensity"));
        out.f32(9, t->f32(r, "DirDarkenIntensity"));
        out.u32(10, t->u32(r, "LightID"));
        out.f32(11, t->f32(r, "ParticleScale"));
        out.u32(12, t->u32(r, "ParticleMovement"));
        out.u32(13, t->u32(r, "ParticleTexSlots"));
        out.u32(14, t->u32(r, "MaterialID"));
        out.str(15, texture0);
        for (std::size_t k = 1; k < 6; ++k) out.str(15 + k, t->str(r, "Texture", k));
        // PBR rows carry Color[3]; the beta's new light params author no river/ocean bands, so the
        // water colour has to come from here. Slot 1 is the water body colour ("Fel Tainted - Lake"
        // 00ff13, "Test 0" ff001b), slot 2 the darker deep colour on 18 of 24 rows, slot 0 a pale tint
        // shared by unrelated rows (ccdbb3 x4, dcd7c6 x3). Hand (1, 2) to the two WotLK Color slots;
        // LiquidRender reads them for material-130 rows only. Classic rows keep (0, 1).
        out.u32(21, t->u32(r, "Color", pbr ? 1 : 0));
        out.u32(22, t->u32(r, "Color", pbr ? 2 : 1));
        for (std::size_t k = 0; k < 18; ++k) out.f32(23 + k, t->f32(r, "Float", k));
        for (std::size_t k = 0; k < 4; ++k) out.u32(41 + k, t->u32(r, "Int", k));
        out.end();
      }
      if (pbr_rows)
      {
        LogDebug << "DB2 -> DBC: LiquidType: " << pbr_rows << " PBR water rows (material 130) mapped to the classic ocean/water frames" << std::endl;
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    bool buildGroundEffectTexture(DBCFile& dbc)
    {
      auto t = loadTable("GroundEffectTexture");
      if (!t->ok) return false;
      DbcBuilder out(11);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        out.begin();
        out.u32(0, t->file.id(r));
        for (std::size_t k = 0; k < 4; ++k) out.u32(1 + k, t->u32(r, "DoodadID", k));
        for (std::size_t k = 0; k < 4; ++k) out.u32(5 + k, t->u32(r, "DoodadWeight", k));
        out.u32(9, t->u32(r, "Density"));
        out.u32(10, t->u32(r, "Sound"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    bool buildGroundEffectDoodad(DBCFile& dbc)
    {
      auto t = loadTable("GroundEffectDoodad");
      if (!t->ok) return false;
      DbcBuilder out(3);
      for (std::size_t r = 0; r < t->file.recordCount(); ++r)
      {
        // WotLK Doodadpath is a bare file name; MapChunk::detailDoodads prepends "world/nodxt/detail/"
        std::string path = pathForFileDataId(t->u32(r, "ModelFileID"));
        if (path.rfind("fdid:", 0) != 0)
        {
          path = BlizzardArchive::ClientData::normalizeFilenameInternal(path); // listfile paths come back in WoW form
        }
        static std::string const detail_root = "world/nodxt/detail/";
        if (path.rfind(detail_root, 0) == 0)
        {
          path = path.substr(detail_root.size());
        }
        else if (path.rfind("fdid:", 0) != 0)
        {
          auto const slash = path.find_last_of('/');
          if (slash != std::string::npos) path = path.substr(slash + 1);
        }
        out.begin();
        out.u32(0, t->file.id(r));
        out.str(1, path);
        out.u32(2, t->u32(r, "Flags"));
        out.end();
      }
      dbc.loadSynthesized(out.field_count, std::move(out.data), std::move(out.strings));
      return true;
    }

    // ---------------------------------------------------------------------------------------- liquid objects
    std::unordered_map<std::uint32_t, std::uint32_t> g_liquid_object_type;   // LiquidObject id -> LiquidTypeID
    std::unordered_map<std::uint32_t, int> g_liquid_type_lvf;                // LiquidType id -> LiquidMaterial.LVF
    bool g_liquid_object_loaded = false;

    void loadLiquidObjects()
    {
      if (g_liquid_object_loaded) return;
      g_liquid_object_loaded = true;

      auto objects = loadTable("LiquidObject");
      auto types = loadTable("LiquidType");
      auto materials = loadTable("LiquidMaterial");
      if (!objects->ok || !types->ok || !materials->ok)
      {
        LogError << "DB2 -> DBC: LiquidObject/LiquidType/LiquidMaterial unavailable; MH2O liquid-object layers use vertex format 0." << std::endl;
        return;
      }

      // LiquidMaterial (1.15.9 / 2.5.6 layout 98E5D7AA: ID, Flags, LVF -- the column is fetched by NAME):
      // material 1 = {Flags 1, LVF 0} (water: height + depth), material 2 = {Flags 0, LVF 1} (magma / slime:
      // height + uv). Measured against the ADT bytes: every LiquidObject-referencing river layer of
      // azeroth_30..33_47..50 carries 5 bytes per vertex (float height + u8 depth) = LVF 0.
      std::unordered_map<std::uint32_t, int> material_lvf;
      for (std::size_t r = 0; r < materials->file.recordCount(); ++r)
      {
        material_lvf[materials->file.id(r)] = materials->i32(r, "LVF");
      }
      for (std::size_t r = 0; r < types->file.recordCount(); ++r)
      {
        std::uint32_t const type_id = types->file.id(r);
        auto const found = material_lvf.find(types->u32(r, "MaterialID"));
        int lvf = found != material_lvf.end() ? found->second : 0;
        // Ocean (LiquidType 2) is depth-only regardless of its material: the same rule the client-derived
        // WoWViewerCpp reader applies (`if (liquidTypeId == 2) matLVF = 2`), and what the data holds --
        // azeroth_29_48 (Stormwind harbour): 156 ocean layers without vertex data + 81 with 1 byte per
        // vertex; the WotLK build of the same tile encodes them as LVF 2 (101 + 92 layers).
        if (type_id == 2) lvf = 2;
        g_liquid_type_lvf[type_id] = lvf;
      }
      for (std::size_t r = 0; r < objects->file.recordCount(); ++r)
      {
        g_liquid_object_type[objects->file.id(r)] = objects->u32(r, "LiquidTypeID");
      }
      LogDebug << "DB2 -> DBC: " << g_liquid_object_type.size() << " LiquidObject rows, " << g_liquid_type_lvf.size()
               << " LiquidType rows mapped to vertex formats (material 1 -> LVF " << (material_lvf.count(1) ? material_lvf[1] : -1)
               << ", material 2 -> LVF " << (material_lvf.count(2) ? material_lvf[2] : -1) << ")" << std::endl;
    }
  }

  void setClientBuild(std::string const& build)
  {
    g_client_build = build;
  }

  void reset()
  {
    std::lock_guard<std::mutex> const lock(g_mutex);
    g_tables.clear();
    g_choices_by_extra.clear(); g_elements_by_choice.clear(); g_custom_geosets.clear(); g_custom_materials.clear();
    g_layout_by_race_sex.clear(); g_texture_type_by_layout_target.clear(); g_customization_loaded = false;
    g_liquid_object_type.clear();
    g_liquid_type_lvf.clear();
    g_liquid_object_loaded = false;
    g_model_files_by_resource.clear();
    g_texture_files_by_material.clear();
    g_component_model.clear();
    g_component_texture.clear();
    g_item_variants_loaded = false;
  }

  bool fillDBC(DBCFile& dbc, std::string const& dbc_filename)
  {
    // "DBFilesClient\\Light.dbc" -> "light"
    std::string name = lower(dbc_filename);
    auto const slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto const dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    if (name == "areatable") return buildAreaTable(dbc);
    if (name == "map") return buildMap(dbc);
    if (name == "light") return buildLight(dbc);
    if (name == "lightparams") return buildLightParams(dbc);
    if (name == "lightskybox") return buildLightSkybox(dbc);
    if (name == "lightintband") return buildLightBands(dbc, false);
    if (name == "lightfloatband") return buildLightBands(dbc, true);
    if (name == "liquidtype") return buildLiquidType(dbc);
    if (name == "groundeffecttexture") return buildGroundEffectTexture(dbc);
    if (name == "groundeffectdoodad") return buildGroundEffectDoodad(dbc);
    if (name == "creaturemodeldata") return buildCreatureModelData(dbc);
    if (name == "creaturedisplayinfo") return buildCreatureDisplayInfo(dbc);
    if (name == "creaturedisplayinfoextra") return buildCreatureDisplayInfoExtra(dbc);
    if (name == "gameobjectdisplayinfo") return buildGameObjectDisplayInfo(dbc);
    if (name == "itemdisplayinfo") return buildItemDisplayInfo(dbc);
    if (name == "charhairgeosets") return buildCharHairGeosets(dbc);
    if (name == "characterfacialhairstyles") return buildCharacterFacialHairStyles(dbc);
    if (name == "particlecolor") return buildParticleColor(dbc);
    if (name == "helmetgeosetvisdata") return buildHelmetGeosetVisData(dbc);
    if (name == "soundentries") return buildSoundEntries(dbc);
    if (name == "zonemusic") return buildZoneMusic(dbc);
    if (name == "zoneintromusictable") return buildZoneIntroMusicTable(dbc);
    if (name == "soundambience") return buildSoundAmbience(dbc);
    if (name == "soundproviderpreferences") return buildSoundProviderPreferences(dbc);
    if (name == "soundwatertype") return buildSoundWaterType(dbc);
    if (name == "footstepterrainlookup") return buildFootstepTerrainLookup(dbc);
    if (name == "terraintype") return buildTerrainType(dbc);
    if (name == "creaturesounddata") return buildCreatureSoundData(dbc);
    if (name == "wmoareatable") return buildWMOAreaTable(dbc);
    if (name == "loadingscreens") return buildLoadingScreens(dbc);
    return false;
  }

  CreatureCustomization creatureCustomization(std::uint32_t creature_display_info_extra_id, std::uint32_t race, std::uint32_t sex)
  {
    CreatureCustomization out;
    std::lock_guard<std::mutex> const lock(g_customization_mutex);
    loadCustomization();
    auto const chosen_it = g_choices_by_extra.find(creature_display_info_extra_id);
    if (chosen_it == g_choices_by_extra.end())
    {
      return out;
    }
    out.has_options = true;
    std::unordered_set<std::uint32_t> const chosen(chosen_it->second.begin(), chosen_it->second.end());
    auto const layout_it = g_layout_by_race_sex.find(race * 2 + (sex & 1));
    std::uint32_t const layout = layout_it == g_layout_by_race_sex.end() ? 1u : layout_it->second;
    std::unordered_set<std::uint32_t> geoset_types_done;
    for (std::uint32_t const choice : chosen_it->second)
    {
      auto const elements = g_elements_by_choice.find(choice);
      if (elements == g_elements_by_choice.end()) continue;
      for (auto const& e : elements->second)
      {
        if (e.related && !chosen.count(e.related)) continue;
        if (e.geoset)
        {
          auto const g = g_custom_geosets.find(e.geoset);
          // First element per geoset type wins: troll male style choice 17829 lists geoset 0/1 then 0/0, and
          // CharHairGeosets (the WotLK-era source it was converted from) resolves that variation to geoset 1.
          if (g != g_custom_geosets.end() && geoset_types_done.insert(g->second.first).second)
          {
            out.geosets.emplace_back(g->second.first, g->second.second);
          }
        }
        if (e.material)
        {
          auto const m = g_custom_materials.find(e.material);
          if (m == g_custom_materials.end()) continue;
          auto const type = g_texture_type_by_layout_target.find((static_cast<std::uint64_t>(layout) << 32) | m->second.first);
          if (type == g_texture_type_by_layout_target.end()) continue;
          std::string path = itemTexturePathForVariant(m->second.second, sex);
          if (!path.empty())
          {
            out.textures.push_back({type->second, m->second.first, std::move(path)});
          }
        }
      }
    }
    return out;
  }

  int liquidObjectVertexFormat(std::uint32_t liquid_object_id, std::uint32_t liquid_type)
  {
    static std::mutex liquid_mutex; // tiles load on several threads
    std::lock_guard<std::mutex> const lock(liquid_mutex);
    loadLiquidObjects();

    // LiquidObject id -> LiquidTypeID. Id 42 is the ocean object every converted ocean layer references
    // (WotLK LVF-2 layers became `liquid_object_or_lvf = 42`); the Classic tables start at 5921, so the
    // row is missing and the instance's own liquid type (2) stands in -- exactly what WoWViewerCpp does
    // (`liquidObjectId = value > 41 ? value : liquid_type`, then `getLiquidObjectData(value, liquid_type)`).
    std::uint32_t type = liquid_type;
    auto const object = g_liquid_object_type.find(liquid_object_id);
    if (object != g_liquid_object_type.end())
    {
      type = object->second;
    }
    else
    {
      static std::unordered_map<std::uint32_t, unsigned> s_missing;
      if (s_missing[liquid_object_id]++ == 0)
      {
        LogDebug << "MH2O: LiquidObject " << liquid_object_id << " has no row in this client; using the layer's liquid type "
                 << liquid_type << " for its vertex format" << std::endl;
      }
    }
    auto const lvf = g_liquid_type_lvf.find(type);
    return lvf == g_liquid_type_lvf.end() ? (type == 2 ? 2 : 0) : lvf->second;
  }

  std::string itemModelPathForVariant(std::uint32_t model_resources_id, std::uint32_t race, std::uint32_t sex, int position)
  {
    static std::mutex variant_mutex;
    std::lock_guard<std::mutex> const lock(variant_mutex);
    loadItemVariants();
    auto const files = g_model_files_by_resource.find(model_resources_id);
    if (files == g_model_files_by_resource.end() || files->second.empty())
    {
      return {};
    }
    // Score every candidate: side first (when the resource distinguishes sides at all), then the wearer.
    // Class-specific files (ClassID != 0) only win for a matching class, which NPC displays never name.
    bool sided = false;
    for (std::uint32_t f : files->second)
    {
      auto const it = g_component_model.find(f);
      if (it != g_component_model.end() && it->second.position != 255) sided = true;
    }
    std::uint32_t best = files->second.front();
    int best_score = -1;
    for (std::uint32_t f : files->second)
    {
      auto const it = g_component_model.find(f);
      ComponentInfo const info = it != g_component_model.end() ? it->second : ComponentInfo();
      int score = 0;
      if (sided && position >= 0)
      {
        if (info.position == static_cast<std::uint8_t>(position)) score += 1000;
        else if (info.position != 255) continue; // the other side
      }
      if (info.cls != 0) continue;
      if (!info.known) score += 1;                       // generic file
      else
      {
        if (info.race == race) score += 100;
        else if (info.race == 0) score += 40;
        if (info.gender == sex) score += 10;
        else if (info.gender == 2) score += 4;
      }
      if (score > best_score) { best_score = score; best = f; }
    }
    return pathForFileDataID(best, "m2");
  }

  std::string itemTexturePathForVariant(std::uint32_t material_resources_id, std::uint32_t sex)
  {
    static std::mutex variant_mutex;
    std::lock_guard<std::mutex> const lock(variant_mutex);
    loadItemVariants();
    auto const files = g_texture_files_by_material.find(material_resources_id);
    if (files == g_texture_files_by_material.end() || files->second.empty())
    {
      return {};
    }
    std::uint32_t best = files->second.front();
    int best_score = -1;
    for (std::uint32_t f : files->second)
    {
      auto const it = g_component_texture.find(f);
      ComponentInfo const info = it != g_component_texture.end() ? it->second : ComponentInfo();
      int score = 0;
      if (info.cls != 0) continue;
      if (!info.known) score += 1;
      else if (info.gender == sex) score += 10;
      else if (info.gender == 3 || info.gender == 2) score += 4; // "any gender" rows are 3 in ComponentTextureFileData
      if (score > best_score) { best_score = score; best = f; }
    }
    return pathForFileDataID(best, "blp");
  }

  std::string resolveItemVariantName(std::string const& name, std::uint32_t race, std::uint32_t sex)
  {
    if (name.rfind("modelres:", 0) == 0)
    {
      std::uint32_t const id = static_cast<std::uint32_t>(std::strtoul(name.c_str() + 9, nullptr, 10));
      auto const colon = name.find(':', 9);
      int const position = colon == std::string::npos ? -1 : std::atoi(name.c_str() + colon + 1);
      return itemModelPathForVariant(id, race, sex, position);
    }
    if (name.rfind("matres:", 0) == 0)
    {
      return itemTexturePathForVariant(static_cast<std::uint32_t>(std::strtoul(name.c_str() + 7, nullptr, 10)), sex);
    }
    return name;
  }
}
