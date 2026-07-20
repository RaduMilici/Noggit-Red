// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LiquidTextureManager.hpp"
#include "opengl/context.inl"
#include "noggit/DBC.h"
#include "noggit/application/NoggitApplication.hpp"

#include <QtCore/QSettings>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

using namespace Noggit::Rendering;

namespace
{
  std::string classic_liquid_texture(unsigned liquid_type_id, int type)
  {
    switch (liquid_type_id)
    {
      case 2:
        return "XTextures\\ocean\\ocean_h.";
      case 3:
        return "XTextures\\lava\\lava.";
      case 4:
      case 21:
        return "XTextures\\slime\\slime.";
      default:
        break;
    }

    switch (type)
    {
      case 2:
        return "XTextures\\lava\\lava.";
      case 3:
        return "XTextures\\slime\\slime.";
      default:
        return "XTextures\\river\\lake_a.";
    }
  }

  std::string liquid_texture_base_from_dbc(std::string texture_filename)
  {
    // Real (3.3.5a/WotLK) LiquidType.dbc stores a %d frame placeholder, e.g.
    // "XTextures\LavaGreen\lavagreen.%d.blp". Our frame loader appends "1.blp","2.blp",...
    // to a base, so the base is everything before %d. Without this the literal %d survived,
    // the existence check failed, no profile was created, and every lava layer was skipped.
    auto const placeholder = texture_filename.find("%d");
    if (placeholder != std::string::npos)
    {
      return texture_filename.substr(0, placeholder);
    }

    if (texture_filename.size() >= 6
        && texture_filename.substr(texture_filename.size() - 6) == ".1.blp")
    {
      texture_filename.resize(texture_filename.size() - 5);
      return texture_filename;
    }

    if (texture_filename.size() >= 5
        && texture_filename.substr(texture_filename.size() - 5) == "1.blp")
    {
      texture_filename.resize(texture_filename.size() - 5);
      return texture_filename;
    }

    if (texture_filename.size() >= 4
        && texture_filename.substr(texture_filename.size() - 4) == ".blp")
    {
      texture_filename.resize(texture_filename.size() - 4);
      return texture_filename;
    }

    return texture_filename;
  }

  bool liquid_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* capture_debug = std::getenv("NOGGIT_CAPTURE_DEBUG");
      if (capture_debug && *capture_debug && std::strcmp(capture_debug, "0") != 0)
      {
        return true;
      }

      char const* liquid_debug = std::getenv("NOGGIT_LIQUID_DEBUG");
      return liquid_debug && *liquid_debug && std::strcmp(liquid_debug, "0") != 0;
    }();

    return enabled;
  }

  void apply_real_classic_liquid_aliases(tsl::robin_map<unsigned, std::tuple<GLuint, glm::vec2, int, unsigned>>& texture_frames_map)
  {
    auto set_shader_type = [&texture_frames_map](unsigned liquid_type_id, int type)
    {
      auto profile = texture_frames_map.find(liquid_type_id);
      if (profile != texture_frames_map.end())
      {
        texture_frames_map[liquid_type_id] = std::make_tuple(std::get<0>(profile->second),
                                                             std::get<1>(profile->second),
                                                             type,
                                                             std::get<3>(profile->second));
      }
    };

    set_shader_type(3, 2);
    set_shader_type(4, 3);
    set_shader_type(21, 3);
  }
}

LiquidTextureManager::LiquidTextureManager(Noggit::NoggitRenderContext context)
  : _context(context)
{
}

