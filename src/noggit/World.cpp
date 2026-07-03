// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/World.h>
#include <noggit/World.inl>

#include <math/frustum.hpp>
#include <noggit/Brush.h> // brush
#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h>
#include <noggit/WMOInstance.h> // WMOInstance
#include <noggit/texture_set.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/ObjectEditor.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ActionManager.hpp>
#include <external/tracy/Tracy.hpp>
#include <ClientFile.hpp>
#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>
#endif
#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cassert>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <limits>
#include <array>
#include <cstdint>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#ifdef USE_MYSQL_UID_STORAGE
namespace mysql
{
  std::vector<CreatureSpawnRecord> getCreatureSpawns(std::size_t mapID, std::string* error);
  std::vector<GameObjectSpawnRecord> getGameObjectSpawns(std::size_t mapID, std::string* error);
}
#endif

namespace
{
  constexpr char const* creature_geoset_trace_filename = "creature_geoset_trace_20260604.log";
  constexpr std::size_t max_logged_creature_model_failures = 8;
  constexpr bool enable_creature_spawn_character_geosets = true;

  std::string normalize_model_filename(std::string filename);
  std::string normalize_texture_filename(std::string filename);

  struct ClientFileHeaderProbe
  {
    bool opened = false;
    std::size_t size = 0;
    std::string magic;
  };

  enum class CharacterTextureRegion
  {
    ArmUpper,
    ArmLower,
    Hand,
    FaceUpper,
    FaceLower,
    TorsoUpper,
    TorsoLower,
    LegUpper,
    LegLower,
    Foot
  };

  struct CharacterTextureRegionRect
  {
    int x;
    int y;
    int width;
    int height;
  };

  constexpr int legacy_character_texture_layout_width = 512;
  constexpr int legacy_character_texture_layout_height = 512;

  CharacterTextureRegionRect legacy_character_texture_rect(CharacterTextureRegion region)
  {
    switch (region)
    {
      case CharacterTextureRegion::ArmUpper:
        return {0, 0, 256, 128};
      case CharacterTextureRegion::ArmLower:
        return {0, 128, 256, 128};
      case CharacterTextureRegion::Hand:
        return {0, 256, 256, 64};
      case CharacterTextureRegion::FaceUpper:
        return {0, 320, 256, 64};
      case CharacterTextureRegion::FaceLower:
        return {0, 384, 256, 128};
      case CharacterTextureRegion::TorsoUpper:
        return {256, 0, 256, 128};
      case CharacterTextureRegion::TorsoLower:
        return {256, 128, 256, 64};
      case CharacterTextureRegion::LegUpper:
        return {256, 192, 256, 128};
      case CharacterTextureRegion::LegLower:
        return {256, 320, 256, 128};
      case CharacterTextureRegion::Foot:
        return {256, 448, 256, 64};
    }

    return {0, 0, 0, 0};
  }

  std::string resolve_item_texture_component_filename(std::string texture_name, CharacterTextureRegion region)
  {
    if (texture_name.empty())
    {
      return {};
    }

    texture_name = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(texture_name));
    if (texture_name.find('/') != std::string::npos)
    {
      return normalize_texture_filename(std::move(texture_name));
    }

    char const* prefix = nullptr;
    switch (region)
    {
      case CharacterTextureRegion::ArmUpper:
        prefix = "item/texturecomponents/armuppertexture/";
        break;
      case CharacterTextureRegion::ArmLower:
        prefix = "item/texturecomponents/armlowertexture/";
        break;
      case CharacterTextureRegion::Hand:
        prefix = "item/texturecomponents/handtexture/";
        break;
      case CharacterTextureRegion::TorsoUpper:
        prefix = "item/texturecomponents/torsouppertexture/";
        break;
      case CharacterTextureRegion::TorsoLower:
        prefix = "item/texturecomponents/torsolowertexture/";
        break;
      case CharacterTextureRegion::LegUpper:
        prefix = "item/texturecomponents/leguppertexture/";
        break;
      case CharacterTextureRegion::LegLower:
        prefix = "item/texturecomponents/leglowertexture/";
        break;
      case CharacterTextureRegion::Foot:
        prefix = "item/texturecomponents/foottexture/";
        break;
    }

    if (!prefix)
    {
      return {};
    }

