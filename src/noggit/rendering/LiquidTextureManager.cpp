// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "LiquidTextureManager.hpp"
#include "opengl/context.inl"
#include "noggit/DBC.h"
#include "noggit/application/NoggitApplication.hpp"

#include <array>

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
      case 0:
        return "XTextures\\lava\\lava.";
      case 2:
        return "XTextures\\slime\\slime.";
      default:
        return "XTextures\\river\\lake_a.";
    }
  }

  std::array<unsigned char, 4> fallback_liquid_color(unsigned liquid_type_id, int type)
  {
    switch (liquid_type_id)
    {
      case 2:
        return {35, 88, 130, 190};
      case 3:
        return {255, 92, 24, 220};
      case 4:
      case 21:
        return {35, 155, 65, 210};
      default:
        break;
    }

    if (type == 0)
    {
      return {255, 92, 24, 220};
    }
    if (type == 2)
    {
      return {35, 155, 65, 210};
    }

    return {45, 120, 170, 170};
  }

  void upload_fallback_liquid_profile(tsl::robin_map<unsigned, std::tuple<GLuint, glm::vec2, int, unsigned>>& texture_frames_map, unsigned liquid_type_id, int type)
  {
    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);
    auto color = fallback_liquid_color(liquid_type_id, type);
    gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture_frames_map[liquid_type_id] = std::make_tuple(array, glm::vec2(0.f, 0.f), type, 1);
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
    int type = field_count > LiquidTypeDB::Type ? record.getInt(LiquidTypeDB::Type) : 3;
    glm::vec2 anim = {1.f, 0.f};
    int shader_type = field_count > LiquidTypeDB::ShaderType ? record.getInt(LiquidTypeDB::ShaderType) : 3;

    if (field_count > LiquidTypeDB::AnimationY)
    {
      anim = {record.getFloat(LiquidTypeDB::AnimationX), record.getFloat(LiquidTypeDB::AnimationY)};
    }

    std::string filename;

    if (field_count <= LiquidTypeDB::TextureFilenames)
    {
      filename = classic_liquid_texture(liquid_type_id, type);
    }
    else if (shader_type == 3)
    {
      filename = "XTextures\\river\\lake_a.";
      anim = glm::vec2(1.f, 0.f);
    }
    else
    [[likely]]
    {
      try
      {
        std::string db_string_template = record.getString(LiquidTypeDB::TextureFilenames);
        filename = db_string_template.substr(0, db_string_template.length() - 6);
      }
      catch (...)
      {
        filename = "XTextures\\river\\lake_a.";
      }
    }

    GLuint array = 0;
    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    if (!client_data->exists(filename + "1.blp"))
    {
      filename = "XTextures\\river\\lake_a.";
    }

    if (!client_data->exists(filename + "1.blp"))
    {
      gl.deleteTextures(1, &array);
      upload_fallback_liquid_profile(_texture_frames_map, liquid_type_id, type);
      LogError << "Using fallback liquid texture for type " << liquid_type_id << std::endl;
      continue;
    }

    blp_texture tex(filename + "1.blp", _context);
    tex.finishLoading();

    int width_ = tex.width();
    int height_ = tex.height();
    const unsigned mip_level = tex.mip_level();
    const bool is_uncompressed = !tex.compression_format();

    constexpr unsigned N_FRAMES = 30;

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

    unsigned n_frames = 30;
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

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 3);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    _texture_frames_map[liquid_type_id] = std::make_tuple(array, anim, type, n_frames);
  }

  if (_texture_frames_map.empty())
  {
    upload_fallback_liquid_profile(_texture_frames_map, 1, 3);
    upload_fallback_liquid_profile(_texture_frames_map, 2, 3);
    upload_fallback_liquid_profile(_texture_frames_map, 3, 0);
    upload_fallback_liquid_profile(_texture_frames_map, 4, 2);
    upload_fallback_liquid_profile(_texture_frames_map, 21, 2);
    LogError << "Turtle water: installed built-in Classic liquid profiles" << std::endl;
  }

  _uploaded = true;
}

void LiquidTextureManager::unload()
{
  for (auto& pair : _texture_frames_map)
  {
    GLuint array = std::get<0>(pair.second);
    gl.deleteTextures(1, &array);
  }

  _texture_frames_map.clear();
  _uploaded = false;
}