void LiquidTextureManager::upload()
{
  if (_uploaded)
    return;

  auto client_data = Noggit::Application::NoggitApplication::instance()->clientData();

  for (int i = 0; i < gLiquidTypeDB.getRecordCount(); ++i)
  {
    const DBCFile::Record record = gLiquidTypeDB.getRecord(i);
    size_t const field_count = gLiquidTypeDB.getFieldCount();
    unsigned liquid_type_id = record.getInt(LiquidTypeDB::ID);
    int type = field_count > LiquidTypeDB::Type ? record.getInt(LiquidTypeDB::Type) : 0;
    glm::vec2 anim = {1.f, 0.f};

    if (field_count > LiquidTypeDB::AnimationY)
    {
      anim = {record.getFloat(LiquidTypeDB::AnimationX), record.getFloat(LiquidTypeDB::AnimationY)};
    }

    std::string filename;
    char const* texture_source = "dbc";

    if (field_count <= LiquidTypeDB::TextureFilenames)
    {
      filename = classic_liquid_texture(liquid_type_id, type);
      texture_source = "classic-layout";
    }
    else
    {
      std::string db_string_template = record.getString(LiquidTypeDB::TextureFilenames);
      if (db_string_template.empty())
      {
        filename = classic_liquid_texture(liquid_type_id, type);
        texture_source = "classic-empty-texture";
      }
      else
      {
        filename = liquid_texture_base_from_dbc(db_string_template);
      }
    }

    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    if (!client_data->exists(filename + "1.blp"))
    {
      gl.deleteTextures(1, &array);
      LogError << "Skipping liquid type " << liquid_type_id
               << ": missing client texture '" << filename << "1.blp'"
               << " from " << texture_source
               << std::endl;
      continue;
    }

    blp_texture tex(filename + "1.blp", _context);
    tex.finishLoading();

    int width_ = tex.width();
    int height_ = tex.height();
    const unsigned mip_level = tex.mip_level();
    const bool is_uncompressed = !tex.compression_format();

    constexpr unsigned N_FRAMES = 60; // reference noggit3 allows up to 60 liquid frames

    if (is_uncompressed)
    {
      for (unsigned int j = 0; j < mip_level; ++j)
      {
        gl.texImage3D(GL_TEXTURE_2D_ARRAY, j, GL_RGBA8, width_, height_, N_FRAMES, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      nullptr);

        width_ = std::max(width_ >> 1, 1);
        height_ = std::max(height_ >> 1, 1);
      }
    }
    else
    [[likely]]
    {
      for (unsigned int j = 0; j < mip_level; ++j)
      {
        gl.compressedTexImage3D(GL_TEXTURE_2D_ARRAY, j, tex.compression_format().value(), width_, height_, N_FRAMES,
                                0, static_cast<GLsizei>(tex.compressed_data()[j].size() * N_FRAMES), nullptr);

        width_ = std::max(width_ >> 1, 1);
        height_ = std::max(height_ >> 1, 1);
      }
    }

    unsigned n_frames = N_FRAMES;
    for (int j = 0; j < N_FRAMES; ++j)
    {
      if (!client_data->exists(filename + std::to_string((j + 1)) + ".blp"))
      {
        n_frames = j;
        break;
      }

      blp_texture tex_frame(filename + std::to_string(j + 1) + ".blp", _context);
      tex_frame.finishLoading();

      if (tex_frame.height() != tex.height() || tex_frame.width() != tex.width())
        LogError << "Liquid texture resolution mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else if (tex_frame.compression_format() != tex.compression_format())
        LogError << "Liquid texture compression mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else if (tex_frame.mip_level() != tex.mip_level())
        LogError << "Liquid texture mip level mismatch. Make sure all textures within a liquid type use identical format." << std::endl;
      else
      [[likely]]
      {
        tex_frame.uploadToArray(j);
        continue;
      }

      tex.uploadToArray(j);
    }

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 1);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Anisotropic filtering (checklist 20.5): water/lava are seen at grazing angles more than any
    // other surface, so they benefit the most. Same QSettings level as TextureManager.
    {
      GLfloat hw_max = 1.0f;
      gl.getFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &hw_max);
      float const requested = QSettings().value("render/anisotropic_filtering", 16.0f).toFloat();
      float const level = std::clamp(requested, 1.0f, hw_max >= 1.0f ? hw_max : 1.0f);
      if (level > 1.0f)
      {
        gl.texParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, level);
      }
    }

    _texture_frames_map[liquid_type_id] = std::make_tuple(array, anim, type, n_frames);
    if (liquid_debug_enabled())
    {
      LogDebug << "Loaded liquid profile id=" << liquid_type_id
               << " type=" << type
               << " texture='" << filename << "'"
               << " source=" << texture_source
               << " frames=" << n_frames
               << " anim=(" << anim.x << ", " << anim.y << ")"
               << std::endl;
    }
  }

  // The classic id->shader-type aliases (ids 3/4/21 -> magma/slime) are a vanilla 1.12
  // crutch. Only apply them for the classic DBC layout so they can never clobber a real
  // (3.3.5a) LiquidType.dbc, which carries a correct Type field and the TextureFilenames column.
  if (gLiquidTypeDB.getFieldCount() <= LiquidTypeDB::TextureFilenames)
  {
    apply_real_classic_liquid_aliases(_texture_frames_map);
  }

  if (_texture_frames_map.empty())
  {
    LogError << "No liquid profiles loaded; liquid rendering skipped until real client textures are available." << std::endl;
  }

  _uploaded = true;
}

void LiquidTextureManager::unload()
{
  std::unordered_set<GLuint> deleted_arrays;
  for (auto& pair : _texture_frames_map)
  {
    GLuint array = std::get<0>(pair.second);
    if (array && deleted_arrays.insert(array).second)
    {
      gl.deleteTextures(1, &array);
    }
  }

  _texture_frames_map.clear();
  _uploaded = false;
}

void LiquidTextureManager::reapply_anisotropy()
{
  // Mirror of TextureManager::reapply_anisotropy for the liquid arrays (water/lava/slime), which
  // carry their own arrays in _texture_frames_map. Level is set explicitly (incl. 1 = off) so a
  // lowered setting clears a previously-set higher value. Needs a current GL context.
  GLfloat hw_max = 1.0f;
  gl.getFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &hw_max);
  float const requested = QSettings().value("render/anisotropic_filtering", 16.0f).toFloat();
  float const level = std::max(1.0f, std::clamp(requested, 1.0f, hw_max >= 1.0f ? hw_max : 1.0f));
  for (auto& pair : _texture_frames_map)
  {
    GLuint const array = std::get<0>(pair.second);
    if (!array)
      continue;
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);
    gl.texParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, level);
  }
}