    return normalize_texture_filename(std::string(prefix) + texture_name);
  }

  bool load_client_texture_image(std::string const& filename,
                                 QImage& image,
                                 std::string* loaded_filename = nullptr,
                                 char const** load_method = nullptr)
  {
    if (filename.empty())
    {
      return false;
    }

    auto const try_load = [&](std::string const& candidate)
    {
      bool const is_baked_texture = candidate.find("textures/bakednpctextures/") != std::string::npos;
      bool const is_generated_texture = candidate.find("textures/generated/") != std::string::npos;
      auto* client = Noggit::Application::NoggitApplication::instance()->clientData();

      if (!is_generated_texture && !client->exists(candidate))
      {
        return false;
      }

      try
      {
        scoped_blp_texture_reference texture(candidate, Noggit::NoggitRenderContext::MAP_VIEW);
        texture->finishLoading();

        auto const mip = texture->data().find(0);
        if (mip == texture->data().end() || mip->second.empty() || texture->width() <= 0 || texture->height() <= 0)
        {
          if (is_baked_texture)
          {
            LogDebug << "baked texture raw path unavailable: '" << candidate
                     << "' hasMip=" << (mip != texture->data().end())
                     << " mipEmpty=" << (mip == texture->data().end() ? 1 : mip->second.empty())
                     << " width=" << texture->width()
                     << " height=" << texture->height()
                     << std::endl;
          }

          if (is_baked_texture || is_generated_texture)
          {
            return false;
          }
        }

        if (mip != texture->data().end() && !mip->second.empty() && texture->width() > 0 && texture->height() > 0)
        {
          QImage decoded(reinterpret_cast<unsigned char const*>(mip->second.data()),
                         texture->width(),
                         texture->height(),
                         QImage::Format_RGBA8888);
          image = decoded.copy();
          if (!image.isNull())
          {
            if (loaded_filename)
            {
              *loaded_filename = candidate;
            }
            if (load_method)
            {
              *load_method = "raw";
            }
          }
          if (!image.isNull())
          {
            return true;
          }
        }
      }
      catch (std::exception const& error)
      {
        if (is_baked_texture)
        {
          LogDebug << "baked texture raw decode failed: '" << candidate << "' error='" << error.what() << "'" << std::endl;
        }
        if (is_baked_texture || is_generated_texture)
        {
          return false;
        }
      }
      catch (...)
      {
        if (is_baked_texture)
        {
          LogDebug << "baked texture raw decode failed: '" << candidate << "' error='<unknown>'" << std::endl;
        }
        if (is_baked_texture || is_generated_texture)
        {
          return false;
        }
      }

      try
      {
        QPixmap const* rendered = Noggit::BLPRenderer::getInstance().render_blp_to_pixmap(candidate, -1, -1);
        if (!rendered || rendered->isNull())
        {
          if (is_baked_texture)
          {
            LogDebug << "baked texture renderer returned null pixmap: '" << candidate << "'" << std::endl;
          }
          return false;
        }

        image = rendered->toImage().convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull())
        {
          if (is_baked_texture)
          {
            LogDebug << "baked texture renderer produced null image: '" << candidate << "'" << std::endl;
          }
          return false;
        }
        if (!image.isNull())
        {
          if (loaded_filename)
          {
            *loaded_filename = candidate;
          }
          if (load_method)
          {
            *load_method = "renderer";
          }
        }
        return !image.isNull();
      }
      catch (std::exception const& error)
      {
        if (is_baked_texture)
        {
          LogDebug << "baked texture renderer decode failed: '" << candidate << "' error='" << error.what() << "'" << std::endl;
        }
        return false;
      }
      catch (...)
      {
        if (is_baked_texture)
        {
          LogDebug << "baked texture renderer decode failed: '" << candidate << "' error='<unknown>'" << std::endl;
        }
        return false;
      }
    };

    if (try_load(filename))
    {
      return true;
    }

    bool const is_texture_component = filename.find("item/texturecomponents/") != std::string::npos;
    bool const already_gendered = filename.ends_with("_u.blp") || filename.ends_with("_m.blp") || filename.ends_with("_f.blp");
    if (!is_texture_component || already_gendered)
    {
      return false;
    }

    auto const extension_pos = filename.rfind(".blp");
    if (extension_pos == std::string::npos)
    {
      return false;
    }

    auto const stem = filename.substr(0, extension_pos);
    for (char const* suffix : {"_u.blp", "_m.blp", "_f.blp"})
    {
      if (try_load(stem + suffix))
      {
        return true;
      }
    }

    return false;
  }

  void set_texture_override(std::vector<std::pair<std::size_t, std::string>>& overrides,
                            std::size_t slot,
                            std::string const& texture)
  {
    if (texture.empty())
    {
      return;
    }

    auto existing = std::find_if(overrides.begin(), overrides.end(),
      [slot](std::pair<std::size_t, std::string> const& override_entry)
      {
        return override_entry.first == slot;
      });

    if (existing != overrides.end())
    {
      existing->second = texture;
      return;
    }

    overrides.emplace_back(slot, texture);
  }

  struct CharacterTextureLayer
  {
    std::string texture;
    CharacterTextureRegion region;
  };

  enum class CharacterSectionType : std::uint32_t
  {
    Skin = 0,
    Face = 1,
    FacialHair = 2,
    Hair = 3,
    Underwear = 4
  };

  struct CharacterSectionTextures
  {
    std::array<std::string, 3> textures;
    bool found = false;
  };

  bool compose_character_body_texture(std::uint32_t display_id,
                                      std::string const& base_texture,
                                      std::vector<CharacterTextureLayer> const& layers,
                                      Noggit::NoggitRenderContext context,
                                      std::string& composed_texture,
                                      std::ostringstream* debug_details)
  {
    if (!display_id || base_texture.empty() || layers.empty())
    {
      return false;
    }

    try
    {
      QImage body_image;
      if (!load_client_texture_image(base_texture, body_image))
      {
        if (debug_details)
        {
          *debug_details << " legsTextureBaseLoadFailed=1";
        }
        return false;
      }

      body_image = body_image.convertToFormat(QImage::Format_RGBA8888);
      bool applied_any_layer = false;

      auto const paint_region = [&](std::string const& texture_filename, CharacterTextureRegion region)
      {
        if (texture_filename.empty())
        {
          return;
        }

        QImage overlay_image;
        std::string loaded_overlay_filename;
        char const* overlay_load_method = nullptr;
        if (!load_client_texture_image(texture_filename, overlay_image, &loaded_overlay_filename, &overlay_load_method))
        {
          if (debug_details)
          {
            *debug_details << " overlayLoadFailed='" << texture_filename << "'";
          }
          return;
        }

        if (debug_details && !loaded_overlay_filename.empty() && loaded_overlay_filename != texture_filename)
        {
          *debug_details << " overlayLoaded='" << texture_filename << "'->'" << loaded_overlay_filename << "'";
          if (overlay_load_method)
          {
            *debug_details << " overlayLoadedVia='" << overlay_load_method << "'";
          }
        }
        else if (debug_details && overlay_load_method)
        {
          *debug_details << " overlayLoadedVia='" << overlay_load_method << "'";
        }

        auto const layout_rect = legacy_character_texture_rect(region);
        QRect const target_rect(
          layout_rect.x * body_image.width() / legacy_character_texture_layout_width,
          layout_rect.y * body_image.height() / legacy_character_texture_layout_height,
          std::max(1, layout_rect.width * body_image.width() / legacy_character_texture_layout_width),
          std::max(1, layout_rect.height * body_image.height() / legacy_character_texture_layout_height));

        QPainter painter(&body_image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawImage(target_rect, overlay_image.convertToFormat(QImage::Format_RGBA8888));
        painter.end();
        applied_any_layer = true;
      };

      for (auto const& layer : layers)
      {
        paint_region(layer.texture, layer.region);
      }

      if (!applied_any_layer)
      {
        return false;
      }

      std::vector<std::uint32_t> raw_pixels(static_cast<std::size_t>(body_image.width()) * static_cast<std::size_t>(body_image.height()));
      auto const* source_pixels = reinterpret_cast<std::uint32_t const*>(body_image.constBits());
      std::copy(source_pixels, source_pixels + raw_pixels.size(), raw_pixels.begin());

      composed_texture = normalize_texture_filename(
        "textures/generated/creaturedisplay-" + std::to_string(display_id) + "-body-composed.blp");
      TextureManager::register_raw_texture(composed_texture,
                                           context,
                                           body_image.width(),
                                           body_image.height(),
                                           std::move(raw_pixels));

      if (debug_details)
      {
        *debug_details << " bodyTextureComposed='" << composed_texture
                       << "' layerCount=" << layers.size();
      }

      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  bool creature_texture_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* model_texture = std::getenv("NOGGIT_MODEL_TEXTURE_DEBUG");
      if (model_texture && *model_texture && std::strcmp(model_texture, "0") != 0)
      {
        return true;
      }

      char const* classic_debug = std::getenv("NOGGIT_CLASSIC_M2_DEBUG");
      return classic_debug && *classic_debug && std::strcmp(classic_debug, "0") != 0;
    }();

    return enabled;
  }

  bool classic_pants_composite_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_PANTS_COMPOSITE");
      if (!value || !*value)
      {
        return true;
      }

      return std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool classic_probe_hide_trousers_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_HIDE_TROUSERS");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool classic_probe_hide_chest_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_HIDE_CHEST");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool classic_probe_disable_baked_npc_textures_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_DISABLE_BAKED_NPC_TEXTURES");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool classic_disable_shoulder_attachments_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_DISABLE_SHOULDERS");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool creature_spawn_attachments_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CREATURE_ATTACHMENTS");
      return !value || !*value || std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool creature_spawn_geosets_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CREATURE_GEOSETS");
      return !value || !*value || std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  std::string format_texture_override_list(std::vector<std::pair<std::size_t, std::string>> const& overrides)
  {
    std::ostringstream stream;
    for (std::size_t index = 0; index < overrides.size(); ++index)
    {
      if (index != 0)
      {
        stream << ", ";
      }

      stream << overrides[index].first << "='" << overrides[index].second << "'";
    }

    return stream.str();
  }

  enum class CreatureModelPathStatus
  {
    Success,
    MissingDisplayId,
    MissingDisplayInfo,
    MissingModelInfo,
    EmptyModelName
  };

  struct CreatureModelPathResult
  {
    CreatureModelPathStatus status = CreatureModelPathStatus::MissingDisplayId;
    std::string path;
  };

  enum class CharacterGeosetFamily : std::uint16_t
  {
    SkinOrHairStyle = 0,
    Geoset100 = 1,
    Geoset200 = 2,
    Geoset300 = 3,
    Gloves = 4,
    Boots = 5,
    Tail = 6,
    Ears = 7,
    Wristbands = 8,
    Kneepads = 9,
    Chest = 10,
    Pants = 11,
    Tabard = 12,
    Trousers = 13,
    Cape = 15,
    EyeGlow = 17,
    Belt = 18,
    Feet = 20,
    Torso = 22
  };

  enum class CharacterAttachmentId : int
  {
    LeftWrist = 0,
    RightPalm = 1,
    LeftPalm = 2,
    RightShoulder = 5,
    LeftShoulder = 6,
    Helmet = 11,
    RightBackSheath = 26
  };

  std::string normalize_texture_filename(std::string filename);

  struct CreatureAttachmentModelSpec
  {
    int attachment_id = -1;
    std::string model_path;
    std::vector<std::pair<std::size_t, std::string>> texture_overrides;
  };

  struct CreatureGeosetSelection
  {
    std::vector<std::uint16_t> visible_ids;
    std::vector<std::uint16_t> controlled_families;
    std::string debug_summary;

    [[nodiscard]] bool empty() const
    {
      return visible_ids.empty() && controlled_families.empty();
    }
  };

  std::uint16_t geoset_family_of(std::uint16_t geoset_id)
  {
    return static_cast<std::uint16_t>(geoset_id / 100);
  }

  std::uint16_t make_geoset_id(CharacterGeosetFamily family, std::uint32_t flags, bool relative = true)
  {
    auto const relative_offset = relative ? 1u : 0u;
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(family) * 100u + flags + relative_offset);
  }

  void assign_geoset_selection(CreatureGeosetSelection& selection,
                               CharacterGeosetFamily family,
                               std::uint32_t flags,
                               bool relative = true)
  {
    auto const family_id = static_cast<std::uint16_t>(family);
    auto const geoset_id = make_geoset_id(family, flags, relative);

    selection.controlled_families.erase(
      std::remove(selection.controlled_families.begin(), selection.controlled_families.end(), family_id),
      selection.controlled_families.end());
    selection.controlled_families.push_back(family_id);

    selection.visible_ids.erase(
      std::remove_if(selection.visible_ids.begin(), selection.visible_ids.end(),
        [family_id](std::uint16_t existing_id)
        {
          return geoset_family_of(existing_id) == family_id;
        }),
      selection.visible_ids.end());
    selection.visible_ids.push_back(geoset_id);
  }

  void add_geoset_visible_id(CreatureGeosetSelection& selection,
                             std::uint16_t geoset_id)
  {
    auto const family_id = geoset_family_of(geoset_id);

    if (std::find(selection.controlled_families.begin(), selection.controlled_families.end(), family_id)
        == selection.controlled_families.end())
    {
      selection.controlled_families.push_back(family_id);
    }

    if (std::find(selection.visible_ids.begin(), selection.visible_ids.end(), geoset_id)
        == selection.visible_ids.end())
    {
      selection.visible_ids.push_back(geoset_id);
    }
  }

  void hide_geoset_family(CreatureGeosetSelection& selection,
                          CharacterGeosetFamily family)
  {
    auto const family_id = static_cast<std::uint16_t>(family);

    selection.controlled_families.erase(
      std::remove(selection.controlled_families.begin(), selection.controlled_families.end(), family_id),
      selection.controlled_families.end());
    selection.controlled_families.push_back(family_id);

    selection.visible_ids.erase(
      std::remove_if(selection.visible_ids.begin(), selection.visible_ids.end(),
        [family_id](std::uint16_t existing_id)
        {
          return geoset_family_of(existing_id) == family_id;
        }),
      selection.visible_ids.end());
  }

  void assign_geoset_selection_if_nonzero(CreatureGeosetSelection& selection,
                                          CharacterGeosetFamily family,
                                          std::uint32_t flags,
                                          bool relative = true)
  {
    if (!flags)
    {
      return;
    }

    assign_geoset_selection(selection, family, flags, relative);
  }

  void append_item_geoset_debug(std::ostringstream& debug,
                                char const* slot_name,
                                std::uint32_t item_display_id,
                                DBCFile::Record const& item_display)
  {
    debug << " " << slot_name << "Item=" << item_display_id
          << " geoset=["
          << item_display.getUInt(ItemDisplayInfoDB::GeosetGroup1) << ","
          << item_display.getUInt(ItemDisplayInfoDB::GeosetGroup2) << ","
          << item_display.getUInt(ItemDisplayInfoDB::GeosetGroup3) << "]"
          << " helmetVis=["
          << item_display.getUInt(ItemDisplayInfoDB::HelmetGeosetVis1) << ","
          << item_display.getUInt(ItemDisplayInfoDB::HelmetGeosetVis2) << "]"
          << " model1='" << item_display.getString(ItemDisplayInfoDB::ModelName1) << "'"
              << " model2='" << item_display.getString(ItemDisplayInfoDB::ModelName2) << "'";

            auto const upper_arm_texture = item_display.getString(ItemDisplayInfoDB::TextureUpperArm);
            auto const lower_arm_texture = item_display.getString(ItemDisplayInfoDB::TextureLowerArm);
            auto const hands_texture = item_display.getString(ItemDisplayInfoDB::TextureHands);
            auto const upper_chest_texture = item_display.getString(ItemDisplayInfoDB::TextureUpperChest);
            auto const lower_chest_texture = item_display.getString(ItemDisplayInfoDB::TextureLowerChest);
            auto const upper_leg_texture = item_display.getString(ItemDisplayInfoDB::TextureUpperLeg);
            auto const lower_leg_texture = item_display.getString(ItemDisplayInfoDB::TextureLowerLeg);

            if (*upper_arm_texture || *lower_arm_texture || *hands_texture)
            {
          debug << " armTex=['" << upper_arm_texture
            << "','" << lower_arm_texture
            << "','" << hands_texture << "']";
            }

            if (*upper_chest_texture || *lower_chest_texture)
            {
          debug << " chestTex=['" << upper_chest_texture
            << "','" << lower_chest_texture << "']";
            }

            if (*upper_leg_texture || *lower_leg_texture)
            {
          debug << " legTex=['" << upper_leg_texture
            << "','" << lower_leg_texture << "']";
            }
  }

  void append_item_display_debug_if_present(std::ostringstream& debug,
                                            char const* slot_name,
                                            std::uint32_t item_display_id)
  {
    if (!item_display_id)
    {
      return;
    }

    try
    {
      auto item_display = gItemDisplayInfoDB.getByID(item_display_id);
      append_item_geoset_debug(debug, slot_name, item_display_id, item_display);
    }
    catch (DBCFile::NotFound const&)
    {
      debug << " " << slot_name << "Item=" << item_display_id << " missing=1";
    }
  }

  void append_uint16_list_debug(std::ostringstream& debug,
                                char const* label,
                                std::vector<std::uint16_t> const& values)
  {
    debug << " " << label << "=[";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
      if (index != 0)
      {
        debug << ", ";
      }
      debug << values[index];
    }
    debug << "]";
  }

  void append_creature_geoset_trace(char const* category, std::string const& message)
  {
    if (!creature_texture_debug_enabled())
    {
      return;
    }

    std::ofstream trace(creature_geoset_trace_filename, std::ios::app);
    if (!trace.is_open())
    {
      return;
    }

    trace << category << ' ' << message << '\n';
  }

  struct ClassicFacialHairGeosets
  {
    std::uint32_t beard = 0;
    std::uint32_t moustache = 0;
    std::uint32_t sideburn = 0;
  };

  std::optional<ClassicFacialHairGeosets> resolve_classic_facial_hair_geosets(std::uint32_t race_id,
                                                                              std::uint32_t sex_id,
                                                                              std::uint32_t variation_id)
  {
    auto const sane_selector = [](std::uint32_t value)
    {
      return value <= 50;
    };

    for (auto it = gCharacterFacialHairStylesDB.begin(); it != gCharacterFacialHairStylesDB.end(); ++it)
    {
      if (it->getUInt(CharacterFacialHairStylesDB::RaceID) != race_id
          || it->getUInt(CharacterFacialHairStylesDB::SexID) != sex_id
          || it->getUInt(CharacterFacialHairStylesDB::VariationID) != variation_id)
      {
        continue;
      }

      auto const beard = it->getUInt(CharacterFacialHairStylesDB::BeardGeoset);
      auto const moustache = it->getUInt(CharacterFacialHairStylesDB::MoustacheGeoset);
      auto const sideburn = it->getUInt(CharacterFacialHairStylesDB::SideburnGeoset);

      // Guard against malformed/unsupported classic DBC layouts producing
      // garbage selectors that would hide the whole head family.
      if (!sane_selector(beard) || !sane_selector(moustache) || !sane_selector(sideburn))
      {
        return std::nullopt;
      }

      return ClassicFacialHairGeosets{beard, moustache, sideburn};
    }

    return std::nullopt;
  }

  std::optional<std::uint32_t> resolve_classic_hair_geoset(std::uint32_t race_id,
                                                           std::uint32_t sex_id,
                                                           std::uint32_t variation_id)
  {
    for (auto it = gCharacterHairGeosetsDB.begin(); it != gCharacterHairGeosetsDB.end(); ++it)
    {
      if (it->getUInt(CharacterHairGeosetsDB::RaceID) != race_id
          || it->getUInt(CharacterHairGeosetsDB::SexID) != sex_id
          || it->getUInt(CharacterHairGeosetsDB::VariationID) != variation_id)
      {
        continue;
      }

      auto const geoset_id = it->getUInt(CharacterHairGeosetsDB::GeosetID);
      if (geoset_id > 99)
      {
        return std::nullopt;
      }

      return std::max<std::uint32_t>(1u, geoset_id);
    }

    return std::nullopt;
  }

  void apply_character_default_geosets(CreatureGeosetSelection& selection)
  {
    // Match WMVx ModelDefaultsGeosetModifier: geoset 0 plus each family's
    // xx1 default stays visible until a later customization/equipment modifier
    // picks a sibling in that family.
    assign_geoset_selection(selection, CharacterGeosetFamily::SkinOrHairStyle, 0, false);
    assign_geoset_selection(selection, CharacterGeosetFamily::Geoset100, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Geoset200, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Geoset300, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Gloves, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Boots, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Tail, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Ears, 2, false);
    assign_geoset_selection(selection, CharacterGeosetFamily::Wristbands, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Kneepads, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Chest, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Pants, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Tabard, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Trousers, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Cape, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Belt, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::EyeGlow, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Feet, 0);
    assign_geoset_selection(selection, CharacterGeosetFamily::Torso, 0);
  }

  std::string attachment_model_variant_suffix(std::uint32_t race_id, std::uint32_t sex_id)
  {
    bool const female = sex_id != 0;

    switch (race_id)
    {
      case 1:  return female ? "huf" : "hum";
      case 2:  return female ? "orf" : "orm";
      case 3:  return female ? "dwf" : "dwm";
      case 4:  return female ? "nif" : "nim";
      case 5:  return female ? "scf" : "scm";
      case 6:  return female ? "taf" : "tam";
      case 7:  return female ? "gnf" : "gnm";
      case 8:  return female ? "trf" : "trm";
      case 9:  return female ? "gof" : "gom";
      case 10: return female ? "bef" : "bem";
      case 11: return female ? "drf" : "drm";
      default: return {};
    }
  }

  std::string resolve_item_attachment_model_path(char const* component_dir,
                                                 std::string model_name,
                                                 std::uint32_t race_id,
                                                 std::uint32_t sex_id)
  {
    model_name = normalize_model_filename(std::move(model_name));
    if (model_name.empty())
    {
      return {};
    }

    auto* client = Noggit::Application::NoggitApplication::instance()->clientData();

    if (model_name.find('/') != std::string::npos)
    {
      return model_name;
    }

    auto candidate = normalize_model_filename(std::string("item/objectcomponents/") + component_dir + "/" + model_name);
    if (client->exists(candidate))
    {
      return candidate;
    }

    auto const suffix = attachment_model_variant_suffix(race_id, sex_id);
    if (!suffix.empty())
    {
      auto suffix_pos = candidate.rfind(".m2");
      if (suffix_pos != std::string::npos)
      {
        auto variant = candidate;
        variant.insert(suffix_pos, "_" + suffix);
        if (client->exists(variant))
        {
          return variant;
        }
      }
    }

    return candidate;
  }

  void append_item_attachment_specs(std::vector<CreatureAttachmentModelSpec>& attachments,
                                    std::uint32_t item_display_id,
                                    char const* component_dir,
                                    int attachment_id_model1,
                                    int attachment_id_model2,
                                    std::uint32_t race_id,
                                    std::uint32_t sex_id)
  {
    if (!item_display_id)
    {
      return;
    }

    try
    {
      auto item_display = gItemDisplayInfoDB.getByID(item_display_id);

      auto resolve_texture_path = [&](std::string texture_name)
      {
        auto texture = normalize_texture_filename(std::move(texture_name));
        if (texture.empty())
        {
          return texture;
        }

        if (texture.find('/') == std::string::npos)
        {
          texture = normalize_texture_filename(std::string("item/objectcomponents/")
                                               + component_dir
                                               + "/"
                                               + texture);
        }

        return texture;
      };

      auto add_item_attachment = [&](int attachment_id, std::string model_name, std::string texture_name)
      {
        if (component_dir == std::string_view("shoulder"))
        {
          std::string lower_model_name = model_name;
          std::transform(lower_model_name.begin(), lower_model_name.end(), lower_model_name.begin(), [](unsigned char c)
          {
            return static_cast<char>(std::tolower(c));
          });

          if (lower_model_name.rfind("lshoulder_", 0) == 0)
          {
            attachment_id = static_cast<int>(CharacterAttachmentId::LeftShoulder);
          }
          else if (lower_model_name.rfind("rshoulder_", 0) == 0)
          {
            attachment_id = static_cast<int>(CharacterAttachmentId::RightShoulder);
          }
        }

        auto model_path = resolve_item_attachment_model_path(component_dir,
                                                             std::move(model_name),
                                                             race_id,
                                                             sex_id);
        if (model_path.empty())
        {
          return;
        }

        CreatureAttachmentModelSpec spec;
        spec.attachment_id = attachment_id;
        spec.model_path = std::move(model_path);

        auto texture_path = resolve_texture_path(std::move(texture_name));
        if (!texture_path.empty())
        {
          spec.texture_overrides.emplace_back(2u, std::move(texture_path));
        }

        attachments.push_back(std::move(spec));
      };

      add_item_attachment(attachment_id_model1,
                          item_display.getString(ItemDisplayInfoDB::ModelName1),
                          item_display.getString(ItemDisplayInfoDB::ModelTexture1));
      if (attachment_id_model2 >= 0)
      {
        add_item_attachment(attachment_id_model2,
                            item_display.getString(ItemDisplayInfoDB::ModelName2),
                            item_display.getString(ItemDisplayInfoDB::ModelTexture2));
      }
    }
    catch (DBCFile::NotFound const&)
    {
    }
  }

  std::vector<CreatureAttachmentModelSpec> resolve_creature_attachment_models(std::uint32_t display_id)
  {
    std::vector<CreatureAttachmentModelSpec> attachments;
    if (!display_id)
    {
      return attachments;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto extra_display_id = display.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if (!extra_display_id)
      {
        return attachments;
      }

      auto display_extra = gCreatureDisplayInfoExtraDB.getByID(extra_display_id);
      auto const race_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplayRaceID);
      auto const sex_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplaySexID);

      append_item_attachment_specs(attachments,
                                   display_extra.getUInt(CreatureDisplayInfoExtraDB::HeadDisplayID),
                                   "head",
                                   static_cast<int>(CharacterAttachmentId::Helmet),
                                   -1,
                                   race_id,
                                   sex_id);
      if (!classic_disable_shoulder_attachments_enabled())
      {
        append_item_attachment_specs(attachments,
                                     display_extra.getUInt(CreatureDisplayInfoExtraDB::ShouldersDisplayID),
                                     "shoulder",
                                     static_cast<int>(CharacterAttachmentId::LeftShoulder),
                                     static_cast<int>(CharacterAttachmentId::RightShoulder),
                                     race_id,
                                     sex_id);
      }
    }
    catch (DBCFile::NotFound const&)
    {
    }

    return attachments;
  }

  std::vector<CreatureAttachmentModelSpec> resolve_creature_equipment_attachment_models(std::uint32_t display_id,
                                                                                       std::uint32_t mainhand_display_id,
                                                                                       std::uint32_t offhand_display_id,
                                                                                       std::uint32_t ranged_display_id,
                                                                                       std::uint32_t offhand_inventory_type)
  {
    std::vector<CreatureAttachmentModelSpec> attachments;
    if (!display_id)
    {
      return attachments;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto extra_display_id = display.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if (!extra_display_id)
      {
        return attachments;
      }

      auto display_extra = gCreatureDisplayInfoExtraDB.getByID(extra_display_id);
      auto const race_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplayRaceID);
      auto const sex_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplaySexID);

      append_item_attachment_specs(attachments,
                                   mainhand_display_id,
                                   "weapon",
                                   static_cast<int>(CharacterAttachmentId::RightPalm),
                                   -1,
                                   race_id,
                                   sex_id);

      bool const offhand_is_shield = offhand_inventory_type == 14;
      append_item_attachment_specs(attachments,
                                   offhand_display_id,
                                   offhand_is_shield ? "shield" : "weapon",
                                   offhand_is_shield
                                     ? static_cast<int>(CharacterAttachmentId::LeftWrist)
                                     : static_cast<int>(CharacterAttachmentId::LeftPalm),
                                   -1,
                                   race_id,
                                   sex_id);

      append_item_attachment_specs(attachments,
                                   ranged_display_id,
                                   "weapon",
                                   static_cast<int>(CharacterAttachmentId::RightBackSheath),
                                   -1,
                                   race_id,
                                   sex_id);
    }
    catch (DBCFile::NotFound const&)
    {
    }

    return attachments;
  }

  // Aura state-kit visuals: creatures with permanent auras (creature_template.auras) get the aura's
  // STATE kit effect models attached while the aura is active -- verified against the live client via
  // apitrace (e.g. Anomalus's "Arcane Aura" 51098 -> SpellVisual 4659 -> state kit -> chest effect
  // Spells\RibbonTrail.m2 = the stationary twinkling chest sparkle). Resolution chain:
  // aura spell id -> spell_template.spellVisual1 -> SpellVisual.StateKit -> kit Head/Chest/Base
  // effects -> SpellVisualEffectName model path, attached at the M2 Head(20)/Chest(34)/Base(19)
  // attachment points.
  std::string normalize_effect_model_path(std::string path)
  {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    if (path.size() > 4)
    {
      auto const ext = path.substr(path.size() - 4);
      if (ext == ".mdx" || ext == ".mdl")
      {
        path.replace(path.size() - 4, 4, ".m2");
      }
    }
    return path;
  }

  std::vector<CreatureAttachmentModelSpec> resolve_creature_aura_attachment_models(
      std::string const& auras,
      std::map<std::uint32_t, World::SpellInfo> const& spell_infos)
  {
    std::vector<CreatureAttachmentModelSpec> attachments;
    if (auras.empty())
    {
      return attachments;
    }

    std::istringstream tokens(auras);
    std::uint32_t spell_id = 0;
    while (tokens >> spell_id)
    {
      auto const info_it = spell_infos.find(spell_id);
      if (info_it == spell_infos.end() || !info_it->second.spell_visual)
      {
        continue;
      }

      try
      {
        auto visual = gSpellVisualDB.getByID(info_it->second.spell_visual);
        auto const state_kit_id = visual.getUInt(SpellVisualDB::StateKit);
        if (!state_kit_id)
        {
          continue;
        }

        auto kit = gSpellVisualKitDB.getByID(state_kit_id);

        struct KitSlot { std::size_t field; int attachment_id; };
        // M2 attachment points: Head=20, Chest=34, Base=19 (verified against Anomalus.m2's attachment
        // table + the live-client trace position of the chest effect). NOTE: not named "slots" -- Qt
        // defines that as a macro.
        std::array<KitSlot, 3> const kit_slots { { { SpellVisualKitDB::HeadEffect, 20 },
                                                   { SpellVisualKitDB::ChestEffect, 34 },
                                                   { SpellVisualKitDB::BaseEffect, 19 } } };
        for (auto const& kit_slot : kit_slots)
        {
          auto const effect_id = kit.getUInt(kit_slot.field);
          if (!effect_id || effect_id == 0xFFFFFFFFu)
          {
            continue;
          }

          try
          {
            auto effect = gSpellVisualEffectNameDB.getByID(effect_id);
            std::string model_path = normalize_effect_model_path(effect.getString(SpellVisualEffectNameDB::FileName));
            if (model_path.empty())
            {
              continue;
            }

            CreatureAttachmentModelSpec spec;
            spec.attachment_id = kit_slot.attachment_id;
            spec.model_path = std::move(model_path);
            attachments.push_back(std::move(spec));
          }
          catch (DBCFile::NotFound const&)
          {
          }
        }
      }
      catch (DBCFile::NotFound const&)
      {
      }
    }

    return attachments;
  }

  CreatureGeosetSelection resolve_creature_geoset_selection(std::uint32_t display_id)
  {
    CreatureGeosetSelection selection;
    if (!display_id)
    {
      return selection;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto extra_display_id = display.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if (!extra_display_id)
      {
        selection.debug_summary = " extraDisplay=0";
        return selection;
      }

      auto display_extra = gCreatureDisplayInfoExtraDB.getByID(extra_display_id);
      std::ostringstream debug;
      debug << " extraDisplay=" << extra_display_id;
      debug << " buildProbe=20260605a";
      bool has_robe_bottom = false;
      auto const race_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplayRaceID);
      auto const sex_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplaySexID);
      auto const hair_style_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::HairStyleID);
      auto const facial_hair_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::FacialHairID);
      auto const gloves_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::GlovesDisplayID);

      apply_character_default_geosets(selection);
      debug << " defaults=wmvxLegacy";

      if (auto hair_geoset = resolve_classic_hair_geoset(race_id, sex_id, hair_style_id))
      {
        add_geoset_visible_id(selection, static_cast<std::uint16_t>(*hair_geoset));
        debug << " hairGeosetApplied=" << *hair_geoset;
      }
      else
      {
        debug << " hairGeosetMissing=1";
      }

      if (race_id == 9)
      {
        auto const goblin_hair_variant = std::max<std::uint32_t>(hair_style_id, 1u);
        assign_geoset_selection(selection, CharacterGeosetFamily::Geoset100, goblin_hair_variant);
        debug << " goblinHairVariantApplied=" << goblin_hair_variant;
      }

      if (auto facial_hair_geosets = resolve_classic_facial_hair_geosets(race_id, sex_id, facial_hair_id))
      {
        assign_geoset_selection_if_nonzero(selection, CharacterGeosetFamily::Geoset100, facial_hair_geosets->beard, true);
        assign_geoset_selection_if_nonzero(selection, CharacterGeosetFamily::Geoset200, facial_hair_geosets->moustache, true);
        assign_geoset_selection_if_nonzero(selection, CharacterGeosetFamily::Geoset300, facial_hair_geosets->sideburn, true);
        debug << " facialHairApplied=["
              << facial_hair_geosets->beard << ","
              << facial_hair_geosets->moustache << ","
              << facial_hair_geosets->sideburn << "]";
      }
      else
      {
        debug << " facialHairMissing=1";
      }

      auto apply_item = [&](char const* slot_name,
                            std::uint32_t item_display_id,
                            auto&& callback)
      {
        if (!item_display_id)
        {
          return;
        }

        try
        {
          auto item_display = gItemDisplayInfoDB.getByID(item_display_id);
          append_item_geoset_debug(debug, slot_name, item_display_id, item_display);
          callback(item_display);
        }
        catch (DBCFile::NotFound const&)
        {
          debug << " " << slot_name << "Item=" << item_display_id << " missing=1";
        }
      };

      apply_item("head", display_extra.getUInt(CreatureDisplayInfoExtraDB::HeadDisplayID),
        [&](DBCFile::Record const&){});

      apply_item("shirt", display_extra.getUInt(CreatureDisplayInfoExtraDB::ShirtDisplayID),
        [&](DBCFile::Record const& item_display)
        {
          auto const wrist_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup1);
          auto const robe_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup3);
          assign_geoset_selection(selection, CharacterGeosetFamily::Wristbands, wrist_flags);
          debug << " shirtWristApplied=" << wrist_flags;
          if (robe_flags != 0)
          {
            assign_geoset_selection(selection, CharacterGeosetFamily::Trousers, robe_flags);
            has_robe_bottom = has_robe_bottom || robe_flags == 1;
            debug << " shirtRobeApplied=" << robe_flags;
          }
        });

      apply_item("chest", display_extra.getUInt(CreatureDisplayInfoExtraDB::ChestDisplayID),
        [&](DBCFile::Record const& item_display)
        {
          auto const wrist_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup1);
          auto const robe_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup3);
          assign_geoset_selection(selection, CharacterGeosetFamily::Wristbands, wrist_flags);
          debug << " chestWristApplied=" << wrist_flags;
          if (robe_flags != 0)
          {
            assign_geoset_selection(selection, CharacterGeosetFamily::Trousers, robe_flags);
            has_robe_bottom = has_robe_bottom || robe_flags == 1;
            debug << " chestRobeApplied=" << robe_flags;
          }
        });

      apply_item("legs", display_extra.getUInt(CreatureDisplayInfoExtraDB::LegsDisplayID),
        [&](DBCFile::Record const& item_display)
        {
          auto const kneepad_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup2);
          auto const robe_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup3);
          assign_geoset_selection(selection, CharacterGeosetFamily::Kneepads, kneepad_flags);
          assign_geoset_selection(selection, CharacterGeosetFamily::Trousers, robe_flags);
          has_robe_bottom = has_robe_bottom || robe_flags == 1;
          debug << " legsKneepadApplied=" << kneepad_flags
                << " legsTrousersApplied=" << robe_flags;
        });

      apply_item("gloves", gloves_display_id,
        [&](DBCFile::Record const& item_display)
        {
          auto const glove_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup1);
          assign_geoset_selection(selection, CharacterGeosetFamily::Gloves, glove_flags);
          debug << " glovesApplied=" << glove_flags;
        });

      apply_item("boots", display_extra.getUInt(CreatureDisplayInfoExtraDB::BootsDisplayID),
        [&](DBCFile::Record const& item_display)
        {
          if (!has_robe_bottom)
          {
            auto const boot_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup1);
            assign_geoset_selection(selection, CharacterGeosetFamily::Boots, boot_flags);
            debug << " bootsApplied=" << boot_flags;
          }
        });

      apply_item("bracers", display_extra.getUInt(CreatureDisplayInfoExtraDB::BracersDisplayID),
        [&](DBCFile::Record const&){});
      apply_item("shoulders", display_extra.getUInt(CreatureDisplayInfoExtraDB::ShouldersDisplayID),
        [&](DBCFile::Record const&){});
      apply_item("belt", display_extra.getUInt(CreatureDisplayInfoExtraDB::BeltDisplayID),
        [&](DBCFile::Record const&){});
      apply_item("tabard", display_extra.getUInt(CreatureDisplayInfoExtraDB::TabardDisplayID),
        [&](DBCFile::Record const&)
        {
          if (!has_robe_bottom)
          {
            assign_geoset_selection(selection, CharacterGeosetFamily::Tabard, 1);
            debug << " tabardShown=1";
          }
          else
          {
            hide_geoset_family(selection, CharacterGeosetFamily::Tabard);
            debug << " tabardSuppressedByRobe=1";
          }
        });

      if (classic_probe_hide_trousers_enabled())
      {
        hide_geoset_family(selection, CharacterGeosetFamily::Trousers);
        debug << " probeHideTrousers=1";
      }

      if (classic_probe_hide_chest_enabled())
      {
        hide_geoset_family(selection, CharacterGeosetFamily::Chest);
        debug << " probeHideChest=1";
      }

      append_uint16_list_debug(debug, "selectedGeosets", selection.visible_ids);
      append_uint16_list_debug(debug, "controlledFamilies", selection.controlled_families);

      selection.debug_summary = debug.str();
      return selection;
    }
    catch (DBCFile::NotFound const&)
    {
      selection.debug_summary = " lookupFailed=1";
      return selection;
    }
  }

  std::string normalize_model_filename(std::string filename)
  {
    filename = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(filename));

    std::size_t found;
    if ((found = filename.rfind(".mdx")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }
    else if ((found = filename.rfind(".mdl")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }
    else if (filename.rfind('.') == std::string::npos)
    {
      filename += ".m2";
    }

    return filename;
  }

  std::string normalize_texture_filename(std::string filename)
  {
    if (filename.empty())
    {
      return filename;
    }

    filename = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(filename));
    if (filename.rfind('.') == std::string::npos)
    {
      filename += ".blp";
    }

    return filename;
  }

  std::string normalize_baked_creature_texture_filename(std::string filename)
  {
    if (filename.empty())
    {
      return filename;
    }

    std::string lower = BlizzardArchive::ClientData::normalizeFilenameInternal(filename);
    // Any bare filename (no path separator) belongs in bakednpctextures ΓÇö
    // this covers both "creaturedisplayextra-xxx.blp" and MD5-hash names.
    if (lower.find('/') == std::string::npos && lower.find('\\') == std::string::npos)
    {
      filename = "textures\\bakednpctextures\\" + filename;
    }

    return normalize_texture_filename(std::move(filename));
  }

  bool character_section_texture_suffix_matches(DBCFile::Record const& section,
                                                std::uint32_t value)
  {
    std::string const suffix = "_" + (value < 10 ? std::string("0") : std::string()) + std::to_string(value) + ".blp";

    for (std::size_t field : {CharacterSectionsDB::TextureName1,
                              CharacterSectionsDB::TextureName2,
                              CharacterSectionsDB::TextureName3})
    {
      auto texture = normalize_texture_filename(section.getString(field));
      if (texture.size() >= suffix.size()
          && texture.compare(texture.size() - suffix.size(), suffix.size(), suffix) == 0)
      {
        return true;
      }
    }

    return false;
  }

  CharacterSectionTextures resolve_character_section_textures(std::uint32_t race_id,
                                                              std::uint32_t sex_id,
                                                              CharacterSectionType section_type,
                                                              std::optional<std::uint32_t> section_id,
                                                              std::optional<std::uint32_t> color_id,
                                                              std::uint32_t preferred_suffix_value)
  {
    CharacterSectionTextures selected;
    bool selected_matches_suffix = false;

    for (auto it = gCharacterSectionsDB.begin(); it != gCharacterSectionsDB.end(); ++it)
    {
      if (it->getUInt(CharacterSectionsDB::RaceID) != race_id
          || it->getUInt(CharacterSectionsDB::SexID) != sex_id
          || it->getUInt(CharacterSectionsDB::BaseSection) != static_cast<std::uint32_t>(section_type))
      {
        continue;
      }

      if (section_id && it->getUInt(CharacterSectionsDB::VariationIndex) != *section_id)
      {
        continue;
      }

      if (color_id && it->getUInt(CharacterSectionsDB::ColorIndex) != *color_id)
      {
        continue;
      }

      bool const candidate_matches_suffix = character_section_texture_suffix_matches(*it, preferred_suffix_value);
      if (selected.found && (selected_matches_suffix || !candidate_matches_suffix))
      {
        continue;
      }

      selected.found = true;
      selected_matches_suffix = candidate_matches_suffix;
      selected.textures = {
        normalize_texture_filename(it->getString(CharacterSectionsDB::TextureName1)),
        normalize_texture_filename(it->getString(CharacterSectionsDB::TextureName2)),
        normalize_texture_filename(it->getString(CharacterSectionsDB::TextureName3))
      };
    }

    return selected;
  }

  std::vector<std::pair<std::size_t, std::string>> resolve_creature_texture_overrides_stable(std::uint32_t display_id)
  {
    if (!display_id)
    {
      return {};
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      constexpr std::array<std::pair<std::size_t, std::size_t>, 6> slot_fields = {{
        {1, CreatureDisplayInfoDB::TextureVariation1},
        {2, CreatureDisplayInfoDB::TextureVariation2},
        {3, CreatureDisplayInfoDB::TextureVariation3},
        {11, CreatureDisplayInfoDB::TextureVariation1},
        {12, CreatureDisplayInfoDB::TextureVariation2},
        {13, CreatureDisplayInfoDB::TextureVariation3},
      }};

      std::string model_dir;
      try
      {
        auto model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
        auto model = gCreatureModelDataDB.getByID(model_id);
        auto model_path = normalize_model_filename(model.getString(CreatureModelDataDB::ModelName));
        auto sep = model_path.rfind('/');
        if (sep != std::string::npos)
        {
          model_dir = model_path.substr(0, sep + 1);
        }
      }
      catch (...) {}

      std::vector<std::pair<std::size_t, std::string>> overrides;
      overrides.reserve(slot_fields.size());
      bool has_explicit_texture_variation = false;
      std::ostringstream debug_details;

      if (creature_texture_debug_enabled())
      {
        debug_details << "display=" << display_id
                      << " modelDir='" << model_dir << "'";
      }

      for (auto const& slot_field : slot_fields)
      {
        auto texture = normalize_texture_filename(display.getString(slot_field.second));
        if (!texture.empty())
        {
          has_explicit_texture_variation = true;
          if (!model_dir.empty() && texture.find('/') == std::string::npos)
          {
            texture = model_dir + texture;
          }
          overrides.emplace_back(slot_field.first, std::move(texture));
        }
      }

      if (creature_texture_debug_enabled())
      {
        debug_details << " explicitVariation=" << (has_explicit_texture_variation ? 1 : 0);
        debug_details << " explicitOverrides=[" << format_texture_override_list(overrides) << "]";
      }

      auto extra_display_id = display.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if (extra_display_id && !has_explicit_texture_variation)
      {
        try
        {
          auto display_extra = gCreatureDisplayInfoExtraDB.getByID(extra_display_id);
          auto baked_texture = normalize_baked_creature_texture_filename(display_extra.getString(CreatureDisplayInfoExtraDB::BakedTexture));
          auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
          bool const is_character_model = model_dir.rfind("character/", 0) == 0;
          bool const baked_texture_exists = !baked_texture.empty() && client->exists(baked_texture);
          bool baked_texture_loadable = !is_character_model && baked_texture_exists;

          if (baked_texture_exists && baked_texture_loadable)
          {
            overrides.emplace_back(1, std::move(baked_texture));
          }
          else
          {
            auto race_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplayRaceID);
            auto sex_id  = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplaySexID);
            auto skin_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::SkinID);

            std::string race_name;
            switch (race_id)
            {
              case  1: race_name = "Human";    break;
              case  2: race_name = "Orc";      break;
              case  3: race_name = "Dwarf";    break;
              case  4: race_name = "NightElf"; break;
              case  5: race_name = "Scourge";  break;
              case  6: race_name = "Tauren";   break;
              case  7: race_name = "Gnome";    break;
              case  8: race_name = "Troll";    break;
              case 10: race_name = "BloodElf"; break;
              case 11: race_name = "Draenei";  break;
              default: break;
            }

            if (!race_name.empty())
            {
              std::string const sex_name = (sex_id == 0) ? "Male" : "Female";
              std::string skin_pad = (skin_id < 10 ? "0" : "") + std::to_string(skin_id);
              auto skin_path = normalize_texture_filename("Character/" + race_name + "/" + sex_name + "/"
                + race_name + sex_name + "Skin00_" + skin_pad + ".blp");
              if (!client->exists(skin_path))
              {
                skin_path = normalize_texture_filename("Character/" + race_name + "/" + sex_name + "/"
                  + race_name + sex_name + "Skin00_00.blp");
              }
              if (client->exists(skin_path))
              {
                overrides.emplace_back(1, std::move(skin_path));
              }
            }
          }
        }
        catch (DBCFile::NotFound const&)
        {
        }
      }

      if (creature_texture_debug_enabled())
      {
        debug_details << " finalOverrides=[" << format_texture_override_list(overrides) << "]";
        LogDebug << "Creature stable texture overrides: " << debug_details.str() << std::endl;
        append_creature_geoset_trace("stable-texture", debug_details.str());
      }

      return overrides;
    }
    catch (DBCFile::NotFound const&)
    {
      return {};
    }
  }

  std::string build_character_model_skin_fallback(std::string const& model_dir)
  {
    if (model_dir.rfind("character/", 0) != 0 || model_dir.size() <= 1)
    {
      return {};
    }

    std::string path = model_dir;
    if (path.back() == '/')
    {
      path.pop_back();
    }

    auto sex_sep = path.rfind('/');
    if (sex_sep == std::string::npos)
    {
      return {};
    }

    auto race_sep = path.rfind('/', sex_sep == 0 ? 0 : sex_sep - 1);
    if (race_sep == std::string::npos)
    {
      return {};
    }

    std::string const race_name = path.substr(race_sep + 1, sex_sep - race_sep - 1);
    std::string const sex_name = path.substr(sex_sep + 1);
    if (race_name.empty() || sex_name.empty())
    {
      return {};
    }

    std::string base_name = race_name + sex_name;
    return normalize_texture_filename(path + "/" + base_name + "skin00_00.blp");
  }

  ClientFileHeaderProbe probe_client_file_header(std::string const& filename)
  {
    ClientFileHeaderProbe probe;

    if (filename.empty())
    {
      return probe;
    }

    try
    {
      BlizzardArchive::ClientFile file(filename,
        Noggit::Application::NoggitApplication::instance()->clientData());

      if (file.isEof())
      {
        return probe;
      }

      probe.opened = true;
      probe.size = file.getSize();

      std::size_t const magic_size = std::min<std::size_t>(4, probe.size);
      probe.magic.assign(file.getBuffer(), file.getBuffer() + magic_size);
      for (char& character : probe.magic)
      {
        if (character < 32 || character > 126)
        {
          character = '?';
        }
      }
    }
    catch (...)
    {
    }

    return probe;
  }

  glm::vec3 server_to_client_position(float server_x, float server_y, float server_z, bool global_wmo_map)
  {
    if (global_wmo_map)
    {
      return {-server_y, server_z, -server_x};
    }

    return {ZEROPOINT - server_y, server_z, ZEROPOINT - server_x};
  }

  float server_to_client_orientation(float orientation)
  {
    // Normalize to [0, 360) so it round-trips through client_to_server_* and fits the editor's 0..360
    // spinbox (the raw degrees(o)-180 lands in -180..180, or beyond for un-normalized DB angles, which
    // the spinbox would clamp). Same rotation either way.
    float degrees = std::fmod(glm::degrees(orientation) - 180.0f, 360.0f);
    if (degrees < 0.0f)
    {
      degrees += 360.0f;
    }
    return degrees;
  }

  std::vector<std::pair<std::size_t, std::string>> resolve_creature_texture_overrides(std::uint32_t display_id,
                                                                                      Noggit::NoggitRenderContext context)
  {
    if (!display_id)
    {
      return {};
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      constexpr std::array<std::pair<std::size_t, std::size_t>, 6> slot_fields = {{
        {1, CreatureDisplayInfoDB::TextureVariation1},
        {2, CreatureDisplayInfoDB::TextureVariation2},
        {3, CreatureDisplayInfoDB::TextureVariation3},
        {11, CreatureDisplayInfoDB::TextureVariation1},
        {12, CreatureDisplayInfoDB::TextureVariation2},
        {13, CreatureDisplayInfoDB::TextureVariation3},
      }};

      // Classic DBC stores short texture names (e.g. "DireWolf") with no path.
      // Resolve the model directory so we can build the full archive path.
      std::string model_dir;
      try
      {
        auto model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
        auto model = gCreatureModelDataDB.getByID(model_id);
        auto model_path = normalize_model_filename(model.getString(CreatureModelDataDB::ModelName));
        auto sep = model_path.rfind('/');
        if (sep != std::string::npos)
        {
          model_dir = model_path.substr(0, sep + 1);
        }
      }
      catch (...) {}

      std::vector<std::pair<std::size_t, std::string>> overrides;
      overrides.reserve(slot_fields.size());
      bool has_explicit_texture_variation = false;
      bool const debug_character_override = creature_texture_debug_enabled() && model_dir.rfind("character/", 0) == 0;
      std::ostringstream debug_details;

      if (debug_character_override)
      {
        debug_details << "display=" << display_id
                      << " modelDir='" << model_dir << "'";
      }

      auto append_override = [&overrides](std::size_t slot, std::string texture)
      {
        if (texture.empty())
        {
          return;
        }

        auto existing = std::find_if(overrides.begin(), overrides.end(),
          [slot](std::pair<std::size_t, std::string> const& override_entry)
          {
            return override_entry.first == slot;
          });

        if (existing == overrides.end())
        {
          overrides.emplace_back(slot, std::move(texture));
        }
      };

      for (auto const& slot_field : slot_fields)
      {
        auto texture = normalize_texture_filename(display.getString(slot_field.second));
        if (!texture.empty())
        {
          has_explicit_texture_variation = true;
          // No slash ΓåÆ Classic short name; prepend model directory.
          if (!model_dir.empty() && texture.find('/') == std::string::npos)
          {
            texture = model_dir + texture;
          }
          overrides.emplace_back(slot_field.first, std::move(texture));
        }
      }

      if (debug_character_override)
      {
        debug_details << " explicitVariation=" << (has_explicit_texture_variation ? 1 : 0);
        if (has_explicit_texture_variation)
        {
          debug_details << " explicitOverrides=[" << format_texture_override_list(overrides) << "]";
        }
      }

      auto extra_display_id = display.getUInt(CreatureDisplayInfoDB::ExtendedDisplayInfoID);
      if (debug_character_override)
      {
        debug_details << " extraDisplay=" << extra_display_id;
      }
      if (extra_display_id)
      {
        try
        {
          auto display_extra = gCreatureDisplayInfoExtraDB.getByID(extra_display_id);
          auto race_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplayRaceID);
          auto sex_id  = display_extra.getUInt(CreatureDisplayInfoExtraDB::DisplaySexID);
          auto skin_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::SkinID);
          auto face_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::FaceID);
          auto hair_style_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::HairStyleID);
          auto hair_color_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::HairColorID);
          auto facial_hair_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::FacialHairID);
          auto head_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::HeadDisplayID);
          auto shoulders_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::ShouldersDisplayID);
          auto shirt_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::ShirtDisplayID);
          auto chest_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::ChestDisplayID);
          auto belt_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::BeltDisplayID);
          auto legs_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::LegsDisplayID);
          auto boots_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::BootsDisplayID);
          auto bracers_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::BracersDisplayID);
          auto gloves_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::GlovesDisplayID);
          auto tabard_display_id = display_extra.getUInt(CreatureDisplayInfoExtraDB::TabardDisplayID);
          auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
          std::vector<CharacterTextureLayer> body_texture_layers;
          bool const is_character_model = model_dir.rfind("character/", 0) == 0;
          CharacterSectionTextures skin_section;
          CharacterSectionTextures face_section;
          CharacterSectionTextures hair_section;
          CharacterSectionTextures facial_hair_section;
          CharacterSectionTextures underwear_section;

          if (is_character_model)
          {
            skin_section = resolve_character_section_textures(race_id,
                                                              sex_id,
                                                              CharacterSectionType::Skin,
                                                              std::nullopt,
                                                              skin_id,
                                                              skin_id);
            face_section = resolve_character_section_textures(race_id,
                                                              sex_id,
                                                              CharacterSectionType::Face,
                                                              face_id,
                                                              skin_id,
                                                              skin_id);
            hair_section = resolve_character_section_textures(race_id,
                                                              sex_id,
                                                              CharacterSectionType::Hair,
                                                              hair_style_id,
                                                              hair_color_id,
                                                              hair_color_id);
            facial_hair_section = resolve_character_section_textures(race_id,
                                                                     sex_id,
                                                                     CharacterSectionType::FacialHair,
                                                                     hair_color_id,
                                                                     hair_color_id,
                                                                     hair_color_id);
            underwear_section = resolve_character_section_textures(race_id,
                                                                   sex_id,
                                                                   CharacterSectionType::Underwear,
                                                                   std::nullopt,
                                                                   skin_id,
                                                                   skin_id);
          }

          if (debug_character_override)
          {
            debug_details << " raceId=" << race_id
                          << " sexId=" << sex_id
                          << " skinId=" << skin_id
                          << " faceId=" << face_id
                          << " hairStyleId=" << hair_style_id
                          << " hairColorId=" << hair_color_id
                          << " facialHairId=" << facial_hair_id
                          << " headDisplayId=" << head_display_id
                          << " shouldersDisplayId=" << shoulders_display_id
                          << " shirtDisplayId=" << shirt_display_id
                          << " chestDisplayId=" << chest_display_id
                          << " beltDisplayId=" << belt_display_id
                          << " legsDisplayId=" << legs_display_id
                          << " bootsDisplayId=" << boots_display_id
                          << " bracersDisplayId=" << bracers_display_id
                          << " glovesDisplayId=" << gloves_display_id
                          << " tabardDisplayId=" << tabard_display_id;

            if (is_character_model)
            {
              debug_details << " charSectionsFound=[skin:" << (skin_section.found ? 1 : 0)
                            << ",face:" << (face_section.found ? 1 : 0)
                            << ",hair:" << (hair_section.found ? 1 : 0)
                            << ",facial:" << (facial_hair_section.found ? 1 : 0)
                            << ",underwear:" << (underwear_section.found ? 1 : 0)
                            << "]";
            }

            append_item_display_debug_if_present(debug_details, "head", head_display_id);
            append_item_display_debug_if_present(debug_details, "shoulders", shoulders_display_id);
            append_item_display_debug_if_present(debug_details, "shirt", shirt_display_id);
            append_item_display_debug_if_present(debug_details, "chest", chest_display_id);
            append_item_display_debug_if_present(debug_details, "belt", belt_display_id);
            append_item_display_debug_if_present(debug_details, "legs", legs_display_id);
            append_item_display_debug_if_present(debug_details, "boots", boots_display_id);
            append_item_display_debug_if_present(debug_details, "bracers", bracers_display_id);
            append_item_display_debug_if_present(debug_details, "gloves", gloves_display_id);
            append_item_display_debug_if_present(debug_details, "tabard", tabard_display_id);
          }

          auto append_body_layer = [&](DBCFile::Record const& item_display,
                                       std::size_t texture_field,
                                       CharacterTextureRegion region)
          {
            auto texture = resolve_item_texture_component_filename(item_display.getString(texture_field), region);
            if (!texture.empty())
            {
              body_texture_layers.push_back({std::move(texture), region});
            }
          };

          auto append_section_body_layer = [&](CharacterSectionTextures const& section,
                                               std::size_t texture_index,
                                               CharacterTextureRegion region)
          {
            if (!section.found || texture_index >= section.textures.size())
            {
              return;
            }

            auto texture = section.textures[texture_index];
            if (!texture.empty())
            {
              body_texture_layers.push_back({std::move(texture), region});
            }
          };

          auto append_item_body_layers = [&](std::uint32_t item_display_id, auto&& callback)
          {
            if (!item_display_id)
            {
              return;
            }

            try
            {
              auto const item_display = gItemDisplayInfoDB.getByID(item_display_id);
              callback(item_display);
            }
            catch (DBCFile::NotFound const&)
            {
            }
          };

          append_section_body_layer(underwear_section, 0, CharacterTextureRegion::LegUpper);
          append_section_body_layer(underwear_section, 1, CharacterTextureRegion::TorsoUpper);
          append_section_body_layer(face_section, 0, CharacterTextureRegion::FaceLower);
          append_section_body_layer(face_section, 1, CharacterTextureRegion::FaceUpper);
          append_section_body_layer(facial_hair_section, 0, CharacterTextureRegion::FaceLower);
          append_section_body_layer(facial_hair_section, 1, CharacterTextureRegion::FaceUpper);
          append_section_body_layer(hair_section, 1, CharacterTextureRegion::FaceLower);
          append_section_body_layer(hair_section, 2, CharacterTextureRegion::FaceUpper);

          auto append_chest_like_layers = [&](DBCFile::Record const& item_display)
          {
            append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperArm, CharacterTextureRegion::ArmUpper);
            append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerArm, CharacterTextureRegion::ArmLower);
            append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperChest, CharacterTextureRegion::TorsoUpper);
            append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerChest, CharacterTextureRegion::TorsoLower);

            auto const robe_flags = item_display.getUInt(ItemDisplayInfoDB::GeosetGroup3);
            if (robe_flags != 0)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperLeg, CharacterTextureRegion::LegUpper);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerLeg, CharacterTextureRegion::LegLower);
            }
          };

          append_item_body_layers(shirt_display_id, append_chest_like_layers);
          append_item_body_layers(chest_display_id, append_chest_like_layers);
          append_item_body_layers(belt_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerChest, CharacterTextureRegion::TorsoLower);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperLeg, CharacterTextureRegion::LegUpper);
            });
          append_item_body_layers(bracers_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerArm, CharacterTextureRegion::ArmLower);
            });
          append_item_body_layers(legs_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperLeg, CharacterTextureRegion::LegUpper);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerLeg, CharacterTextureRegion::LegLower);
            });
          append_item_body_layers(gloves_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureHands, CharacterTextureRegion::Hand);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerArm, CharacterTextureRegion::ArmLower);
            });
          append_item_body_layers(boots_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerLeg, CharacterTextureRegion::LegLower);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureFoot, CharacterTextureRegion::Foot);
            });
          append_item_body_layers(tabard_display_id,
            [&](DBCFile::Record const& item_display)
            {
              append_body_layer(item_display, ItemDisplayInfoDB::TextureUpperChest, CharacterTextureRegion::TorsoUpper);
              append_body_layer(item_display, ItemDisplayInfoDB::TextureLowerChest, CharacterTextureRegion::TorsoLower);
            });

          std::string character_skin_texture;
          bool character_skin_exists = false;
          if (is_character_model)
          {
            if (skin_section.found && !skin_section.textures[0].empty())
            {
              character_skin_texture = skin_section.textures[0];
              character_skin_exists = client->exists(character_skin_texture);
            }

            if (!hair_section.textures[0].empty() && client->exists(hair_section.textures[0]))
            {
              append_override(6, hair_section.textures[0]);
              if (debug_character_override)
              {
                debug_details << " hairTexture='" << hair_section.textures[0] << "'";
              }
            }

            if (skin_section.found && !skin_section.textures[1].empty() && client->exists(skin_section.textures[1]))
            {
              append_override(8, skin_section.textures[1]);
              if (debug_character_override)
              {
                debug_details << " extraSkinTexture='" << skin_section.textures[1] << "'";
              }
            }

            if (!character_skin_exists)
            {
              std::string race_name;
              switch (race_id)
              {
                case  1: race_name = "Human";    break;
                case  2: race_name = "Orc";      break;
                case  3: race_name = "Dwarf";    break;
                case  4: race_name = "NightElf"; break;
                case  5: race_name = "Scourge";  break;
                case  6: race_name = "Tauren";   break;
                case  7: race_name = "Gnome";    break;
                case  8: race_name = "Troll";    break;
                case 10: race_name = "BloodElf"; break;
                case 11: race_name = "Draenei";  break;
                default: break;
              }

              if (!race_name.empty())
              {
                std::string const sex_name = (sex_id == 0) ? "Male" : "Female";
                std::string skin_pad = (skin_id < 10 ? "0" : "") + std::to_string(skin_id);
                character_skin_texture = normalize_texture_filename("Character/" + race_name + "/" + sex_name + "/"
                  + race_name + sex_name + "Skin00_" + skin_pad + ".blp");
                character_skin_exists = client->exists(character_skin_texture);
                if (!character_skin_exists)
                {
                  character_skin_texture = normalize_texture_filename("Character/" + race_name + "/" + sex_name + "/"
                    + race_name + sex_name + "Skin00_00.blp");
                  character_skin_exists = client->exists(character_skin_texture);
                }
              }
            }
          }

          if (!has_explicit_texture_variation)
          {
            auto baked_texture = normalize_baked_creature_texture_filename(display_extra.getString(CreatureDisplayInfoExtraDB::BakedTexture));
            bool const disable_baked_npc_textures = is_character_model && classic_probe_disable_baked_npc_textures_enabled();
            auto append_character_fallback = [&](std::string texture)
            {
              if (is_character_model)
              {
                append_override(1, texture);
                append_override(2, texture);
              }
              else
              {
                append_override(1, std::move(texture));
              }
            };

            bool const baked_texture_exists = !baked_texture.empty() && client->exists(baked_texture);
            bool baked_texture_loadable = baked_texture_exists;
            char const* baked_texture_load_method = nullptr;
            ClientFileHeaderProbe baked_texture_probe;
            if (debug_character_override && baked_texture_exists)
            {
              baked_texture_probe = probe_client_file_header(baked_texture);
            }

            if (baked_texture_exists && is_character_model && !disable_baked_npc_textures)
            {
              QImage baked_texture_image;
              baked_texture_loadable = load_client_texture_image(baked_texture,
                                                                 baked_texture_image,
                                                                 nullptr,
                                                                 &baked_texture_load_method);
            }

            if (baked_texture_exists && baked_texture_loadable && !disable_baked_npc_textures)
            {
              if (debug_character_override)
              {
                debug_details << " baked='" << baked_texture << "' bakedExists=1";
                debug_details << " bakedLoadable=" << (baked_texture_loadable ? 1 : 0);
                debug_details << " bakedOpened=" << (baked_texture_probe.opened ? 1 : 0);
                if (baked_texture_probe.opened)
                {
                  debug_details << " bakedSize=" << baked_texture_probe.size
                                << " bakedMagic='" << baked_texture_probe.magic << "'";
                }
                if (baked_texture_load_method)
                {
                  debug_details << " bakedLoadMethod='" << baked_texture_load_method << "'";
                }
              }
              append_character_fallback(std::move(baked_texture));
            }
            else
            {
              // Baked texture not available (hash-named runtime textures are not shipped in MPQ).
              // Fall back to the character's base skin texture derived from CDIExtra fields.
              if (debug_character_override)
              {
                debug_details << " baked='" << baked_texture << "' bakedExists=" << (baked_texture_exists ? 1 : 0);
                if (baked_texture_exists)
                {
                  debug_details << " bakedLoadable=" << (baked_texture_loadable ? 1 : 0);
                  debug_details << " bakedOpened=" << (baked_texture_probe.opened ? 1 : 0);
                  if (baked_texture_probe.opened)
                  {
                    debug_details << " bakedSize=" << baked_texture_probe.size
                                  << " bakedMagic='" << baked_texture_probe.magic << "'";
                  }
                  if (baked_texture_load_method)
                  {
                    debug_details << " bakedLoadMethod='" << baked_texture_load_method << "'";
                  }
                }
                if (disable_baked_npc_textures)
                {
                  debug_details << " bakedSuppressed=1";
                }
              }

              if (!character_skin_texture.empty())
              {
                if (debug_character_override)
                {
                  debug_details << " skin='" << character_skin_texture << "' skinExists=" << (character_skin_exists ? 1 : 0);
                }
                if (character_skin_exists)
                {
                  append_character_fallback(character_skin_texture);
                }
              }
            }
          }

          if (debug_character_override && !character_skin_texture.empty() && has_explicit_texture_variation)
          {
            debug_details << " skin='" << character_skin_texture << "' skinExists=" << (character_skin_exists ? 1 : 0);
          }

          if (classic_pants_composite_enabled()
              && model_dir.rfind("character/", 0) == 0
              && !body_texture_layers.empty())
          {
            auto const body_override = std::find_if(overrides.begin(), overrides.end(),
              [](std::pair<std::size_t, std::string> const& override_entry)
              {
                return override_entry.first == 1u || override_entry.first == 2u;
              });

            if (body_override != overrides.end())
            {
              std::string composed_texture;
              bool composed = compose_character_body_texture(display_id,
                                                             body_override->second,
                                                             body_texture_layers,
                                                             context,
                                                             composed_texture,
                                                             debug_character_override ? &debug_details : nullptr);

              if (!composed && character_skin_exists && body_override->second != character_skin_texture)
              {
                if (debug_character_override)
                {
                  debug_details << " bodyTextureBaseFallback='" << character_skin_texture << "'";
                }

                composed = compose_character_body_texture(display_id,
                                                          character_skin_texture,
                                                          body_texture_layers,
                                                          context,
                                                          composed_texture,
                                                          debug_character_override ? &debug_details : nullptr);
              }

              if (composed)
              {
                set_texture_override(overrides, 1u, composed_texture);
                set_texture_override(overrides, 2u, composed_texture);
              }
            }
          }
        }
        catch (DBCFile::NotFound const&)
        {
        }
      }

      if (overrides.empty() && model_dir.rfind("character/", 0) == 0)
      {
        auto* client = Noggit::Application::NoggitApplication::instance()->clientData();
        auto fallback_texture = build_character_model_skin_fallback(model_dir);
        bool const fallback_exists = !fallback_texture.empty() && client->exists(fallback_texture);
        if (debug_character_override)
        {
          debug_details << " modelFallback='" << fallback_texture << "' modelFallbackExists=" << (fallback_exists ? 1 : 0);
        }
        if (fallback_exists)
        {
          append_override(1, fallback_texture);
          append_override(2, fallback_texture);
          append_override(8, std::move(fallback_texture));
        }
      }

      if (debug_character_override)
      {
        std::ostringstream override_log;
        override_log << debug_details.str()
                     << " finalOverrides=[" << format_texture_override_list(overrides) << "]";

        LogDebug << "Creature texture overrides: " << override_log.str() << std::endl;
        append_creature_geoset_trace("texture", override_log.str());
      }

      return overrides;
    }
    catch (DBCFile::NotFound const&)
    {
      return {};
    }
  }

  CreatureModelPathResult resolve_creature_model_path(std::uint32_t display_id)
  {
    if (!display_id)
    {
      return {};
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      if (!model_id)
      {
        return {CreatureModelPathStatus::MissingModelInfo, {}};
      }

      try
      {
        auto model = gCreatureModelDataDB.getByID(model_id);
        auto model_name = normalize_model_filename(model.getString(CreatureModelDataDB::ModelName));
        if (model_name.empty())
        {
          return {CreatureModelPathStatus::EmptyModelName, {}};
        }

        return {CreatureModelPathStatus::Success, std::move(model_name)};
      }
      catch (DBCFile::NotFound const&)
      {
        return {CreatureModelPathStatus::MissingModelInfo, {}};
      }
    }
    catch (DBCFile::NotFound const&)
    {
      return {CreatureModelPathStatus::MissingDisplayInfo, {}};
    }
  }

  // CreatureModelData.ModelScale (M) -- the model's intrinsic scale, applied by the client on top of the
  // object scale. This is NOT the display scale. The object scale (creature_template.scale, with the
  // CreatureDisplayInfo fallback) is handled by the caller via resolve_creature_display_scale.
  //
  // Verified against the server source (tortoise-wow ObjectMgr.cpp:1436): the object scale is
  // creature_template.scale when > 0, ELSE CreatureDisplayInfo.scale (D) -- a *fallback*, not an extra
  // multiplier. The client then multiplies by ModelData.ModelScale (M). Final = (T>0?T:D) * M. Treating
  // D as an always-on multiplier made Flamewaker (T=2.5,D=2) render at 5.0, dwarfing Gehennas (T=0->D=3)
  // at 3.0 -- the in-game ratio is the reverse (Gehennas >= Flamewaker).
  float resolve_creature_model_scale(std::uint32_t display_id)
  {
    if (!display_id)
    {
      return 1.0f;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      auto model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      auto model = gCreatureModelDataDB.getByID(model_id);

      float model_scale = model.getFloat(CreatureModelDataDB::ModelScale);
      return model_scale > 0.0f ? model_scale : 1.0f;
    }
    catch (DBCFile::NotFound const&)
    {
      return 1.0f;
    }
  }

  // CreatureDisplayInfo.CreatureModelScale (D) -- the object-scale fallback used when
  // creature_template.scale is 0 (matches server ObjectMgr.cpp:1436). 0/missing -> 1.0.
  float resolve_creature_display_scale(std::uint32_t display_id)
  {
    if (!display_id)
    {
      return 1.0f;
    }

    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      float display_scale = display.getFloat(CreatureDisplayInfoDB::CreatureModelScale);
      return display_scale > 0.0f ? display_scale : 1.0f;
    }
    catch (DBCFile::NotFound const&)
    {
      return 1.0f;
    }
  }

  std::string resolve_gameobject_model_path(std::uint32_t display_id)
  {
    if (!display_id)
    {
      return {};
    }

    try
    {
      auto display = gGameObjectDisplayInfoDB.getByID(display_id);
      auto model_name = normalize_model_filename(display.getString(GameObjectDisplayInfoDB::ModelName));
      if (model_name.empty())
      {
        return {};
      }

      return model_name;
    }
    catch (DBCFile::NotFound const&)
    {
      return {};
    }
  }
}


bool World::IsEditableWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
  ZoneScoped;
  auto directory_column = record.Columns.find("Directory");
  if (directory_column == record.Columns.end() || directory_column->second.Value.empty())
  {
    Log << "World " << record.RecordId << ": missing Directory column in Map.dbc row, skipping." << std::endl;
    return false;
  }

  std::string lMapName = directory_column->second.Value;

  std::stringstream ssfilename;
  ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(ssfilename.str()))
  {
    Log << "World " << record.RecordId << ": " << lMapName << " has no WDT file!" << std::endl;
    return false;
  }

  BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  //sometimes, wdts don't open, so ignore them...
  if (mf.isEof())
    return false;

  const char * lPointer = reinterpret_cast<const char*>(mf.getPointer());

  // Not using the libWDT here doubles performance. You might want to look at your lib again and improve it.
  const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
  if (lFlags & 1)
    return true; // filter them later

  const int * lData = reinterpret_cast<const int*>(lPointer + 8 + 4 + 8 + 0x20 + 8);
  for (int i = 0; i < 8192; i += 2)
  {
    if (lData[i] & 1)
      return true;
  }

  return false;
}

bool World::IsEditableWorld(DBCFile::Record const& record)
{
  ZoneScoped;

  std::string lMapName = record.getString(MapDB::InternalName);
  if (lMapName.empty())
    return false;

  std::stringstream ssfilename;
  ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(ssfilename.str()))
  {
    Log << "World " << record.getUInt(MapDB::MapID) << ": " << lMapName << " has no WDT file!" << std::endl;
    return false;
  }

  BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  if (mf.isEof())
    return false;

  const char* lPointer = reinterpret_cast<const char*>(mf.getPointer());

  const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
  if (lFlags & 1)
    return true;

  const int* lData = reinterpret_cast<const int*>(lPointer + 8 + 4 + 8 + 0x20 + 8);
  for (int i = 0; i < 8192; i += 2)
  {
    if (lData[i] & 1)
      return true;
  }

  return false;
}

bool World::IsWMOWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
    ZoneScoped;
    auto directory_column = record.Columns.find("Directory");
    if (directory_column == record.Columns.end() || directory_column->second.Value.empty())
      return false;

    std::string lMapName = directory_column->second.Value;

    std::stringstream ssfilename;
    ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

    BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

    const char* lPointer = reinterpret_cast<const char*>(mf.getPointer());

    const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
    if (lFlags & 1)
        return true;

    return false;
}

bool World::IsWMOWorld(DBCFile::Record const& record)
{
    ZoneScoped;

    std::string lMapName = record.getString(MapDB::InternalName);
    if (lMapName.empty())
      return false;

    std::stringstream ssfilename;
    ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

    BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());
    if (mf.isEof())
      return false;

    const char* lPointer = reinterpret_cast<const char*>(mf.getPointer());

    const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
    if (lFlags & 1)
        return true;

    return false;
}

World::World(const std::string& name, int map_id, Noggit::NoggitRenderContext context, bool create_empty)
    : _renderer(Noggit::Rendering::WorldRender(this))
    , _model_instance_storage(this)
    , _tile_update_queue(this)
    , mapIndex(name, map_id, this, context, create_empty)
    , horizon(name, &mapIndex)
    , mWmoFilename(mapIndex.globalWMOName)
    , mWmoEntry(mapIndex.wmoEntry)
    , animtime(0)
    , time(1450)
    , basename(name)
    , _current_selection()
    , _settings(new QSettings())
    , _context(context)
{
  LogDebug << "Loading world \"" << name << "\"." << std::endl;
  append_creature_geoset_trace("world", std::string("name='") + name + "' mapId=" + std::to_string(map_id));
  _loaded_tiles_buffer[0] = std::make_pair<std::pair<int, int>, MapTile*>(std::make_pair(0, 0), nullptr);
  _creature_spawn_status = "Creature spawns inactive";
}

World::~World()
{
  // Mark teardown BEFORE any member is destroyed. The members below (mapIndex -> every MapTile, then
  // _model_instance_storage) are destroyed in reverse-declaration order AFTER this body runs, so each
  // MapTile::~MapTile sees is_unloading()==true and skips its per-tile derefTile()/remove_models_if_needed
  // -- the instance storage frees all M2/WMO instances itself when it is destroyed. This prevents the
  // map-exit crash where one tile's unload freed an instance still referenced by another tile, which then
  // dereferenced the freed SceneObject in SceneObject::derefTile.
  _unloading = true;
}

bool World::reloadCreatureSpawns()
{
  append_creature_geoset_trace("reload", std::string("mapId=") + std::to_string(mapIndex._map_id));
  _creature_spawns_load_attempted = true;
  if (!_creature_spawns.empty())
  {
    AsyncLoader::instance().wait_until_idle();
  }
  clearCreatureSpawns();

#ifdef USE_MYSQL_UID_STORAGE
  std::string error;
  auto rows = mysql::getCreatureSpawns(mapIndex._map_id, &error);
  if (!error.empty())
  {
    _creature_spawn_status = "Creature spawn load failed: " + error;
    return false;
  }

  LogDebug << "Creature spawn DBC stats: CreatureDisplayInfo records=" << gCreatureDisplayInfoDB.getRecordCount()
           << ", CreatureModelData records=" << gCreatureModelDataDB.getRecordCount() << std::endl;

  // Per-display bounding radii (the client's exact selection-circle size source).
  auto const bounding_radii = mysql::getCreatureBoundingRadii();

  _creature_spawns.reserve(rows.size());
  std::size_t resolved_models = 0;
  std::size_t missing_display_id = 0;
  std::size_t missing_display_info = 0;
  std::size_t missing_model_info = 0;
  std::size_t empty_model_name = 0;
  std::size_t model_construct_failures = 0;

  for (auto const& row : rows)
  {
    CreatureSpawnOverlay spawn;
    spawn.guid = row.guid;
    spawn.entry = row.entry;
    spawn.display_id = row.display_id;
    spawn.name = row.name;
    spawn.pos = server_to_client_position(row.position_x,
              row.position_y,
              row.position_z,
              mapIndex.hasAGlobalWMO());
    spawn.original_pos = spawn.pos;
    spawn.orientation = server_to_client_orientation(row.orientation);
    spawn.original_orientation = spawn.orientation;
    spawn.animation_time_offset = static_cast<int>(((row.guid * 1103515245u) + (row.entry * 12345u)) % 3500u);
    // Object scale = creature_template.scale, falling back to CreatureDisplayInfo.scale (D) when 0
    // (server ObjectMgr.cpp:1436). Final render = template_scale * model_scale = (T>0?T:D) * M.
    spawn.template_scale = row.template_scale > 0.0f ? row.template_scale : resolve_creature_display_scale(row.display_id);
    spawn.mainhand_display_id = row.mainhand_display_id;
    spawn.offhand_display_id = row.offhand_display_id;
    spawn.ranged_display_id = row.ranged_display_id;
    spawn.mainhand_inventory_type = row.mainhand_inventory_type;
    spawn.offhand_inventory_type = row.offhand_inventory_type;
    spawn.ranged_inventory_type = row.ranged_inventory_type;
    spawn.auras = row.auras;
    if (auto const radius_it = bounding_radii.find(spawn.display_id); radius_it != bounding_radii.end())
    {
      // The client's ring radius = bounding_radius x the RAW template scale (1.0 when unset) --
      // verified against live Onyxia (1.8 x 2.0 = 3.6) vs Onyxian Warder (3.0 x 1.0 = 3.0). The
      // display-scale fallback the model render uses does NOT apply here (the warder's ring would
      // wrongly double to 6.0).
      spawn.bounding_radius = radius_it->second * (row.template_scale > 0.0f ? row.template_scale : 1.0f);
    }

    if (spawn.display_id)
    {
      auto const model_result = resolve_creature_model_path(spawn.display_id);
      if (model_result.status == CreatureModelPathStatus::Success)
      {
        spawn.model_path = model_result.path;
        spawn.model_scale = resolve_creature_model_scale(spawn.display_id);
        spawn.is_character_model = spawn.model_path.rfind("character/", 0) == 0;
        ++resolved_models;
      }
      else
      {
        switch (model_result.status)
        {
          case CreatureModelPathStatus::MissingDisplayInfo: ++missing_display_info; break;
          case CreatureModelPathStatus::MissingModelInfo:   ++missing_model_info;   break;
          case CreatureModelPathStatus::EmptyModelName:     ++empty_model_name;     break;
          default:                                          ++missing_display_id;   break;
        }
      }
    }
    else
    {
      ++missing_display_id;
    }

    _creature_spawns.emplace_back(std::move(spawn));
  }

  // Fetch spell details for every permanent aura on the loaded spawns in ONE query. Used to resolve
  // aura state-kit visuals (the effect models the client attaches while an aura is on -- e.g. the
  // arcane elementals' chest sparkle = RibbonTrail.m2 from "Arcane Aura"'s state kit) and by the
  // creature-info UI.
  _spell_infos.clear();
  {
    std::set<std::uint32_t> aura_spell_ids;
    for (auto const& spawn : _creature_spawns)
    {
      std::istringstream tokens(spawn.auras);
      std::uint32_t spell_id = 0;
      while (tokens >> spell_id)
      {
        if (spell_id)
        {
          aura_spell_ids.insert(spell_id);
        }
      }
    }
    if (!aura_spell_ids.empty())
    {
      std::string spell_error;
      auto spell_rows = mysql::getSpellInfos(aura_spell_ids, &spell_error);
      if (!spell_error.empty())
      {
        LogDebug << "Aura spell info load failed: " << spell_error << std::endl;
      }
      for (auto const& [spell_id, row] : spell_rows)
      {
        SpellInfo info;
        info.entry = row.entry;
        info.spell_visual = row.spell_visual;
        info.icon_id = row.icon_id;
        info.school = row.school;
        info.name = row.name;
        info.description = row.description;
        _spell_infos.emplace(spell_id, std::move(info));
      }
      LogDebug << "Aura spell infos loaded: " << _spell_infos.size()
               << " (requested " << aura_spell_ids.size() << ")" << std::endl;
    }
  }

  _creature_spawns_loaded = true;
  _creature_spawn_status = "Creature spawns loaded: " + std::to_string(_creature_spawns.size())
                         + " (models: " + std::to_string(resolved_models)
                         + ", noDisplayId: " + std::to_string(missing_display_id)
                         + ", noDisplayInfo: " + std::to_string(missing_display_info)
                         + ", noModelInfo: " + std::to_string(missing_model_info)
                         + ", emptyModelName: " + std::to_string(empty_model_name)
                         + ", modelCreateFail: " + std::to_string(model_construct_failures) + ")";
  LogDebug << _creature_spawn_status << std::endl;

  std::string gameobject_error;
  auto gameobject_rows = mysql::getGameObjectSpawns(mapIndex._map_id, &gameobject_error);
  if (!gameobject_error.empty())
  {
    LogDebug << "Gameobject spawn load failed: " << gameobject_error << std::endl;
  }

  std::size_t gameobject_resolved_models = 0;
  std::size_t gameobject_missing_display_id = 0;
  std::size_t gameobject_missing_model = 0;
  _gameobject_spawns.reserve(gameobject_rows.size());
  for (auto const& row : gameobject_rows)
  {
    GameObjectSpawnOverlay spawn;
    spawn.guid = row.guid;
    spawn.entry = row.entry;
    spawn.display_id = row.display_id;
    spawn.name = row.name;
    spawn.pos = server_to_client_position(row.position_x,
              row.position_y,
              row.position_z,
              mapIndex.hasAGlobalWMO());
    spawn.orientation = server_to_client_orientation(row.orientation);
    spawn.original_pos = spawn.pos;
    spawn.original_orientation = spawn.orientation;
    spawn.animation_time_offset = static_cast<int>(((row.guid * 1664525u) + (row.entry * 1013904223u)) % 3500u);
    // Object scale = creature_template.scale, falling back to CreatureDisplayInfo.scale (D) when 0
    // (server ObjectMgr.cpp:1436). Final render = template_scale * model_scale = (T>0?T:D) * M.
    spawn.template_scale = row.template_scale > 0.0f ? row.template_scale : resolve_creature_display_scale(row.display_id);

    if (spawn.display_id)
    {
      spawn.model_path = resolve_gameobject_model_path(spawn.display_id);
      if (!spawn.model_path.empty())
      {
        ++gameobject_resolved_models;
      }
      else
      {
        ++gameobject_missing_model;
      }
    }
    else
    {
      ++gameobject_missing_display_id;
    }

    _gameobject_spawns.emplace_back(std::move(spawn));
  }

  if (!gameobject_rows.empty() || !gameobject_error.empty())
  {
    _creature_spawn_status += "; gameobjects loaded: " + std::to_string(_gameobject_spawns.size())
                            + " (models: " + std::to_string(gameobject_resolved_models)
                            + ", noDisplayId: " + std::to_string(gameobject_missing_display_id)
                            + ", noModel: " + std::to_string(gameobject_missing_model) + ")";
    LogDebug << "Gameobject spawns loaded: " << _gameobject_spawns.size()
             << " (models: " << gameobject_resolved_models
             << ", noDisplayId: " << gameobject_missing_display_id
             << ", noModel: " << gameobject_missing_model << ")"
             << std::endl;
  }
  return true;
#else
  _creature_spawn_status = "Creature spawns unavailable: build without MySQL support";
  return false;
#endif
}

bool World::ensureCreatureSpawnModel(CreatureSpawnOverlay& spawn)
{
  if (spawn.model_instance.has_value())
  {
    return true;
  }

  if (spawn.model_create_failed || spawn.model_path.empty())
  {
    return false;
  }

  try
  {
    BlizzardArchive::Listfile::FileKey const file_key(spawn.model_path);
    spawn.model_instance.emplace(file_key, _context);
    spawn.model_instance->pos = spawn.pos;
    spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
    spawn.model_instance->scale = std::clamp(spawn.template_scale * spawn.model_scale,
                                             ModelInstance::min_scale(),
                                             ModelInstance::max_scale());
    spawn.model_instance->updateTransformMatrix();

    auto const overrides = applyCreatureSpawnModelAppearance(spawn, *spawn.model_instance, Noggit::NoggitRenderContext::MAP_VIEW);

    spawn.attachment_models.clear();
    {
      std::vector<CreatureAttachmentModelSpec> attachment_specs;
      if (spawn.is_character_model && creature_spawn_attachments_enabled())
      {
        attachment_specs = resolve_creature_attachment_models(spawn.display_id);
        auto equipment_specs = resolve_creature_equipment_attachment_models(spawn.display_id,
                                                                            spawn.mainhand_display_id,
                                                                            spawn.offhand_display_id,
                                                                            spawn.ranged_display_id,
                                                                            spawn.offhand_inventory_type);
        attachment_specs.insert(attachment_specs.end(),
                                std::make_move_iterator(equipment_specs.begin()),
                                std::make_move_iterator(equipment_specs.end()));
      }

      // Aura state-kit effect models (chest sparkles etc.) apply to EVERY creature with permanent
      // auras, not just character-skeleton models.
      auto aura_specs = resolve_creature_aura_attachment_models(spawn.auras, _spell_infos);
      attachment_specs.insert(attachment_specs.end(),
                              std::make_move_iterator(aura_specs.begin()),
                              std::make_move_iterator(aura_specs.end()));

      if (creature_texture_debug_enabled() && !attachment_specs.empty())
      {
        std::ostringstream attachment_line;
        attachment_line << "guid=" << spawn.guid
                        << " display=" << spawn.display_id
                        << " mainhandDisplay=" << spawn.mainhand_display_id
                        << " offhandDisplay=" << spawn.offhand_display_id
                        << " rangedDisplay=" << spawn.ranged_display_id
                        << " attachmentCount=" << attachment_specs.size();
        for (auto const& attachment_spec : attachment_specs)
        {
          attachment_line << " {id=" << attachment_spec.attachment_id
                          << ", model='" << attachment_spec.model_path << "'}";
        }
        LogDebug << "Creature attachment selection: " << attachment_line.str() << std::endl;
        append_creature_geoset_trace("attachment", attachment_line.str());
      }

      for (auto const& attachment_spec : attachment_specs)
      {
        try
        {
          CreatureSpawnOverlay::AttachmentModel attachment;
          attachment.attachment_id = attachment_spec.attachment_id;
          attachment.model_instance.emplace(BlizzardArchive::Listfile::FileKey(attachment_spec.model_path), _context);
          attachment.model_instance->pos = spawn.pos;
          attachment.model_instance->dir = spawn.model_instance->dir;
          attachment.model_instance->scale = spawn.model_instance->scale;
          attachment.model_instance->updateTransformMatrix();

          for (auto const& texture_override : attachment_spec.texture_overrides)
          {
            attachment.model_instance->setReplaceTexture(texture_override.first, texture_override.second);
          }

          spawn.attachment_models.push_back(std::move(attachment));
        }
        catch (std::exception const& ex)
        {
          if (creature_texture_debug_enabled())
          {
            LogDebug << "Creature attachment model load failed: guid=" << spawn.guid
                     << " display_id=" << spawn.display_id
                     << " attachment=" << attachment_spec.attachment_id
                     << " path='" << attachment_spec.model_path << "'"
                     << " error=" << ex.what()
                     << std::endl;
          }
        }
      }
    }

    std::size_t geoset_controlled_family_count = 0;
    std::string geoset_debug_summary;

    if (enable_creature_spawn_character_geosets
        && creature_spawn_geosets_enabled()
        && spawn.is_character_model)
    {
      auto const geoset_selection = resolve_creature_geoset_selection(spawn.display_id);
      geoset_controlled_family_count = geoset_selection.controlled_families.size();
      geoset_debug_summary = geoset_selection.debug_summary;
      if (!geoset_selection.empty())
      {
        spawn.model_instance->setGeosetSelections(geoset_selection.visible_ids,
                                                  geoset_selection.controlled_families);
      }

      if (creature_texture_debug_enabled())
      {
        std::ostringstream geoset_line;
        geoset_line << "guid=" << spawn.guid
                    << " display=" << spawn.display_id
                    << " model='" << spawn.model_path << "'"
                    << " controlledFamilyCount=" << geoset_selection.controlled_families.size()
                    << geoset_selection.debug_summary;
        LogDebug << "Creature geoset selection: " << geoset_line.str() << std::endl;
        append_creature_geoset_trace("selection", geoset_line.str());
      }
    }

    if (creature_texture_debug_enabled()
        && spawn.is_character_model)
    {
      std::ostringstream applied_slots;
      std::ostringstream override_paths;
      bool first = true;
      for (auto const& replacement : spawn.model_instance->replaceTextures())
      {
        if (!first)
        {
          applied_slots << ", ";
        }
        first = false;
        applied_slots << replacement.first;
      }

      for (std::size_t index = 0; index < overrides.size(); ++index)
      {
        if (index != 0)
        {
          override_paths << ", ";
        }

        override_paths << overrides[index].first << ":'" << overrides[index].second << "'";
      }

      std::ostringstream replacements_line;
      replacements_line << "guid=" << spawn.guid
                        << " display=" << spawn.display_id
                        << " model='" << spawn.model_path << "'"
                        << " overrideCount=" << overrides.size()
                        << " appliedSlots=[" << applied_slots.str() << "]"
                        << " overridePaths=[" << override_paths.str() << "]"
                        << " controlledFamilyCount=" << geoset_controlled_family_count
                        << geoset_debug_summary;

      LogDebug << "Creature instance replacements: " << replacements_line.str() << std::endl;
      append_creature_geoset_trace("instance", replacements_line.str());
    }

    return true;
  }
  catch (std::exception const& ex)
  {
    spawn.model_create_failed = true;
    LogDebug << "Creature spawn model load failed: guid=" << spawn.guid
             << " display_id=" << spawn.display_id
             << " path=" << spawn.model_path
             << " error=" << ex.what() << std::endl;
  }

  return false;
}

bool World::ensureGameObjectSpawnModel(GameObjectSpawnOverlay& spawn)
{
  if (spawn.model_instance.has_value())
  {
    return true;
  }

  if (spawn.model_create_failed || spawn.model_path.empty())
  {
    return false;
  }

  if (spawn.model_path.ends_with(".wmo"))
  {
    spawn.model_create_failed = true;
    LogDebug << "Gameobject WMO overlay skipped: guid=" << spawn.guid
             << " entry=" << spawn.entry
             << " display_id=" << spawn.display_id
             << " path=" << spawn.model_path
             << std::endl;
    return false;
  }

  try
  {
    BlizzardArchive::Listfile::FileKey const file_key(spawn.model_path);
    spawn.model_instance.emplace(file_key, _context);
    spawn.model_instance->pos = spawn.pos;
    spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
    spawn.model_instance->scale = std::clamp(spawn.template_scale,
                                             ModelInstance::min_scale(),
                                             ModelInstance::max_scale());
    spawn.model_instance->updateTransformMatrix();
    return true;
  }
  catch (std::exception const& ex)
  {
    spawn.model_create_failed = true;
    LogDebug << "Gameobject spawn model load failed: guid=" << spawn.guid
             << " entry=" << spawn.entry
             << " display_id=" << spawn.display_id
             << " path=" << spawn.model_path
             << " error=" << ex.what() << std::endl;
  }

  return false;
}

std::vector<std::pair<std::size_t, std::string>> World::applyCreatureSpawnModelAppearance(CreatureSpawnOverlay const& spawn,
                                                                                         ModelInstance& model_instance,
                                                                                         Noggit::NoggitRenderContext context) const
{
  auto const overrides = spawn.is_character_model
    ? resolve_creature_texture_overrides(spawn.display_id, context)
    : resolve_creature_texture_overrides_stable(spawn.display_id);

  for (auto const& override_entry : overrides)
  {
    model_instance.setReplaceTexture(override_entry.first, override_entry.second);
  }

  // CreatureModelAlpha (display record): the client draws the creature at this opacity. For translucent
  // creatures (ghosts, arcane elementals like Anomalus -> alpha 200) it makes an energy-on-black body
  // read as see-through dark energy instead of a SOLID BLACK shape -- which is what Noggit drew, because
  // it always rendered the creature fully opaque. Stored on the instance and applied in ModelRenderPass.
  model_instance.model_alpha = 1.0f;
  if (spawn.display_id)
  {
    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(spawn.display_id);
      float const a = static_cast<float>(display.getUInt(CreatureDisplayInfoDB::CreatureModelAlpha)) / 255.0f;
      if (std::isfinite(a) && a > 0.0f && a < 1.0f)
      {
        model_instance.model_alpha = a;
      }
    }
    catch (...) {}
  }

  if (enable_creature_spawn_character_geosets
      && creature_spawn_geosets_enabled()
      && spawn.is_character_model)
  {
    auto const geoset_selection = resolve_creature_geoset_selection(spawn.display_id);
    if (!geoset_selection.empty())
    {
      model_instance.setGeosetSelections(geoset_selection.visible_ids,
                                         geoset_selection.controlled_families);
    }
  }

  return overrides;
}

void World::clearCreatureSpawns()
{
  _creature_spawns.clear();
  _gameobject_spawns.clear();
  _creature_spawns_loaded = false;
}

void World::ensureCreatureSpawnsLoaded()
{
  if (!_creature_spawns_loaded && !_creature_spawns_load_attempted)
  {
    reloadCreatureSpawns();
  }
}

void World::ensureCreaturePatrolPathsLoaded()
{
  if (_patrol_paths_load_attempted)
  {
    return;
  }
  _patrol_paths_load_attempted = true;

#ifdef USE_MYSQL_UID_STORAGE
  std::string error;
  auto points = mysql::getCreaturePatrolPaths(mapIndex._map_id, &error);
  bool const global_wmo = mapIndex.hasAGlobalWMO();

  for (auto const& wp : points)
  {
    _creature_patrol_paths[wp.guid].push_back(
      server_to_client_position(wp.position_x, wp.position_y, wp.position_z, global_wmo));
  }

  LogDebug << "Creature patrol paths loaded: " << _creature_patrol_paths.size() << " path(s), "
           << points.size() << " waypoint(s)"
           << (error.empty() ? std::string() : (std::string(" (error: ") + error + ")")) << std::endl;
#endif
}

std::size_t World::creatureSpawnModelCount() const
{
  return static_cast<std::size_t>(std::count_if(_creature_spawns.begin(), _creature_spawns.end(),
    [](CreatureSpawnOverlay const& spawn)
    {
      return !spawn.model_path.empty() && !spawn.model_create_failed;
    }));
}

std::size_t World::dirtyCreatureSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_creature_spawns.begin(), _creature_spawns.end(),
    [](CreatureSpawnOverlay const& spawn)
    {
      return spawn.dirty;
    }));
}

World::CreatureSpawnOverlay* World::findCreatureSpawn(std::uint32_t guid)
{
  auto it = std::find_if(_creature_spawns.begin(), _creature_spawns.end(),
    [guid](CreatureSpawnOverlay const& spawn)
    {
      return spawn.guid == guid;
    });

  return it != _creature_spawns.end() ? &*it : nullptr;
}

World::CreatureSpawnOverlay const* World::findCreatureSpawn(std::uint32_t guid) const
{
  auto it = std::find_if(_creature_spawns.begin(), _creature_spawns.end(),
    [guid](CreatureSpawnOverlay const& spawn)
    {
      return spawn.guid == guid;
    });

  return it != _creature_spawns.end() ? &*it : nullptr;
}

void World::ensureGameObjectSpawnsLoaded()
{
  // GameObjects are loaded together with creatures, so just make sure that load has run.
  if (!_creature_spawns_loaded && !_creature_spawns_load_attempted)
  {
    reloadCreatureSpawns();
  }
}

std::size_t World::gameObjectSpawnModelCount() const
{
  return static_cast<std::size_t>(std::count_if(_gameobject_spawns.begin(), _gameobject_spawns.end(),
    [](GameObjectSpawnOverlay const& spawn)
    {
      return !spawn.model_path.empty() && !spawn.model_create_failed;
    }));
}

std::size_t World::dirtyGameObjectSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_gameobject_spawns.begin(), _gameobject_spawns.end(),
    [](GameObjectSpawnOverlay const& spawn)
    {
      return spawn.dirty;
    }));
}

World::GameObjectSpawnOverlay* World::findGameObjectSpawn(std::uint32_t guid)
{
  auto it = std::find_if(_gameobject_spawns.begin(), _gameobject_spawns.end(),
    [guid](GameObjectSpawnOverlay const& spawn)
    {
      return spawn.guid == guid;
    });

  return it != _gameobject_spawns.end() ? &*it : nullptr;
}

World::GameObjectSpawnOverlay const* World::findGameObjectSpawn(std::uint32_t guid) const
{
  auto it = std::find_if(_gameobject_spawns.begin(), _gameobject_spawns.end(),
    [guid](GameObjectSpawnOverlay const& spawn)
    {
      return spawn.guid == guid;
    });

  return it != _gameobject_spawns.end() ? &*it : nullptr;
}

void World::LoadSavedSelectionGroups()
{
  _selection_groups.clear();

  auto& saved_map_groups = Noggit::Project::CurrentProject::get()->ObjectSelectionGroups;
  for (auto& map_group : saved_map_groups)
  {
      if (map_group.MapId == mapIndex._map_id)
      {
          for (auto& group : map_group.SelectionGroups)
          {
              selection_group selectionGroup(group, this);
              _selection_groups.push_back(selectionGroup);
          }
          return;
      }
  }
}

void World::saveSelectionGroups()
{
    auto proj_selection_map_group = Noggit::Project::NoggitProjectSelectionGroups();
    proj_selection_map_group.MapId = mapIndex._map_id;
    for (auto& selection_group : _selection_groups)
    {
        proj_selection_map_group.SelectionGroups.push_back(selection_group.getMembers());
    }

    Noggit::Project::CurrentProject::get()->saveObjectSelectionGroups(proj_selection_map_group);
}

void World::update_selection_pivot()
{
  ZoneScoped;
  if (has_multiple_model_selected())
  {
    glm::vec3 pivot = glm::vec3(0);
    int model_count = 0;

    for (auto const& entry : _current_selection)
    {
      if (entry.index() == eEntry_Object)
      {
        pivot += std::get<selected_object_type>(entry)->pos;
        model_count++;
      }
    }

    _multi_select_pivot = pivot / static_cast<float>(model_count);
  }
  else
  {
    _multi_select_pivot = std::nullopt;
  }
}

bool World::is_selected(selection_type selection) const
{
  ZoneScoped;
  if (selection.index() != eEntry_Object)
    return false;

  auto which = std::get<selected_object_type>(selection)->which();

  if (which == eMODEL)
  {
    uint uid = static_cast<ModelInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                                  , _current_selection.end()
                                  , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eMODEL
        && static_cast<ModelInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );

    if (it != _current_selection.end())
    {
      return true;
    }
  }
  else if (which == eWMO)
  {
    uint uid = static_cast<WMOInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                            , _current_selection.end()
                            , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eWMO
        && static_cast<WMOInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );
    if (it != _current_selection.end())
    {
      return true;
    }
  }

  return false;
}

bool World::is_selected(std::uint32_t uid) const
{
  ZoneScoped;
  for (selection_type const& entry : _current_selection)
  {
    if (entry.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(entry);

    if (obj->which() == eWMO)
    {
      if (static_cast<WMOInstance*>(obj)->uid == uid)
      {
        return true;
      }
    }
    else if (obj->which() == eMODEL)
    {
      if (static_cast<ModelInstance*>(obj)->uid == uid)
      {
        return true;
      }
    }
  }

  return false;
}

std::optional<selection_type> World::get_last_selected_model() const
{
  ZoneScoped;
  auto const it
    ( std::find_if ( _current_selection.rbegin()
                   , _current_selection.rend()
                   , [&] (selection_type const& entry)
                     {
                       return entry.index() != eEntry_MapChunk;
                     }
                   )
    );

  return it == _current_selection.rend()
    ? std::optional<selection_type>() : std::optional<selection_type> (*it);
}

std::vector<selected_object_type> const World::get_selected_objects() const
{
    // std::vector<selected_object_type> objects(_selected_model_count);
    std::vector<selected_object_type> objects;
    objects.reserve(_selected_model_count);

    ZoneScoped;
    for (auto& entry : _current_selection)
    {
        if (entry.index() == eEntry_Object)
        {
            auto obj = std::get<selected_object_type>(entry);
            objects.push_back(obj);
        }
    }

    return objects;
}

glm::vec3 getBarycentricCoordinatesAt(
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    const glm::vec3& point,
    const glm::vec3& normal)
{
  glm::vec3 bary;
  // The area of a triangle is

  glm::vec3 aMb = (b - a);
  glm::vec3 cMa = (c - a);
  glm::vec3 bMpoint = (b - point);
  glm::vec3 cMpont = (c - point);
  glm::vec3 aMpoint = (a - point);

  glm::vec3 ABC = glm::cross(aMb ,cMa);
  glm::vec3 PBC = glm::cross(bMpoint, cMpont);
  glm::vec3 PCA = glm::cross(cMpont, aMpoint);

  double areaABC = glm::dot(normal , ABC);
  double areaPBC = glm::dot(normal , PBC);
  double areaPCA = glm::dot(normal , PCA);

  bary.x = areaPBC / areaABC; // alpha
  bary.y = areaPCA / areaABC; // beta
  bary.z = 1.0f - bary.x - bary.y; // gamma

  return bary;
}

void World::rotate_selected_models_randomly(float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
{
  ZoneScoped;
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    math::degrees::vec3& dir = obj->dir;

    float rx = misc::randfloat(minX, maxX);
    float ry = misc::randfloat(minY, maxY);
    float rz = misc::randfloat(minZ, maxZ);

    //Building rotations
    auto heading = math::radians(math::degrees(dir.z))._ * 0.5;
    auto attitude = math::radians(math::degrees(-dir.y))._ * 0.5;
    auto bank = math::radians(math::degrees(dir.x))._ * 0.5;
    // Assuming the angles are in radians.
    double c1 = cos(heading);
    double s1 = sin(heading);
    double c2 = cos(attitude);
    double s2 = sin(attitude);
    double c3 = cos(bank);
    double s3 = sin(bank);
    double c1c2 = c1 * c2;
    double s1s2 = s1 * s2;
    auto w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    auto x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    auto y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    auto z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat baseRotation = glm::quat(x,y,z,w);

    //Building rotations
    heading = math::radians(math::degrees(rx))._ * 0.5;
    attitude = math::radians(math::degrees(ry))._ * 0.5;
    bank = math::radians(math::degrees(rx))._ * 0.5;
    // Assuming the angles are in radians.
    c1 = cos(heading);
    s1 = sin(heading);
    c2 = cos(attitude);
    s2 = sin(attitude);
    c3 = cos(bank);
    s3 = sin(bank);
    c1c2 = c1 * c2;
    s1s2 = s1 * s2;
    w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat newRotation = glm::quat(x, y, z, w);
    glm::quat finalRotation = baseRotation * newRotation;
    glm::quat finalRotationNormalized = glm::normalize(finalRotation);

    auto eulerAngles = glm::eulerAngles(finalRotationNormalized);
    dir.x = math::degrees(math::radians(eulerAngles.z))._;
    dir.y = math::degrees(math::radians(eulerAngles.x))._;
    dir.z = math::degrees(math::radians(eulerAngles.y))._;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
}


void World::rotate_selected_models_to_ground_normal(bool smoothNormals)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(entry, model_update::remove);

    glm::vec3 rayPos = obj->pos;
    math::degrees::vec3& dir = obj->dir;


    selection_result results;
    for_chunk_at(rayPos, [&](MapChunk* chunk)
    {
        {
          math::ray intersect_ray(rayPos, glm::vec3(0.f, -1.f, 0.f));
          chunk->intersect(intersect_ray, &results);
        }
        // object is below ground
        if (results.empty())
        {
          math::ray intersect_ray(rayPos, glm::vec3(0.f, 1.f, 0.f));
          chunk->intersect(intersect_ray, &results);
        }
    });

    // !\ todo We shouldn't end up with empty ever (but we do, on completely flat ground)
    if (results.empty())
    {
      // just to avoid models disappearing when this happens
      updateTilesEntry(entry, model_update::add);
      continue;
    }


// We hit the terrain, now we take the normal of this position and use it to get the rotation we want.
    auto const& hitChunkInfo = std::get<selected_chunk_type>(results.front().second);

    glm::quat q;
    glm::vec3 varnormal;

    // Surface Normal
    auto &p0 = hitChunkInfo.chunk->mVertices[std::get<0>(hitChunkInfo.triangle)];
    auto &p1 = hitChunkInfo.chunk->mVertices[std::get<1>(hitChunkInfo.triangle)];
    auto &p2 = hitChunkInfo.chunk->mVertices[std::get<2>(hitChunkInfo.triangle)];

    glm::vec3 v1 = p1 - p0;
    glm::vec3 v2 = p2 - p0;

    auto tmpVec = glm::cross(v2 ,v1);
    varnormal.x = tmpVec.z;
    varnormal.y = tmpVec.y;
    varnormal.z = tmpVec.x;

    // Smooth option, gradient the normal towards closest vertex
    if (smoothNormals) // Vertex Normal
    {
      auto normalWeights = getBarycentricCoordinatesAt(p0, p1, p2, hitChunkInfo.position, varnormal);

      auto& tile_buffer = hitChunkInfo.chunk->mt->getChunkHeightmapBuffer();
      int chunk_start = (hitChunkInfo.chunk->px * 16 + hitChunkInfo.chunk->py) * mapbufsize * 4;

      const auto& vNormal0 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<0>(hitChunkInfo.triangle) * 4]);
      const auto& vNormal1 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<1>(hitChunkInfo.triangle) * 4]);
      const auto& vNormal2 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<2>(hitChunkInfo.triangle) * 4]);

      varnormal.x =
          vNormal0.x * normalWeights.x +
          vNormal1.x * normalWeights.y +
          vNormal2.x * normalWeights.z;

      varnormal.y =
          vNormal0.y * normalWeights.x +
          vNormal1.y * normalWeights.y +
          vNormal2.y * normalWeights.z;

      varnormal.z =
          vNormal0.z * normalWeights.x +
          vNormal1.z * normalWeights.y +
          vNormal2.z * normalWeights.z;
    }


    glm::vec3 worldUp = glm::vec3(0, 1, 0);
    glm::vec3 a =glm::cross(worldUp ,varnormal);

    q.x = a.x;
    q.y = a.y;
    q.z = a.z;

    auto worldLengthSqrd = glm::length(worldUp) * glm::length(worldUp);
    auto normalLengthSqrd = glm::length(varnormal) * glm::length(varnormal);
    auto worldDotNormal = glm::dot(worldUp, varnormal);

    q.w = std::sqrt((worldLengthSqrd * normalLengthSqrd) + (worldDotNormal));

    auto normalizedQ = glm::normalize(q);

    //math::degrees::vec3 new_dir;
    // To euler, because wow
      /*
      // roll (x-axis rotation)
      double sinr_cosp = 2.0 * (q.w * q.x + q.y * q.z);
      double cosr_cosp = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
      new_dir.z = std::atan2(sinr_cosp, cosr_cosp) * 180.0f / math::constants::pi;

      // pitch (y-axis rotation)
      double sinp = 2.0 * (q.w * q.y - q.z * q.x);
      if (std::abs(sinp) >= 1)
        new_dir.y = std::copysign(math::constants::pi / 2, sinp) * 180.0f / math::constants::pi; // use 90 degrees if out of range
      else
        new_dir.y = std::asin(sinp) * 180.0f / math::constants::pi;

      // yaw (z-axis rotation)
      double siny_cosp = 2.0 * (q.w * q.z + q.x * q.y);
      double cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
      new_dir.x = std::atan2(siny_cosp, cosy_cosp) * 180.0f / math::constants::pi;
     }*/

    auto eulerAngles = glm::eulerAngles(normalizedQ);
    dir.x = math::degrees(math::radians(eulerAngles.z))._; //Roll
    dir.y = math::degrees(math::radians(eulerAngles.x))._; //Pitch
    dir.z = math::degrees(math::radians(eulerAngles.y))._; //Yaw

    std::get<selected_object_type>(entry)->recalcExtents();

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::set_current_selection(selection_type entry)
{
  ZoneScoped;
  reset_selection();
  add_to_selection(entry);
}

void World::add_to_selection(selection_type entry, bool skip_group)
{
  ZoneScoped;
  if (entry.index() == eEntry_Object)
  {
    _selected_model_count++;

    // check if it is in a group
    if (!skip_group)
    {
        auto obj = std::get<selected_object_type>(entry);
        for (auto& group : _selection_groups)
        {
            if (group.contains_object(obj))
            {
                // this then calls add_to_selection() with skip_group = true to avoid repetition
                group.select_group();
                break;
            }
        }
    }
  }
  _current_selection.push_back(entry);
  update_selection_pivot();
}

void World::remove_from_selection(selection_type entry, bool skip_group)
{
  ZoneScoped;
  std::vector<selection_type>::iterator position = std::find(_current_selection.begin(), _current_selection.end(), entry);
  if (position != _current_selection.end())
  {
    if (entry.index() == eEntry_Object)
    {
      _selected_model_count--;

      // check if it is in a group
      if (!skip_group)
      {
        auto obj = std::get<selected_object_type>(entry);
        for (auto& group : _selection_groups)
        {
          if (group.contains_object(obj))
          {
              // this then calls remove_from_selection() with skip_group = true to avoid repetition
              group.unselect_group();
              break;
          }
        }
      }
    }

    _current_selection.erase(position);
    update_selection_pivot();
  }
}

void World::remove_from_selection(std::uint32_t uid, bool skip_group)
{
  ZoneScoped;
  for (auto it = _current_selection.begin(); it != _current_selection.end(); ++it)
  {
    if (it->index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(*it);

    if (obj->uid == uid)
    {
        _selected_model_count--;
        _current_selection.erase(it);

        // check if it is in a group
        if (!skip_group)
        {
            for (auto& group : _selection_groups)
            {
                if (group.contains_object(obj))
                {
                    // this then calls remove_from_selection() with skip_group = true to avoid repetition
                    group.unselect_group();
                    break;
                }
            }
        }

        update_selection_pivot();
        return;
    }


  }
}

void World::reset_selection()
{
  ZoneScoped;
  _current_selection.clear();
  _multi_select_pivot = std::nullopt;
  _selected_model_count = 0;

  for (auto& selection_group : _selection_groups)
  {
      selection_group.setUnselected();
  }
}

void World::delete_selected_models()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  // erase selected groups as well
  for (auto& group : _selection_groups)
  {
      if (group.isSelected())
      {
          group.remove_group();
      }
  }

  _model_instance_storage.delete_instances(_current_selection);
  need_model_updates = true;
  reset_selection();
}

glm::vec3 World::get_ground_height(glm::vec3 pos)
{
    selection_result hits;
    
    for_chunk_at(pos, [&](MapChunk* chunk)
    {
        {
            math::ray intersect_ray(pos, glm::vec3(0.f, -1.f, 0.f));
            chunk->intersect(intersect_ray, &hits);
        }
        // object is below ground
        if (hits.empty())
        {
            math::ray intersect_ray(pos, glm::vec3(0.f, 1.f, 0.f));
            chunk->intersect(intersect_ray, &hits);
        }
    });

    // this should never happen
    if (hits.empty())
    {
        LogError << "Snap to ground ray intersection failed" << std::endl;
        return glm::vec3(0);
    }

    return std::get<selected_chunk_type>(hits[0].second).position;
}

void World::snap_selected_models_to_the_ground()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    // the ground can only be intersected once
    pos.y = get_ground_height(pos).y;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }

  update_selection_pivot();
  update_selected_model_groups();
}

void World::scale_selected_models(float v, m2_scaling_type type)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    if (entry.index() == eEntry_Object)
    {
      auto obj = std::get<selected_object_type>(entry);

      if (obj->which() != eMODEL)
        continue;

      ModelInstance* mi = static_cast<ModelInstance*>(obj);

      NOGGIT_CUR_ACTION->registerObjectTransformed(mi);

      float scale = mi->scale;

      switch (type)
      {
        case World::m2_scaling_type::set:
          scale = v;
          break;
        case World::m2_scaling_type::add:
          scale += v;
          break;
        case World::m2_scaling_type::mult:
          scale *= v;
          break;
      }

      // if the change is too small, do nothing
      if (std::abs(scale - mi->scale) < ModelInstance::min_scale())
      {
        continue;
      }

      updateTilesModel(mi, model_update::remove);
      mi->scale = std::min(ModelInstance::max_scale(), std::max(ModelInstance::min_scale(), scale));
      mi->recalcExtents();
      updateTilesModel(mi, model_update::add);
    }
  }
  update_selected_model_groups();
}

void World::move_selected_models(float dx, float dy, float dz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }

  update_selection_pivot();
  update_selected_model_groups();
}

void World::move_model(selection_type entry, float dx, float dy, float dz)
{
    ZoneScoped;
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
        return;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);

}

void World::set_selected_models_pos(glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  // move models relative to the pivot when several are selected
  if (has_multiple_model_selected())
  {
    glm::vec3 diff = pos - _multi_select_pivot.value();

    if (change_height)
    {
      move_selected_models(diff);
    }
    else
    {
      move_selected_models(diff.x, 0.f, diff.z);
    }

    return;
  }

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    obj->pos = pos;
    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }

  update_selection_pivot();
  update_selected_model_groups();
}

void World::set_model_pos(selection_type entry, glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  auto type = entry.index();
  if (type == eEntry_MapChunk)
  {
      return;
  }
  
  updateTilesEntry(entry, model_update::remove);
  
  auto& obj = std::get<selected_object_type>(entry);
  NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
  obj->pos = pos;
  obj->recalcExtents();
  
  updateTilesEntry(entry, model_update::add);
}

void World::rotate_selected_models(math::degrees rx, math::degrees ry, math::degrees rz, bool use_pivot)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 dir_change(rx._, ry._, rz._);
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    if (use_pivot && has_multi_select)
    {
      glm::vec3& pos = obj->pos;
      math::degrees::vec3& dir = obj->dir;
      glm::vec3 diff_pos = pos - _multi_select_pivot.value();

      glm::quat rotationQuat = glm::quat(glm::vec3(glm::radians(rx._), glm::radians(ry._), glm::radians(rz._)));
      glm::vec3 rot_result = glm::toMat4(rotationQuat) * glm::vec4(diff_pos,0);

      pos += rot_result - diff_pos;
    }
    else
    {
      math::degrees::vec3& dir = obj->dir;
      dir += dir_change;
    }

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::set_selected_models_rotation(math::degrees rx, math::degrees ry, math::degrees rz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 new_dir(rx._, ry._, rz._);

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type != eEntry_Object)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(entry, model_update::remove);

    math::degrees::vec3& dir = obj->dir;

    dir = new_dir;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::update_selected_model_groups()
{
  for (auto& selection_group : _selection_groups)
  {
      if (selection_group.isSelected())
          selection_group.recalcExtents();
  }
}

MapChunk* World::getChunkAt(glm::vec3 const& pos)
{
  MapTile* tile(mapIndex.getTile(pos));
  if (tile && tile->finishedLoading() && !tile->loading_failed())
  {
    return tile->getChunk((pos.x - tile->xbase) / CHUNKSIZE, (pos.z - tile->zbase) / CHUNKSIZE);
  }
  return nullptr;
}

bool World::isInIndoorWmoGroup(std::array<glm::vec3, 2> obj_bounds, glm::mat4x4 obj_transform)
{
    bool is_indoor = false;
    // check if model bounds is within wmo bounds then check each indor wmo group bounds
    _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
        {
            auto wmo_extents = wmo_instance.getExtents();
            // check if global wmo bounds intersect
            if (obj_bounds[1].x >= wmo_extents[0].x
                && obj_bounds[1].y >= wmo_extents[0].y
                && obj_bounds[1].z >= wmo_extents[0].z
                && wmo_extents[1].x >= obj_bounds[0].x
                && wmo_extents[1].y >= obj_bounds[0].y
                && wmo_extents[1].z >= obj_bounds[0].z)

            {
                for (int i = 0; i < (int)wmo_instance.wmo->groups.size(); ++i)
                {
                    auto const& group = wmo_instance.wmo->groups[i];

                    if (group.is_indoor())
                    {
                        // must call getGroupExtent() to initialize wmo_instance.group_extents
                        // TODO : clear group extents to free memory ?
                      auto const& all_group_extents = wmo_instance.getGroupExtents();
                      auto group_extents_it = all_group_extents.find(i);
                      if (group_extents_it == all_group_extents.end())
                      {
                        continue;
                      }

                      auto& group_extents = group_extents_it->second;

                        // TODO : do a precise calculation instead of using axis aligned bounding boxes.
                        bool aabb_test = obj_bounds[1].x >= group_extents.first.x
                            && obj_bounds[1].y >= group_extents.first.y
                            && obj_bounds[1].z >= group_extents.first.z
                            && group_extents.second.x >= obj_bounds[0].x
                            && group_extents.second.y >= obj_bounds[0].y
                            && group_extents.second.z >= obj_bounds[0].z;

                        if (aabb_test) // oriented box check
                        {
                            /* TODO
                            if (collide_test)
                            {
                                is_indoor = true;
                                return;
                            }
                            */
                        }
                    }
                }
            }
        });

    return is_indoor;
}

selection_result World::intersect (glm::mat4x4 const& model_view
                                  , math::ray const& ray
                                  , bool pOnlyMap
                                  , bool do_objects
                                  , bool draw_terrain
                                  , bool draw_wmo
                                  , bool draw_models
                                  , bool draw_hidden_models
                                  , bool draw_wmo_exterior
                                  )
{
  ZoneScopedN("World::intersect()");
  selection_result results;

  if (draw_terrain)
  {
    ZoneScopedN("World::intersect() : intersect terrain");

    for (auto& pair : _loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      TileIndex index{ static_cast<std::size_t>(pair.first.first)
                        , static_cast<std::size_t>(pair.first.second) };

      // handle tiles that got unloaded mid-frame to avoid illegal access
      if (!mapIndex.tileLoaded(index) || mapIndex.tileAwaitingLoading(index))
          continue;

      if (!tile->finishedLoading() || tile->loading_failed())
        continue;

      if (tile->intersect(ray, &results))
        break;
    }
  }

  if (!pOnlyMap && do_objects)
  {
    if (draw_models)
    {
      ZoneScopedN("World::intersect() : intersect M2s");
      _model_instance_storage.for_each_m2_instance([&] (ModelInstance& model_instance)
      {
        if (draw_hidden_models || !model_instance.model->is_hidden())
        {
          model_instance.intersect(model_view, ray, &results, animtime);
        }
      });
    }

    if (draw_wmo)
    {
      ZoneScopedN("World::intersect() : intersect WMOs");
      _model_instance_storage.for_each_wmo_instance([&] (WMOInstance& wmo_instance)
      {
        if (draw_hidden_models || !wmo_instance.wmo->is_hidden())
        {
          wmo_instance.intersect(ray, &results, draw_wmo_exterior);
        }
      });
    }
  }

  return std::move(results);
}

void World::update_models_emitters(float dt)
{
  ZoneScoped;
  // Remember the per-frame emitter dt so the render pass can advance each creature spawn's OWN particle
  // state at its own animation phase (draw_map runs BEFORE tick, so it reads the previous frame's dt --
  // negligible since dt is ~constant frame to frame).
  _models_emitter_dt = dt;
  while (dt > 0.1f)
  {
    ModelManager::updateEmitters(0.1f);
    dt -= 0.1f;
  }
  ModelManager::updateEmitters(dt);
}

unsigned int World::getAreaID (glm::vec3 const& pos)
{
  ZoneScoped;
  return for_maybe_chunk_at (pos, [&] (MapChunk* chunk) { return chunk->getAreaID(); }).value_or(-1);
}

bool World::camera_is_underwater(glm::vec3 const& pos)
{
  return for_maybe_chunk_at(pos, [&](MapChunk* chunk) -> bool
  {
    ChunkWater* water = chunk->liquid_chunk();
    if (!water)
    {
      return false;
    }
    auto* layers = water->getLayers();
    if (!layers || layers->empty())
    {
      return false;
    }
    // Underwater when the camera sits below the liquid surface. getMaxHeight() is the surface for flat
    // water (the common case); on rare sloped liquid it errs slightly toward "underwater".
    return pos.y < water->getMaxHeight();
  }).value_or(false);
}

unsigned int World::getZoneId(glm::vec3 const& pos)
{
  unsigned int area = getAreaID(pos);
  if (area == static_cast<unsigned int>(-1) || area == 0)
  {
    return area;
  }

  try
  {
    if (gAreaDB.getFieldCount() <= AreaDB::Region)
    {
      return area;
    }
    // Walk ParentAreaID up to the top-level zone (parent == 0).
    for (int guard = 0; guard < 16; ++guard)
    {
      if (!gAreaDB.CheckIfIdExists(area))
      {
        break;
      }
      unsigned int const parent = gAreaDB.getByID(area).getUInt(AreaDB::Region);
      if (parent == 0)
      {
        break;
      }
      area = parent;
    }
  }
  catch (...) {}

  return area;
}

bool World::getInteriorFog(glm::vec3 const& pos, glm::vec3& out_color, float& out_start, float& out_end)
{
  auto contains = [](std::pair<glm::vec3, glm::vec3> const& extents, glm::vec3 const& point)
  {
    return point.x >= extents.first.x && point.x <= extents.second.x
        && point.y >= extents.first.y && point.y <= extents.second.y
        && point.z >= extents.first.z && point.z <= extents.second.z;
  };

  bool found = false;
  _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
  {
    if (found || !wmo_instance.finishedLoading() || wmo_instance.wmo->loading_failed())
    {
      return;
    }

    auto const& wmo_extents = wmo_instance.getExtents();
    if (!contains({wmo_extents[0], wmo_extents[1]}, pos))
    {
      return;
    }

    bool inside_group = false;
    for (auto const& group_extents : wmo_instance.getGroupExtents())
    {
      if (contains(group_extents.second, pos))
      {
        inside_group = true;
        break;
      }
    }
    if (!inside_group)
    {
      return;
    }

    // Only ENCLOSED WMOs (no exterior groups) override the scene fog. An open WMO such as a city
    // (Stormwind) has exterior groups and must keep the outdoor fog -- otherwise its interior MFOG
    // floods the whole open city with dense/bright fog (white-out).
    for (auto const& g : wmo_instance.wmo->groups)
    {
      if (g.is_exterior() || g.is_exterior_lit())
      {
        return;
      }
    }

    for (auto const& wf : wmo_instance.wmo->fogs)
    {
      if (wf.fogend > 1.0f)
      {
        out_color = glm::vec3(wf.color);
        out_start = wf.fogstart;
        out_end = wf.fogend;
        found = true;
        return;
      }
    }
  }, [&]()
  {
    return found;
  });

  return found;
}

unsigned int World::getWMOAreaID(glm::vec3 const& pos)
{
  ZoneScoped;

  auto contains = [](std::pair<glm::vec3, glm::vec3> const& extents, glm::vec3 const& point)
  {
    return point.x >= extents.first.x && point.x <= extents.second.x
        && point.y >= extents.first.y && point.y <= extents.second.y
        && point.z >= extents.first.z && point.z <= extents.second.z;
  };

  auto find_area = [](std::uint32_t wmo_id, std::uint16_t name_set, int group_id) -> unsigned int
  {
    for (DBCFile::Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
      if (i->getUInt(WMOAreaTableDB::WmoId) == wmo_id
          && i->getUInt(WMOAreaTableDB::NameSetId) == name_set
          && i->getInt(WMOAreaTableDB::WMOGroupID) == group_id)
      {
        unsigned int const area_id = i->getUInt(WMOAreaTableDB::AreaTableRefId);
        if (area_id != 0)
        {
          return area_id;
        }
      }
    }

    return static_cast<unsigned int>(-1);
  };

  unsigned int area_id = static_cast<unsigned int>(-1);

  _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
  {
    if (!wmo_instance.finishedLoading() || wmo_instance.wmo->loading_failed())
    {
      return;
    }

    auto const& wmo_extents = wmo_instance.getExtents();
    if (!contains({wmo_extents[0], wmo_extents[1]}, pos))
    {
      return;
    }

    auto const& extents = wmo_instance.getGroupExtents();
    int default_group_id = -1;

    for (auto const& group_extents : extents)
    {
      if (!contains(group_extents.second, pos))
      {
        continue;
      }

      auto const group_index = group_extents.first;
      if (group_index >= 0 && group_index < static_cast<int>(wmo_instance.wmo->groups.size()))
      {
        default_group_id = static_cast<int>(wmo_instance.wmo->groups[group_index].wmo_area_table_group_id());
        area_id = find_area(wmo_instance.wmo->WmoId, wmo_instance.mNameset, default_group_id);
        if (area_id == static_cast<unsigned int>(-1)
            && default_group_id != group_index)
        {
          area_id = find_area(wmo_instance.wmo->WmoId, wmo_instance.mNameset, group_index);
        }
        if (area_id != static_cast<unsigned int>(-1))
        {
          return;
        }
      }
    }

    area_id = find_area(wmo_instance.wmo->WmoId, wmo_instance.mNameset, -1);
  }, [&]()
  {
    return area_id != static_cast<unsigned int>(-1);
  });

  return area_id;
}

unsigned int World::getWMOZoneMusic(glm::vec3 const& pos)
{
  // Mirrors getWMOAreaID's group resolution, but returns the matched WMOAreaTable row's ZoneMusic
  // (column 7) -- WMO interiors (dungeons/caves) carry their music here, not in AreaTable.
  auto contains = [](std::pair<glm::vec3, glm::vec3> const& extents, glm::vec3 const& point)
  {
    return point.x >= extents.first.x && point.x <= extents.second.x
        && point.y >= extents.first.y && point.y <= extents.second.y
        && point.z >= extents.first.z && point.z <= extents.second.z;
  };

  auto find_music = [](std::uint32_t wmo_id, std::uint16_t name_set, int group_id, bool& found) -> unsigned int
  {
    for (DBCFile::Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
      if (i->getUInt(WMOAreaTableDB::WmoId) == wmo_id
          && i->getUInt(WMOAreaTableDB::NameSetId) == name_set
          && i->getInt(WMOAreaTableDB::WMOGroupID) == group_id)
      {
        found = true;
        return i->getUInt(WMOAreaTableDB::ZoneMusic);
      }
    }
    found = false;
    return 0;
  };

  unsigned int music = 0;
  bool resolved = false;

  _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
  {
    if (resolved || !wmo_instance.finishedLoading() || wmo_instance.wmo->loading_failed())
    {
      return;
    }

    auto const& wmo_extents = wmo_instance.getExtents();
    if (!contains({wmo_extents[0], wmo_extents[1]}, pos))
    {
      return;
    }

    // The WMO's outer AABB is loose -- for a big city WMO it can cover a large chunk of nearby
    // terrain. Only treat the camera as "inside" this WMO if it falls within one of the group AABBs;
    // otherwise this isn't the building we're standing in and its music must not apply.
    bool inside_group = false;
    for (auto const& group_extents : wmo_instance.getGroupExtents())
    {
      if (!contains(group_extents.second, pos))
      {
        continue;
      }
      inside_group = true;

      auto const group_index = group_extents.first;
      if (group_index >= 0 && group_index < static_cast<int>(wmo_instance.wmo->groups.size()))
      {
        int const default_group_id = static_cast<int>(wmo_instance.wmo->groups[group_index].wmo_area_table_group_id());
        bool found = false;
        unsigned int zm = find_music(wmo_instance.wmo->WmoId, wmo_instance.mNameset, default_group_id, found);
        if (!found && default_group_id != group_index)
        {
          zm = find_music(wmo_instance.wmo->WmoId, wmo_instance.mNameset, group_index, found);
        }
        if (found && zm > 0)
        {
          music = zm;
          resolved = true;
          return;
        }
      }
    }

    if (!inside_group)
    {
      return; // inside the AABB but not actually inside the building -> not this WMO's music
    }

    {
      bool found = false;
      unsigned int const zm = find_music(wmo_instance.wmo->WmoId, wmo_instance.mNameset, -1, found);
      if (found && zm > 0)
      {
        music = zm;
        resolved = true;
        return;
      }
    }

    // Camera is inside this WMO but the matched group/root rows carry no music. City WMOs (Ironforge,
    // Stormwind) often define the music on a sibling group's WMOAreaTable row, so fall back to the
    // first non-zero ZoneMusic across ALL rows of this WMO -- its whole-building music.
    static bool const s_wmo_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;
    unsigned int whole_wmo_music = 0;
    for (DBCFile::Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
      if (i->getUInt(WMOAreaTableDB::WmoId) != wmo_instance.wmo->WmoId)
      {
        continue;
      }
      unsigned int const row_music = i->getUInt(WMOAreaTableDB::ZoneMusic);
      if (s_wmo_dbg)
      {
        LogError << "ZONEMUSIC   wmoRow wmoId=" << wmo_instance.wmo->WmoId
                 << " nameSet=" << i->getUInt(WMOAreaTableDB::NameSetId)
                 << " groupId=" << i->getInt(WMOAreaTableDB::WMOGroupID)
                 << " areaRef=" << i->getUInt(WMOAreaTableDB::AreaTableRefId)
                 << " zoneMusic=" << row_music << std::endl;
      }
      if (whole_wmo_music == 0 && row_music > 0)
      {
        whole_wmo_music = row_music;
      }
    }

    if (whole_wmo_music > 0)
    {
      music = whole_wmo_music;
    }
    resolved = true; // camera is inside this WMO -> don't keep scanning other instances
  }, [&]()
  {
    return resolved;
  });

  return music;
}

int World::getZoneMusic(glm::vec3 const& pos)
{
  static bool s_zm_dbg = std::getenv("NOGGIT_LIGHT_DEBUG") != nullptr;

  // 1) WMO interior music (WMOAreaTable.ZoneMusic) -- dungeons/caves (Timbermaw, Wailing Caverns).
  unsigned int wmo_music = 0;
  try { wmo_music = getWMOZoneMusic(pos); } catch (...) {}
  if (wmo_music > 0)
  {
    if (s_zm_dbg) { LogError << "ZONEMUSIC via WMOAreaTable -> " << wmo_music << std::endl; }
    return static_cast<int>(wmo_music);
  }

  // 2) Area chains: WMO area then terrain area, walking up ParentAreaID for inherited zone music.
  auto walk = [&](unsigned int area_id) -> int
  {
    try
    {
      if (gAreaDB.getFieldCount() <= AreaDB::ZoneMusic) { return 0; }
      unsigned int a = area_id;
      for (int guard = 0; a != 0 && a != static_cast<unsigned int>(-1) && guard < 16; ++guard)
      {
        if (!gAreaDB.CheckIfIdExists(a)) { break; }
        auto const rec = gAreaDB.getByID(a);
        int const zm = static_cast<int>(rec.getUInt(AreaDB::ZoneMusic));
        unsigned int const parent = rec.getUInt(AreaDB::Region);
        if (s_zm_dbg)
        {
          LogError << "ZONEMUSIC   walk area=" << a << " zoneMusic=" << zm
                   << " ambience=" << rec.getUInt(AreaDB::ZoneMusic - 1)
                   << " parent=" << parent << std::endl;
        }
        if (zm > 0) { return zm; }
        a = parent;
      }
    }
    catch (...) {}
    return 0;
  };

  unsigned int wmo_area = static_cast<unsigned int>(-1);
  unsigned int terrain_area = static_cast<unsigned int>(-1);
  try { wmo_area = getWMOAreaID(pos); } catch (...) {}
  try { terrain_area = getAreaID(pos); } catch (...) {}

  int music = walk(wmo_area);
  if (music == 0 && terrain_area != wmo_area) { music = walk(terrain_area); }

  if (s_zm_dbg)
  {
    // Log only when the resolution changes, so walking into a WMO (e.g. Ironforge) prints one clear
    // line showing exactly which stage produced the music (or where it fell through to terrain).
    static int s_last_wmo_music = -2, s_last_wmo_area = -2, s_last_terrain_area = -2, s_last_music = -2;
    if (static_cast<int>(wmo_music) != s_last_wmo_music || static_cast<int>(wmo_area) != s_last_wmo_area
        || static_cast<int>(terrain_area) != s_last_terrain_area || music != s_last_music)
    {
      LogError << "ZONEMUSIC wmoMusic=" << static_cast<int>(wmo_music)
               << " wmoArea=" << static_cast<int>(wmo_area)
               << " terrainArea=" << static_cast<int>(terrain_area)
               << " -> final=" << music << std::endl;
      s_last_wmo_music = static_cast<int>(wmo_music);
      s_last_wmo_area = static_cast<int>(wmo_area);
      s_last_terrain_area = static_cast<int>(terrain_area);
      s_last_music = music;
    }
  }
  return music;
}

void World::clearHeight(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
    chunk->clearHeight();
  });
  for_all_chunks_on_tile(pos, [this] (MapChunk* chunk) {
      recalc_norms (chunk);
  });
}

void World::clearAllModelsOnADT(TileIndex const& tile)
{
  ZoneScoped;
  _model_instance_storage.delete_instances_from_tile(tile);
  // update_models_by_filename();
}

void World::CropWaterADT(const TileIndex& pos)
{
  ZoneScoped;
  for_tile_at(pos, [](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->CropWater();
  });
}

void World::setAreaID(glm::vec3 const& pos, int id, bool adt, float radius)
{
  ZoneScoped;
  if (adt)
  {
    for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
      chunk->setAreaID(id);
    });
  }
  else
  {

    if (radius >= 0)
    {
      for_all_chunks_in_range(pos, radius,
                              [&] (MapChunk* chunk)
                              {
                                NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
                                chunk->setAreaID(id);
                                return true;
                              }
      );

    }
    else
    {
      for_chunk_at(pos, [&](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
        chunk->setAreaID(id);
      });
    }
  }
}

bool World::GetVertex(float x, float z, glm::vec3 *V) const
{
  ZoneScoped;
  TileIndex tile({x, 0, z});

  if (!mapIndex.tileLoaded(tile))
  {
    return false;
  }

  MapTile* adt = mapIndex.getTile(tile);

  return adt->GetVertex(x, z, V);
}



void World::changeShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->ChangeMCCV(pos, color, change, radius, editMode);
      }
    );
}

void World::stampShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode, QImage* img, bool paint, bool use_image_colors)
{
  ZoneScoped;
  for_all_chunks_in_rect
    ( pos, radius
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->stampMCCV(pos, color, change, radius, editMode, img, paint, use_image_colors);
      }
    );
}

glm::vec3 World::pickShaderColor(glm::vec3 const& pos)
{
  ZoneScoped;
  glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
  for_all_chunks_in_range
  (pos, 0.1f
    , [&] (MapChunk* chunk)
  {
    color = chunk->pickMCCV(pos);
    return true;
  }
  );

  return color;
}

auto World::stamp(glm::vec3 const& pos, float dt, QImage const* img, float radiusOuter
, float radiusInner, int brushType, bool sculpt) -> void
{
  ZoneScoped;
  auto action = NOGGIT_CUR_ACTION;
  float delta = action->getDelta() + dt;
  action->setDelta(delta);

  for_all_chunks_in_rect(pos, radiusOuter,
                          [=](MapChunk* chunk) -> bool
                          {
                            auto action = NOGGIT_CUR_ACTION;
                            action->registerChunkTerrainChange(chunk);
                            action->setBlockCursor(!sculpt);
                            chunk->stamp(pos, dt, img, radiusOuter, radiusInner, brushType, sculpt); return true;
                          }
                          , [this](MapChunk* chunk) -> void
                          {
                            recalc_norms(chunk);

                            // check if coord axis > 0
                            // if true, get chunk by coord axis - 1
                            // else, check if tile coord axis > 0
                            // if true, get tile by tile coord axis - 1,  get last chunk by axis
                            auto get_neighbor =
                              [this, chunk](int px, int py) -> MapChunk*
                              {
                                MapChunk* neighbor{};

                                int new_chunk_x = px + chunk->px;
                                int new_chunk_z = py + chunk->py;

                                if (new_chunk_x < 0 || new_chunk_z < 0 || new_chunk_x == 16 || new_chunk_z == 16)
                                {
                                  TileIndex index(chunk->mt->index.x + px, chunk->mt->index.z + py);
                                  if (index.x != std::numeric_limits<std::size_t>::max()
                                  && index.z != std::numeric_limits<std::size_t>::max()
                                  && index.x != 64
                                  && index.z != 64)
                                  {
                                    MapTile* neighbor_tile = mapIndex.getTile(index);

                                    if (!neighbor_tile)
                                      return nullptr;

                                    neighbor = neighbor_tile->getChunk((new_chunk_x + 16) % 16,
                                                                       (new_chunk_z + 16) % 16);
                                  }
                                }
                                else
                                {
                                  neighbor = chunk->mt->getChunk(new_chunk_x, new_chunk_z);
                                }

                                return neighbor;
                              };

                            if (auto neighbor = get_neighbor(-1, 0); neighbor)
                              chunk->fixGapLeft(neighbor);

                            if (auto neighbor = get_neighbor(0, -1); neighbor)
                              chunk->fixGapAbove(neighbor);

                            if (auto neighbor = get_neighbor(1, 0); neighbor)
                              neighbor->fixGapLeft(chunk);

                            if (auto neighbor = get_neighbor(0, 1); neighbor)
                              neighbor->fixGapAbove(chunk);

                          });
}


void World::changeObjectsWithTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius, bool iter_wmos_, bool iter_m2s)
{
    // applies the terrain brush to the terrain objects hit
    ZoneScoped;

  // Identical code to chunk->changeTerrain()
  //    if (_snap_m2_objects_chkbox->isChecked() || _snap_wmo_objects_chkbox->isChecked()) {
  auto objects_hit = getObjectsInRange(pos, radius, true, iter_wmos_, iter_m2s);

  for (auto obj : objects_hit)
  {

    float dt = change;

    float dist, xdiff, zdiff;
    bool changed = false;

    xdiff = obj->pos.x - pos.x;
    zdiff = obj->pos.z - pos.z;

    if (BrushType == eTerrainType_Quadra)
    {
        if ((std::abs(xdiff) < std::abs(radius / 2)) && (std::abs(zdiff) < std::abs(radius / 2)))
        {
            dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
            dt = dt * (1.0f - dist * inner_radius / radius);
            changed = true;
        }
    }
    else
    {
        dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
        if (dist < radius)
        {
            changed = true;

            switch (BrushType)
            {
            case eTerrainType_Flat:
                break;
            case eTerrainType_Linear:
                dt = dt * (1.0f - dist * (1.0f - inner_radius) / radius);
                break;
            case eTerrainType_Smooth:
                dt = dt / (1.0f + dist / radius);
                break;
            case eTerrainType_Polynom:
                dt = dt * ((dist / radius) * (dist / radius) + dist / radius + 1.0f);
                break;
            case eTerrainType_Trigo:
                dt = dt * cos(dist / radius);
                break;
            case eTerrainType_Gaussian:
                dt = dist < radius * inner_radius ? dt * std::exp(-(std::pow(radius * inner_radius / radius, 2) / (2 * std::pow(0.39f, 2)))) : dt * std::exp(-(std::pow(dist / radius, 2) / (2 * std::pow(0.39f, 2))));

                break;
            default:
                LogError << "Invalid terrain edit type (" << inner_radius << ")" << std::endl;
                changed = false;
                break;
            }
        }
    }
    if (changed)
    {
        move_model(obj, 0.0f, dt, 0.0f);
        // set_model_pos(obj, glm::vec3(obj->pos.x, obj->pos.y + dt, obj->pos.z));
    }
  }
}

void World::changeTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius)
{
  ZoneScoped;

  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->changeTerrain(pos, change, radius, BrushType, inner_radius);
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

std::vector<selected_object_type> World::getObjectsInRange(glm::vec3 const& pos, float radius, bool ignore_height, bool iter_wmos_, bool iter_m2s)
{
    // ignores height by default

    std::vector<selected_object_type> objects_hit_list;

    /* This causes duplicates at tile edges
    for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
    {
        if (!tile->finishedLoading() || tile->loading_failed())
        {
            continue;
        }

        std::vector<uint32_t>* uids = tile->get_uids();

        for (uint32_t uid : *uids)
        {
            auto instance = _model_instance_storage.get_instance(uid);

            */
    if (iter_m2s)
    {
        _model_instance_storage.for_each_m2_instance([&](ModelInstance& model_instance)
            {
                selected_object_type obj = &model_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius) // this is just origin point
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    if (iter_wmos_)
    {
        _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
            {
                selected_object_type obj = &wmo_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius)
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    return objects_hit_list;
}

void World::flattenTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const glm::vec3& origin, math::degrees angle, math::degrees orientation)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->flattenTerrain(pos, remain, radius, BrushType, mode, origin, angle, orientation);
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

std::vector<std::pair<SceneObject*, float>> World::getObjectsGroundDistance(glm::vec3 const& pos, float radius, bool iter_wmos_, bool iter_m2s)
{
    std::vector<std::pair<SceneObject*, float>> objects_ground_distance;

    auto objects_hit = getObjectsInRange(pos, radius, true
        , iter_wmos_, iter_m2s);

    for (auto obj : objects_hit)
    {
        if ((obj->which() == eMODEL && !iter_m2s) || (obj->which() == eWMO && !iter_wmos_))
            continue;
        float height_diff = obj->pos.y - get_ground_height(obj->pos).y;
        objects_ground_distance.push_back(std::pair<SceneObject*, float>(obj, height_diff));
    }

    return objects_ground_distance;
}

void World::blurTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->blurTerrain ( pos
                                  , remain
                                  , radius
                                  , BrushType
                                  , mode
                                  , [this] (float x, float z) -> std::optional<float>
                                    {
                                      glm::vec3 vec;
                                      auto res (GetVertex (x, z, &vec));
                                      return res ? std::optional<float>(vec.y) : std::nullopt;
                                    }
                                  );
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

void World::recalc_norms (MapChunk* chunk) const
{
    ZoneScoped;
    chunk->recalcNorms();
}

bool World::paintTexture(glm::vec3 const& pos, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  return for_all_chunks_in_range
    ( pos, brush->getRadius()
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->paintTexture(pos, brush, strength, pressure, texture);
      }
    );
}

bool World::stampTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture, QImage* img, bool paint)
{
  ZoneScoped;
  return for_all_chunks_in_rect
    ( pos, brush->getRadius()
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->stampTexture(pos, brush, strength, pressure, texture, img, paint);
      }
    );
}

bool World::sprayTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, float spraySize, float sprayPressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  bool succ = false;
  float inc = brush->getRadius() / 4.0f;

  for (float pz = pos.z - spraySize; pz < pos.z + spraySize; pz += inc)
  {
    for (float px = pos.x - spraySize; px < pos.x + spraySize; px += inc)
    {
      if ((sqrt(pow(px - pos.x, 2) + pow(pz - pos.z, 2)) <= spraySize) && ((rand() % 1000) < sprayPressure))
      {
        succ |= paintTexture({px, pos.y, pz}, brush, strength, pressure, texture);
      }
    }
  }

  return succ;
}

bool World::replaceTexture(glm::vec3 const& pos, float radius, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture, bool entire_chunk)
{
  ZoneScoped;
  return for_all_chunks_in_range
    ( pos, radius
      , [&](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->replaceTexture(pos, radius, old_texture, new_texture, entire_chunk);
      }
    );
}

void World::eraseTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_chunk_at(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}

void World::overwriteTextureAtCurrentChunk(glm::vec3 const& pos, scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture)
{
  ZoneScoped;
  for_chunk_at(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->switchTexture(oldTexture, std::move (newTexture));
  });
}

void World::setHole(glm::vec3 const& pos, float radius, bool big, bool hole)
{
  ZoneScoped;
  for_all_chunks_in_range
      ( pos, radius
        , [&](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
          chunk->setHole(pos, radius, big, hole);
          return true;
        }
      );
}

void World::setHoleADT(glm::vec3 const& pos, bool hole)
{
  ZoneScoped;

  for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
    chunk->setHole(pos, 1.0f, true, hole);
  });
}

void World::loadAllTiles()
{
  ZoneScoped;

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();
      }
    }
  }
}

void World::convert_alphamap(bool to_big_alpha)
{
  ZoneScoped;

  if (to_big_alpha == mapIndex.hasBigAlpha())
  {
    return;
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        mTile->convert_alphamap(to_big_alpha);
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }

  mapIndex.convert_alphamap(to_big_alpha);
  mapIndex.save();
}


void World::deleteModelInstance(int uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_model_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteWMOInstance(int uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_wmo_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteInstance(int uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid);
    need_model_updates = true;
    reset_selection();
  }
}

bool World::uid_duplicates_found() const
{
  ZoneScoped;
  return _model_instance_storage.uid_duplicates_found();
}

void World::delete_duplicate_model_and_wmo_instances()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear_duplicates();
  need_model_updates = true;
}

void World::unload_every_model_and_wmo_instance()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear();

  // _models_by_filename.clear();
}

void World::addM2 ( BlizzardArchive::Listfile::FileKey const& file_key
                  , glm::vec3 newPos
                  , float scale
                  , glm::vec3 rotation
                  , Noggit::object_paste_params* paste_params
                  )
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = mapIndex.newGUID();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true);

  // _models_by_filename[file_key.filepath()].push_back(_model_instance_storage.get_model_instance(uid).value());
}

ModelInstance* World::addM2AndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , float scale
    , math::degrees::vec3 rotation
    , Noggit::object_paste_params* paste_params
    , bool ignore_params
)
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = mapIndex.newGUID();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params && !ignore_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true);

  auto instance = _model_instance_storage.get_model_instance(uid).value();
  // _models_by_filename[file_key.filepath()].push_back(instance);

  return instance;
}

void World::addWMO ( BlizzardArchive::Listfile::FileKey const& file_key
                   , glm::vec3 newPos
                   , math::degrees::vec3 rotation
                   )
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = mapIndex.newGUID();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;

  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true);
}

WMOInstance* World::addWMOAndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , math::degrees::vec3 rotation
)
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = mapIndex.newGUID();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;

  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true);

  return _model_instance_storage.get_wmo_instance(uid).value();
}


std::uint32_t World::add_model_instance(ModelInstance model_instance, bool from_reloading)
{
  ZoneScoped;
  return _model_instance_storage.add_model_instance(std::move(model_instance), from_reloading);
}

std::uint32_t World::add_wmo_instance(WMOInstance wmo_instance, bool from_reloading)
{
  ZoneScoped;
  return _model_instance_storage.add_wmo_instance(std::move(wmo_instance), from_reloading);
}

std::optional<selection_type> World::get_model(std::uint32_t uid)
{
  ZoneScoped;
  return _model_instance_storage.get_instance(uid);
}

void World::remove_models_if_needed(std::vector<uint32_t> const& uids)
{
  ZoneScoped;
  // todo: manage instances properly
  // don't unload anything during the uid fix all,
  // otherwise models spanning several adts will be unloaded too soon
  if (mapIndex.uid_fix_all_in_progress())
  {
    return;
  }

  for (uint32_t uid : uids)
  {
    // it handles the removal from the selection if necessary
    _model_instance_storage.unload_instance_and_remove_from_selection_if_necessary(uid);
  }

  // deselect the terrain when an adt is unloaded
  if (_current_selection.size() == 1 && _current_selection.at(0).index() == eEntry_MapChunk)
  {
    reset_selection();
  }
  /*
  if (uids.size())
  {
    update_models_by_filename();
  }*/
}

void World::reload_tile(TileIndex const& tile)
{
  ZoneScoped;
  reset_selection();
  mapIndex.reloadTile(tile);
}

void World::deleteObjects(std::vector<selection_type> const& types)
{
  ZoneScoped;
  _model_instance_storage.delete_instances(types);
  need_model_updates = true;
}

void World::updateTilesEntry(selection_type const& entry, model_update type)
{
  ZoneScoped;
  if (entry.index() != eEntry_Object)
    return;

  auto obj = std::get<selected_object_type>(entry);

  if (obj->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(obj), type);
  else if (obj->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(obj), type);

}


void World::updateTilesEntry(SceneObject* entry, model_update type)
{
  ZoneScoped;
  if (entry->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(entry), type);
  else if (entry->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(entry), type);

}

void World::updateTilesWMO(WMOInstance* wmo, model_update type)
{
  ZoneScoped;
  _tile_update_queue.queue_update(wmo, type);
}

void World::updateTilesModel(ModelInstance* m2, model_update type)
{
  ZoneScoped;
  _tile_update_queue.queue_update(m2, type);
}

void World::wait_for_all_tile_updates()
{
  ZoneScoped;
  _tile_update_queue.wait_for_all_update();
}

unsigned int World::getMapID()
{
  ZoneScoped;
  return mapIndex._map_id;
}

void World::clearTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}


void World::exportADTAlphamap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      for (int i = 1; i < 4; ++i)
      {
        QImage img = tile->getAlphamapImage(i);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
        + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
        + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
      }

    }
  );
}

void World::exportADTNormalmap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {
        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = tile->getNormalmapImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                 + "_normal.png", "PNG");
      }
  );
}

void World::exportADTAlphamap(glm::vec3 const& pos, std::string const& filename)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      QString tex(filename.c_str());
      QImage img = tile->getAlphamapImage(filename);
      img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
               + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
               + "_" + tex.replace("/", "-") + ".png", "PNG");

    }
  );
}

void World::exportADTHeightmap(glm::vec3 const& pos, float min_height, float max_height)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getHeightmapImage(min_height, max_height);
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_height.png", "PNG");


                }
  );
}

void World::exportADTVertexColorMap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getVertexColorsImage();
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol.png", "PNG");


                }
  );
}

void World::importADTAlphamap(glm::vec3 const& pos, QImage const& image, unsigned layer)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  if (image.width() != 1024 || image.height() != 1024)
  {
    QImage scaled = image.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
                  {
                    tile->setAlphaImage(scaled, layer);
                  }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setAlphaImage(image, layer);
      }
    );
  }

}

void World::importADTAlphamap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      for (int i = 1; i < 4; ++i)
      {
        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                       + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                       + "_layer" +  std::to_string(i).c_str() + ".png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        if (img.width() != 1024 || img.height() != 1024)
          img = img.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setAlphaImage(img, i);
      }

    }
  );
}

void World::importADTHeightmap(glm::vec3 const& pos, QImage const& image, float multiplier, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  int desired_dimensions = tiledEdges ? 256 : 257;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
  });

  if (image.width() != desired_dimensions || image.height() != desired_dimensions)
  {
    QImage scaled = image.scaled(desired_dimensions, desired_dimensions, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(scaled, multiplier, mode, tiledEdges);
      }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(image, multiplier, mode, tiledEdges);
      }
    );
  }
}

void World::importADTHeightmap(glm::vec3 const& pos, float multiplier, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {

      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                         + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                         + "_height" + ".png";

      if(!QFileInfo::exists(filename))
        return;

      for_all_chunks_on_tile(pos, [](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
      });

      QImage img;
      img.load(filename, "PNG");

      size_t desiredSize = tiledEdges ? 256 : 257;
      if (img.width() != desiredSize || img.height() != desiredSize)
        img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

      tile->setHeightmapImage(img, multiplier, mode, tiledEdges);

    }
  );
}

void World::importADTVertexColorMap(glm::vec3 const& pos, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol" + ".png";

        if(!QFileInfo::exists(filename))
          return;

        for_all_chunks_on_tile(pos, [](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        });

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
          img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setVertexColorImage(img, mode, tiledEdges);

      }
  );
}

void World::ensureAllTilesetsADT(glm::vec3 const& pos)
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for_all_chunks_on_tile(pos, [=](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);

    for (int i = 0; i < 4; ++i)
    {
      if (chunk->texture_set->num() <= i)
      {
        scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
        chunk->texture_set->addTexture(tex);
      }
    }

  });
}

void World::importADTVertexColorMap(glm::vec3 const& pos, QImage const& image, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
  });

  size_t desiredDimensions = tiledEdges ? 256 : 257;

  if (image.width() != desiredDimensions || image.height() != desiredDimensions)
  {
    QImage scaled = image.scaled(static_cast<int>(desiredDimensions), static_cast<int>(desiredDimensions), Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(scaled, mode, tiledEdges);
        }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(image, mode, tiledEdges);
        }
    );
  }
}

void World::setBaseTexture(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
    if (!!Noggit::Ui::selected_texture::get())
    {
      chunk->addTexture(*Noggit::Ui::selected_texture::get());
    }
  });
}

void World::clear_shadows(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkShadowChange(chunk);
    chunk->clear_shadows();
  });
}

void World::swapTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
  ZoneScoped;
  if (!!Noggit::Ui::selected_texture::get())
  {
    for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
      chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
    });
  }
}

void World::swapTextureGlobal(scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {

        for (size_t z = 0; z < 64; z++)
        {
            for (size_t x = 0; x < 64; x++)
            {
                TileIndex tile(x, z);

                bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
                MapTile* mTile = mapIndex.loadTile(tile);

                if (mTile)
                {
                    mTile->wait_until_loaded();

                    bool tile_changed = false;
                    for_all_chunks_on_tile(mTile, [&](MapChunk* chunk)
                    {
                        // NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                        bool swapped = chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                        if (swapped)
                            tile_changed = true;
                    });

                    if (tile_changed)
                    {
                        mTile->saveTile(this);
                        mapIndex.markOnDisc(tile, true);
                        mapIndex.unsetChanged(tile);
                    }

                    if (unload)
                    {
                        mapIndex.unloadTile(tile);
                    }
                }
            }
        }
    }
}

void World::removeTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {
        for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
            {
                NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                // chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                chunk->eraseTexture(tex);
            });
    }
}


void World::removeTexDuplicateOnADT(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->texture_set->removeDuplicate();
  } );
}

void World::change_texture_flag(glm::vec3 const& pos, scoped_blp_texture_reference const& tex, std::size_t flag, bool add)
{
  ZoneScoped;
  for_chunk_at(pos, [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->change_texture_flag(tex, flag, add);
  });
}

void World::paintLiquid( glm::vec3 const& pos
                       , float radius
                       , int liquid_id
                       , bool add
                       , math::radians const& angle
                       , math::radians const& orientation
                       , bool lock
                       , glm::vec3 const& origin
                       , bool override_height
                       , bool override_liquid_id
                       , float opacity_factor
                       )
{
  ZoneScoped;
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->paintLiquid(pos, radius, liquid_id, add, angle, orientation, lock, origin, override_height, override_liquid_id, chunk, opacity_factor);
    return true;
  });
}

void World::setWaterType(const TileIndex& pos, int type, int layer)
{
  ZoneScoped;
  for_tile_at ( pos
              , [&] (MapTile* tile)
                {
                  for (int i = 0; i < 16; ++i)
                    for (int j = 0; j < 16; ++j)
                      NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

                  tile->Water.setType (type, layer);
                }
              );
}

int World::getWaterType(const TileIndex& tile, int layer)
{
  ZoneScoped;
  if (mapIndex.tileLoaded(tile))
  {
    return mapIndex.getTile(tile)->Water.getType (layer);
  }
  else
  {
    return 0;
  }
}

void World::autoGenWaterTrans(const TileIndex& pos, float factor)
{
  ZoneScoped;
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->Water.autoGen(factor);
  });
}


void World::fixAllGaps()
{
  ZoneScoped;
  std::vector<MapChunk*> chunks;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    MapTile* left = mapIndex.getTileLeft(tile);
    MapTile* above = mapIndex.getTileAbove(tile);
    bool tileChanged = false;

    // fix the gaps with the adt at the left of the current one
    if (left)
    {
      for (unsigned ty = 0; ty < 16; ty++)
      {
        MapChunk* chunk = tile->getChunk(0, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapLeft(left->getChunk(15, ty)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix the gaps with the adt above the current one
    if (above)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, 0);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapAbove(above->getChunk(tx, 15)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix gaps within the adt
    for (unsigned ty = 0; ty < 16; ty++)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        bool changed = false;

        // if the chunk isn't the first of the row
        if (tx && chunk->fixGapLeft(tile->getChunk(tx - 1, ty)))
        {
          changed = true;
        }

        // if the chunk isn't the first of the column
        if (ty && chunk->fixGapAbove(tile->getChunk(tx, ty - 1)))
        {
          changed = true;
        }

        if (changed)
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }
    if (tileChanged)
    {
      mapIndex.setChanged(tile);
    }
  }

  for (MapChunk* chunk : chunks)
  {
    recalc_norms (chunk);
  }
}

bool World::isUnderMap(glm::vec3 const& pos)
{
  ZoneScoped;
  TileIndex const tile (pos);

  if (mapIndex.tileLoaded(tile))
  {
    unsigned chnkX = (pos.x / CHUNKSIZE) - tile.x * 16;
    unsigned chnkZ = (pos.z / CHUNKSIZE) - tile.z * 16;

    // check using the cursor height
    return (mapIndex.getTile(tile)->getChunk(chnkX, chnkZ)->getMinHeight()) > pos.y + 2.0f;
  }

  return true;
}

void World::selectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;

  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk){
    _vertex_chunks.emplace(chunk);
    _vertex_tiles.emplace(chunk->mt);
    chunk->selectVertex(pos, radius, _vertices_selected);
    return true;
  });

}

bool World::deselectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;
  std::unordered_set<glm::vec3*> inRange;

  for (glm::vec3* v : _vertices_selected)
  {
    if (misc::dist(*v, pos) <= radius)
    {
      inRange.emplace(v);
    }
  }

  for (glm::vec3* v : inRange)
  {
    _vertices_selected.erase(v);
  }

  return _vertices_selected.empty();
}

void World::moveVertices(float h)
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "moveVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  _vertex_center_updated = false;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y += h;
  }

  updateVertexCenter();
  updateSelectedVertices();
}

void World::updateSelectedVertices()
{
  ZoneScoped;
  for (MapTile* tile : _vertex_tiles)
  {
    mapIndex.setChanged(tile);
  }

  // fix only the border chunks to be more efficient
  for (MapChunk* chunk : vertexBorderChunks())
  {
    chunk->fixVertices(_vertices_selected);
  }

  for (MapChunk* chunk : _vertex_chunks)
  {
    chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
    recalc_norms (chunk);
  }
}

void World::orientVertices ( glm::vec3 const& ref_pos
                           , math::degrees vertex_angle
                           , math::degrees vertex_orientation
                           )
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "orientVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  for (glm::vec3* v : _vertices_selected)
  {
    v->y = misc::angledHeight(ref_pos, *v, vertex_angle, vertex_orientation);
  }
  updateSelectedVertices();
}

void World::flattenVertices (float height)
{
  ZoneScoped;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y = height;
  }
  updateSelectedVertices();
}

void World::clearVertexSelection()
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();
  _vertex_border_updated = false;
  _vertex_center_updated = false;
  _vertices_selected.clear();
  _vertex_chunks.clear();
  _vertex_tiles.clear();
}

void World::updateVertexCenter()
{
  ZoneScoped;
  _vertex_center_updated = true;
  _vertex_center = { 0,0,0 };
  float f = 1.0f / _vertices_selected.size();
  for (glm::vec3* v : _vertices_selected)
  {
    _vertex_center += (*v) * f;
  }
}

glm::vec3 const& World::vertexCenter()
{
  ZoneScoped;
  if (!_vertex_center_updated)
  {
    updateVertexCenter();
  }

  return _vertex_center;
}

std::unordered_set<MapChunk*>& World::vertexBorderChunks()
{
  ZoneScoped;
  if (!_vertex_border_updated)
  {
    _vertex_border_updated = true;
    _vertex_border_chunks.clear();

    for (MapChunk* chunk : _vertex_chunks)
    {
      if (chunk->isBorderChunk(_vertices_selected))
      {
        _vertex_border_chunks.emplace(chunk);
      }
    }
  }
  return _vertex_border_chunks;
}
/*
void World::update_models_by_filename()
{
  ZoneScoped;
  _models_by_filename.clear();

  _model_instance_storage.for_each_m2_instance([&] (ModelInstance& model_instance)
  {
    _models_by_filename[model_instance.model->file_key().filepath()].push_back(&model_instance);
    // to make sure the transform matrix are up to date
    model_instance.ensureExtents();
  });

  need_model_updates = false;
}
*/
void World::range_add_to_selection(glm::vec3 const& pos, float radius, bool remove)
{
  ZoneScoped;

  auto objects_in_range = getObjectsInRange(pos, radius);

  for (auto obj : objects_in_range)
  {
      if (remove)
      {
          remove_from_selection(obj);
      }
      else
      {
          add_to_selection(obj);
      }
  }
}

float World::getMaxTileHeight(const TileIndex& tile)
{
  ZoneScoped;
  MapTile* m_tile = mapIndex.getTile(tile);

  m_tile->forceRecalcExtents();
  float max_height = m_tile->getMaxHeight();

  std::vector<uint32_t>* uids = m_tile->get_uids();

  for (uint32_t uid : *uids)
  {
    auto instance = _model_instance_storage.get_instance(uid);

    if (instance.value().index() == eEntry_Object)
    {
      auto obj = std::get<selected_object_type>(instance.value());
      obj->ensureExtents();
      max_height = std::max(max_height, std::max(obj->extents[0].y, obj->extents[1].y));
    }
  }


  return max_height;
}

SceneObject* World::getObjectInstance(std::uint32_t uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (!instance)
    return nullptr;

  if (instance.value().index() == eEntry_Object)
  {
    return std::get<selected_object_type>(instance.value());
  }

  return nullptr;
}

void World::setBasename(const std::string &name)
{
  ZoneScoped;
  basename = name;
  mapIndex.set_basename(name);
}


Noggit::VertexSelectionCache World::getVertexSelectionCache()
{
  ZoneScoped;
  return std::move(Noggit::VertexSelectionCache{_vertex_tiles, _vertex_chunks, _vertex_border_chunks,
                                                _vertices_selected, _vertex_center});
}

void World::setVertexSelectionCache(Noggit::VertexSelectionCache& cache)
{
  ZoneScoped;
  _vertex_tiles = cache.vertex_tiles;
  _vertex_chunks = cache.vertex_chunks;
  _vertex_border_chunks = cache.vertex_border_chunks;
  _vertices_selected = cache.vertices_selected;
  _vertex_center = cache.vertex_center;

  _vertex_center_updated = false;
  _vertex_border_updated = false;
}

void World::exportAllADTsAlphamap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        for (int i = 1; i < 4; ++i)
        {
          QImage img = mTile->getAlphamapImage(i);
          img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                   + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                   + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
        }

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsAlphamap(const std::string& filename)
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        bool found = false;

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int k = 1; k < chunk->texture_set->num(); ++k)
            {
              if (chunk->texture_set->filename(k) == filename)
              {
                found = true;
                break;
              }
            }
          }
        }

        if (!found)
          continue;

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QString tex(filename.c_str());
        QImage img = mTile->getAlphamapImage(filename);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_" + tex.replace("/", "-") + ".png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsHeightmap()
{
  ZoneScoped;
  float min_height = std::numeric_limits<float>::max();
  float max_height = std::numeric_limits<float>::lowest();

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        float max = mTile->getMaxHeight();
        float min = mTile->getMinHeight();

        if (max_height < max)
          max_height = max;

        if (min_height > min)
          min_height = min;

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getHeightmapImage(min_height, max_height);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_height.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsVertexColorMap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getVertexColorsImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_vcol.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::importAllADTsAlphamaps()
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        for (int i = 1; i < 4; ++i)
        {
          QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                  + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                  + "_layer" + std::to_string(i).c_str() + ".png";

          if(!QFileInfo::exists(filename))
            continue;

          QImage img;
          img.load(filename, "PNG");

          if (img.width() != 1024 || img.height() != 1024)
          {
            QImage scaled = img.scaled(1024, 1024, Qt::IgnoreAspectRatio);
            mTile->setAlphaImage(scaled, i);
          }
          else
          {
            mTile->setAlphaImage(img, i);
          }

        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::importAllADTsHeightmaps(float multiplier, unsigned int mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_height.png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setHeightmapImage(scaled, multiplier, mode, tiledEdges);
        }
        else
        {
          mTile->setHeightmapImage(img, multiplier, mode, tiledEdges);
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::importAllADTVertexColorMaps(unsigned int mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_vcol.png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setVertexColorImage(scaled, mode, tiledEdges);
        }
        else
        {
          mTile->setVertexColorImage(img, mode, tiledEdges);
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::ensureAllTilesetsAllADTs()
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int i = 0; i < 4; ++i)
            {
              if (chunk->texture_set->num() <= i)
              {
                scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
                chunk->texture_set->addTexture(tex);
              }
            }

          }
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::notifyTileRendererOnSelectedTextureChange()
{
  ZoneScoped;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    tile->renderer()->notifyTileRendererOnSelectedTextureChange();
  }
}

void World::select_objects_in_area(
    const std::array<glm::vec2, 2> selection_box, 
    bool reset_selection,
    glm::mat4x4 view,
    glm::mat4x4 projection,
    int viewport_width, 
    int viewport_height,
    float user_depth,
    glm::vec3 camera_position)
{
    ZoneScoped;
    
    if (reset_selection)
    {
        this->reset_selection();
    }

    for (auto& map_object : _loaded_tiles_buffer)
    {
        MapTile* tile = map_object.second;

        if (!tile)
        {
            break;
        }

        for (auto& pair : tile->getObjectInstances())
        {
            auto objectType = pair.second[0]->which();
            if (objectType == eMODEL || objectType == eWMO)
            {
                for (auto& instance : pair.second)
                {
                    auto model = instance->transformMatrix();
                    glm::mat4 VPmatrix = projection * view;
                    glm::vec4 screenPos = VPmatrix * glm::vec4(instance->pos, 1.0f);
                    screenPos.x /= screenPos.w;
                    screenPos.y /= screenPos.w;

                    screenPos.x = (screenPos.x + 1.0f) / 2.0f;
                    screenPos.y = (screenPos.y + 1.0f) / 2.0f;
                    screenPos.y = 1 - screenPos.y;

                    screenPos.x *= viewport_width;
                    screenPos.y *= viewport_height;
                    
                    auto depth = glm::distance(camera_position, instance->pos);
                    if (depth <= user_depth)
                    {
                        const glm::vec2 screenPos2D = glm::vec2(screenPos);
                        if (misc::pointInside(screenPos2D, selection_box))
                        {
                            auto uid = instance->uid;
                            auto modelInstance = _model_instance_storage.get_instance(uid);
                            if (modelInstance && modelInstance.value().index() == eEntry_Object) {
                                auto obj = std::get<selected_object_type>(modelInstance.value());
                                auto which = std::get<selected_object_type>(modelInstance.value())->which();
                                if (which == eWMO)
                                {
                                    auto model_instance = static_cast<WMOInstance*>(obj);

                                    if (!is_selected(obj) && !model_instance->wmo->is_hidden())
                                    {
                                        this->add_to_selection(obj);
                                    }
                                }
                                else if (which == eMODEL)
                                {
                                    auto model_instance = static_cast<ModelInstance*>(obj);

                                    if (!is_selected(obj) && !model_instance->model->is_hidden())
                                    {
                                        this->add_to_selection(obj);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void World::add_object_group_from_selection()
{
    // create group from selected objects
    selection_group selection_group(get_selected_objects(), this);
    selection_group._is_selected = true;

    _selection_groups.push_back(selection_group);

    // write group to project
    saveSelectionGroups();
}

void World::remove_selection_group(selection_group* group)
{
    // std::vector<selection_type>::iterator position = std::find(_selection_groups.begin(), _selection_groups.end(), group);
    // if (position != _selection_groups.end())
    // {
    //     _selection_groups.erase(position);
    // }

    // for (auto it = _selection_groups.begin(); it != _selection_groups.end(); ++it)
    // {
    //     auto it_group = *it;
    //     if (it_group.getMembers().size() == group->getMembers().size() && it_group.getExtents() == group->getExtents())
    //     // if (it_group.isSelected())
    //     {
    //         _selection_groups.erase(it);
    //         saveSelectionGroups();
    //         return;
    //     }
    // }
}

void World::clear_selection_groups()
{
    // _selection_groups.clear();

    // for (auto it = _selection_groups.begin(); it != _selection_groups.end(); ++it)
    for (auto& group : _selection_groups)
    {
        // auto it_group = *it;
        // it->remove_group();
        group.remove_group(false);
    }
    _selection_groups.clear(); // in case it didn't properly clear
    saveSelectionGroups(); // only save once
}
