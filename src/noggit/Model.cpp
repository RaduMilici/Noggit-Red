// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/bounding_box.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/Log.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/World.h>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <external/tracy/Tracy.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <util/CurrentFunction.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <math/trig.hpp>

namespace
{
  bool classic_m2_debug_enabled();

  bool is_null_asset_reference(std::string filename)
  {
    auto const is_trimmed_char = [](unsigned char character)
    {
      return character == '\0' || std::isspace(character);
    };

    filename.erase(filename.begin(),
                   std::find_if(filename.begin(), filename.end(),
                                [&](unsigned char character) { return !is_trimmed_char(character); }));
    filename.erase(std::find_if(filename.rbegin(), filename.rend(),
                                [&](unsigned char character) { return !is_trimmed_char(character); }).base(),
                   filename.end());

    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char character)
    {
      return static_cast<char>(std::tolower(character));
    });

    std::replace(filename.begin(), filename.end(), '\\', '/');

    if (filename.empty() || filename == "0" || filename == "none" || filename == "null")
    {
      return true;
    }

    if (filename.find('/') == std::string::npos)
    {
      auto const extension_pos = filename.rfind('.');
      if (extension_pos != std::string::npos && filename.substr(0, extension_pos) == "0")
      {
        auto const extension = filename.substr(extension_pos);
        return extension == ".m2"
            || extension == ".mdx"
            || extension == ".mdl"
            || extension == ".blp";
      }
    }

    return false;
  }

  bool should_log_classic_skin_model(BlizzardArchive::Listfile::FileKey const& file_key)
  {
    if (!classic_m2_debug_enabled() || !file_key.hasFilepath())
    {
      return false;
    }

    auto const& path = file_key.filepath();
    return path.find("elementalearth") != std::string::npos
        || path.find("firelord") != std::string::npos
        || path.find("darkironnode") != std::string::npos;
  }

  bool classic_m2_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CLASSIC_M2_DEBUG");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  bool capture_m2_animation_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG");
      return value && *value && std::strcmp(value, "0") != 0;
    }();

    return enabled;
  }

  // Cross-fade between poses on animation/sub-animation transitions (matches the 1.12 client's ~150ms
  // smoothstep blend). ON by default; set NOGGIT_NO_ANIM_BLEND=1 to fall back to hard sequence switches.
  bool anim_transition_blend_enabled()
  {
    static bool const enabled = []()
    {
      char const* value = std::getenv("NOGGIT_NO_ANIM_BLEND");
      return !(value && *value && std::strcmp(value, "0") != 0);
    }();

    return enabled;
  }

  bool is_classic_effect_shell_model_path(std::string const& path)
  {
    return path.starts_with("world/generic/passivedoodads/particleemitters/")
        || path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam.m2"
        || path == "world/khazmodan/ironforge/passivedoodads/lavasteam/lavasteam_low.m2";
  }

  bool is_classic_volcanic_vent_model_path(std::string const& path)
  {
    return path == "world/azeroth/burningsteppes/passivedoodads/volcanicvents/volcanicventsmall01.m2"
        || path == "world/azeroth/burningsteppes/passivedoodads/volcanicvents/volcanicventmed01.m2"
        || path == "world/azeroth/burningsteppes/passivedoodads/volcanicvents/volcanicventlarge01.m2";
  }

  bool classic_effect_debug_enabled()
  {
    static bool const enabled = []()
    {
      char const* effect_debug = std::getenv("NOGGIT_CLASSIC_EFFECT_DEBUG");
      if (effect_debug && *effect_debug && std::strcmp(effect_debug, "0") != 0)
      {
        return true;
      }

      return classic_m2_debug_enabled();
    }();

    return enabled;
  }

  bool classic_effect_particle_parse_enabled_for_model(Model const* model)
  {
    if (!model || !model->usesClassicLayout() || !model->file_key().hasFilepath())
    {
      return false;
    }

    auto const& path = model->file_key().filepath();
    // Character (player) models are excluded -- their attachments/effects are driven separately and
    // were never part of the classic-effect particle work. CREATURE models ARE included now: arcane
    // elementals / ghosts / mana fiends etc. carry on-body sparkle/energy emitters (e.g. Anomalus' 8
    // emitters) that the client shows, and creature SPAWNS draw them via ModelRender::drawParticlesForInstance.
    // (Previously creatures were excluded too, which is why no spawn ever emitted particles.)
    if (path.starts_with("character/"))
    {
      return false;
    }

    return true;
  }

  int16_t clamp_classic_particle_int16(int16_t value, int low, int high)
  {
    return static_cast<int16_t>(std::clamp(static_cast<int>(value), low, high));
  }

  void sanitize_classic_particle_animation_block(ClassicAnimationBlock& block,
                                                std::uint32_t global_sequence_count)
  {
    if (block.seq < -1 || static_cast<std::uint32_t>(block.seq) >= global_sequence_count)
    {
      block.seq = -1;
    }

    if (block.type < Animation::Interpolation::Type::NONE
        || block.type > Animation::Interpolation::Type::HERMITE)
    {
      block.type = Animation::Interpolation::Type::NONE;
    }
  }

  struct ClassicModelHeader
  {
    char id[4];
    uint8_t version[4];
    uint32_t nameLength;
    uint32_t nameOfs;
    uint32_t Flags;

    uint32_t nGlobalSequences;
    uint32_t ofsGlobalSequences;
    uint32_t nAnimations;
    uint32_t ofsAnimations;
    uint32_t nAnimationLookup;
    uint32_t ofsAnimationLookup;
    uint32_t nD;
    uint32_t ofsD;
    uint32_t nBones;
    uint32_t ofsBones;
    uint32_t nKeyBoneLookup;
    uint32_t ofsKeyBoneLookup;

    uint32_t nVertices;
    uint32_t ofsVertices;
    uint32_t nViews;
    uint32_t ofsViews;

    uint32_t nColors;
    uint32_t ofsColors;
    uint32_t nTextures;
    uint32_t ofsTextures;
    uint32_t nTransparency;
    uint32_t ofsTransparency;
    uint32_t nI;
    uint32_t ofsI;
    uint32_t nTexAnims;
    uint32_t ofsTexAnims;
    uint32_t nTexReplace;
    uint32_t ofsTexReplace;

    uint32_t nRenderFlags;
    uint32_t ofsRenderFlags;
    uint32_t nBoneLookup;
    uint32_t ofsBoneLookup;
    uint32_t nTexLookup;
    uint32_t ofsTexLookup;
    uint32_t nTexUnitLookup;
    uint32_t ofsTexUnitLookup;
    uint32_t nTransparencyLookup;
    uint32_t ofsTransparencyLookup;
    uint32_t nTexAnimLookup;
    uint32_t ofsTexAnimLookup;

    glm::vec3 bounding_box_min;
    glm::vec3 bounding_box_max;
    float bounding_box_radius;
    glm::vec3 collision_box_min;
    glm::vec3 collision_box_max;
    float collision_box_radius;

    uint32_t nBoundingTriangles;
    uint32_t ofsBoundingTriangles;
    uint32_t nBoundingVertices;
    uint32_t ofsBoundingVertices;
    uint32_t nBoundingNormals;
    uint32_t ofsBoundingNormals;

    uint32_t nAttachments;
    uint32_t ofsAttachments;
    uint32_t nAttachLookup;
    uint32_t ofsAttachLookup;
    uint32_t nEvents;
    uint32_t ofsEvents;
    uint32_t nLights;
    uint32_t ofsLights;
    uint32_t nCameras;
    uint32_t ofsCameras;
    uint32_t nCameraLookup;
    uint32_t ofsCameraLookup;
    uint32_t nRibbonEmitters;
    uint32_t ofsRibbonEmitters;
    uint32_t nParticleEmitters;
    uint32_t ofsParticleEmitters;
  };

  struct ClassicModelView
  {
    uint32_t n_index;
    uint32_t ofs_index;
    uint32_t n_triangle;
    uint32_t ofs_triangle;
    uint32_t n_vertex_property;
    uint32_t ofs_vertex_property;
    uint32_t n_submesh;
    uint32_t ofs_submesh;
    uint32_t n_texture_unit;
    uint32_t ofs_texture_unit;
    int32_t lod;
  };

  struct ClassicModelGeoset
  {
    uint16_t id;
    uint16_t d2;
    uint16_t vstart;
    uint16_t vcount;
    uint16_t istart;
    uint16_t icount;
    uint16_t d3;
    uint16_t d4;
    uint16_t d5;
    uint16_t d6;
    glm::vec3 center;
  };

  struct ClassicModelAnimation
  {
    uint16_t animID;
    uint16_t subAnimID;
    uint32_t startTimestamp;
    uint32_t endTimestamp;
    float moveSpeed;
    uint32_t flags;
    uint16_t frequency;
    uint16_t unused;
    uint32_t minimumRepetitions;
    uint32_t maximumRepetitions;
    uint32_t blendTime;
    glm::vec3 boxA;
    glm::vec3 boxB;
    float rad;
    int16_t nextAnimation;
    uint16_t aliasNext;
  };

  struct ClassicModelAttachmentDef
  {
    uint32_t id;
    uint16_t bone;
    uint16_t unknown1;
    glm::vec3 pos;
    ClassicAnimationBlock Enabled;
  };

  uint32_t m2_version(uint8_t const version[4])
  {
    uint32_t value = 0;
    std::memcpy(&value, version, sizeof(value));
    return value;
  }

  bool range_fits(BlizzardArchive::ClientFile const& file, uint32_t offset, uint32_t count, size_t element_size)
  {
    return !count || (offset < file.getSize() && count <= (file.getSize() - offset) / element_size);
  }

  std::string read_embedded_model_string(BlizzardArchive::ClientFile const& file,
                                         std::uint32_t offset,
                                         std::uint32_t length)
  {
    if (!length || offset >= file.getSize() || length > file.getSize() - offset)
    {
      return {};
    }

    char const* string_ptr = file.getBuffer() + offset;
    std::size_t string_length = length;
    if (string_length > 0 && string_ptr[string_length - 1] == '\0')
    {
      --string_length;
    }

    return std::string(string_ptr, string_length);
  }

  std::string normalize_embedded_particle_texture_filename(std::string filename,
                                                           std::string const& model_path)
  {
    if (is_null_asset_reference(filename))
    {
      return {};
    }

    std::replace(filename.begin(), filename.end(), '\\', '/');

    if (filename.rfind('.') == std::string::npos)
    {
      filename += ".blp";
    }

    if (filename.find('/') == std::string::npos)
    {
      auto const separator = model_path.rfind('/');
      if (separator != std::string::npos)
      {
        filename = model_path.substr(0, separator + 1) + filename;
      }
    }

    return filename;
  }

  bool m2_header_ranges_fit(BlizzardArchive::ClientFile const& file, ModelHeader const& header)
  {
    return range_fits(file, header.ofsGlobalSequences, header.nGlobalSequences, sizeof(int))
        && range_fits(file, header.ofsAnimations, header.nAnimations, sizeof(ModelAnimation))
        && range_fits(file, header.ofsAnimationLookup, header.nAnimationLookup, sizeof(int16_t))
        && range_fits(file, header.ofsBones, header.nBones, sizeof(ModelBoneDef))
        && range_fits(file, header.ofsKeyBoneLookup, header.nKeyBoneLookup, sizeof(int16_t))
        && range_fits(file, header.ofsVertices, header.nVertices, sizeof(ModelVertex))
        && range_fits(file, header.ofsColors, header.nColors, sizeof(ModelColorDef))
        && range_fits(file, header.ofsTextures, header.nTextures, sizeof(ModelTextureDef))
        && range_fits(file, header.ofsTransparency, header.nTransparency, sizeof(ModelTransDef))
        && range_fits(file, header.ofsTexAnims, header.nTexAnims, sizeof(ModelTexAnimDef))
        && range_fits(file, header.ofsTexReplace, header.nTexReplace, sizeof(uint16_t))
        && range_fits(file, header.ofsRenderFlags, header.nRenderFlags, sizeof(ModelRenderFlags))
        && range_fits(file, header.ofsBoneLookup, header.nBoneLookup, sizeof(uint16_t))
        && range_fits(file, header.ofsTexLookup, header.nTexLookup, sizeof(uint16_t))
        && range_fits(file, header.ofsTexUnitLookup, header.nTexUnitLookup, sizeof(int16_t))
        && range_fits(file, header.ofsTransparencyLookup, header.nTransparencyLookup, sizeof(int16_t))
        && range_fits(file, header.ofsTexAnimLookup, header.nTexAnimLookup, sizeof(int16_t))
        && range_fits(file, header.ofsBoundingTriangles, header.nBoundingTriangles, sizeof(uint16_t))
        && range_fits(file, header.ofsBoundingVertices, header.nBoundingVertices, sizeof(glm::vec3))
        && range_fits(file, header.ofsBoundingNormals, header.nBoundingNormals, sizeof(glm::vec3))
        && range_fits(file, header.ofsAttachments, header.nAttachments, sizeof(ModelAttachmentDef))
        && range_fits(file, header.ofsAttachLookup, header.nAttachLookup, sizeof(int16_t))
        && range_fits(file, header.ofsEvents, header.nEvents, sizeof(ModelEvents))
        && range_fits(file, header.ofsLights, header.nLights, sizeof(ModelLightDef))
        && range_fits(file, header.ofsCameras, header.nCameras, sizeof(ModelCameraDef))
        && range_fits(file, header.ofsCameraLookup, header.nCameraLookup, sizeof(int16_t))
        && range_fits(file, header.ofsRibbonEmitters, header.nRibbonEmitters, sizeof(ModelRibbonEmitterDef))
        && range_fits(file, header.ofsParticleEmitters, header.nParticleEmitters, sizeof(ModelParticleEmitterDef));
  }

  bool m2_static_ranges_fit(BlizzardArchive::ClientFile const& file, ModelHeader const& header)
  {
    return range_fits(file, header.ofsVertices, header.nVertices, sizeof(ModelVertex))
          && range_fits(file, header.ofsTextures, header.nTextures, sizeof(ModelTextureDef))
          && range_fits(file, header.ofsTexReplace, header.nTexReplace, sizeof(uint16_t))
          && range_fits(file, header.ofsRenderFlags, header.nRenderFlags, sizeof(ModelRenderFlags))
          && range_fits(file, header.ofsBoneLookup, header.nBoneLookup, sizeof(uint16_t))
          && range_fits(file, header.ofsTexLookup, header.nTexLookup, sizeof(uint16_t))
          && range_fits(file, header.ofsTexUnitLookup, header.nTexUnitLookup, sizeof(int16_t))
          && range_fits(file, header.ofsTransparencyLookup, header.nTransparencyLookup, sizeof(int16_t))
          && range_fits(file, header.ofsTexAnimLookup, header.nTexAnimLookup, sizeof(int16_t))
          && range_fits(file, header.ofsBoundingTriangles, header.nBoundingTriangles, sizeof(uint16_t))
          && range_fits(file, header.ofsBoundingVertices, header.nBoundingVertices, sizeof(glm::vec3))
          && range_fits(file, header.ofsBoundingNormals, header.nBoundingNormals, sizeof(glm::vec3));
  }

  void copy_classic_header(ClassicModelHeader const& classic_header, ModelHeader& header);

  bool m2_classic_ranges_fit(BlizzardArchive::ClientFile const& file, ClassicModelHeader const& header)
  {
    ModelHeader translated_header;
    copy_classic_header(header, translated_header);

    return m2_static_ranges_fit(file, translated_header)
        && range_fits(file, header.ofsGlobalSequences, header.nGlobalSequences, sizeof(int))
        && range_fits(file, header.ofsAnimations, header.nAnimations, sizeof(ClassicModelAnimation))
        && range_fits(file, header.ofsAnimationLookup, header.nAnimationLookup, sizeof(int16_t))
        && range_fits(file, header.ofsBones, header.nBones, sizeof(ClassicModelBoneDef))
        && range_fits(file, header.ofsKeyBoneLookup, header.nKeyBoneLookup, sizeof(int16_t))
        && range_fits(file, header.ofsTransparency, header.nTransparency, sizeof(ClassicModelTransDef))
        && range_fits(file, header.ofsTexAnims, header.nTexAnims, sizeof(ClassicModelTexAnimDef))
        && range_fits(file, header.ofsAttachments, header.nAttachments, sizeof(ClassicModelAttachmentDef))
        && range_fits(file, header.ofsAttachLookup, header.nAttachLookup, sizeof(int16_t));
  }

  void copy_classic_header(ClassicModelHeader const& classic_header, ModelHeader& header)
  {
    std::memcpy(header.id, classic_header.id, sizeof(header.id));
    std::memcpy(header.version, classic_header.version, sizeof(header.version));
    header.nameLength = classic_header.nameLength;
    header.nameOfs = classic_header.nameOfs;
    header.Flags = classic_header.Flags;
    header.nGlobalSequences = classic_header.nGlobalSequences;
    header.ofsGlobalSequences = classic_header.ofsGlobalSequences;
    header.nAnimations = classic_header.nAnimations;
    header.ofsAnimations = classic_header.ofsAnimations;
    header.nAnimationLookup = classic_header.nAnimationLookup;
    header.ofsAnimationLookup = classic_header.ofsAnimationLookup;
    header.nBones = classic_header.nBones;
    header.ofsBones = classic_header.ofsBones;
    header.nKeyBoneLookup = classic_header.nKeyBoneLookup;
    header.ofsKeyBoneLookup = classic_header.ofsKeyBoneLookup;
    header.nVertices = classic_header.nVertices;
    header.ofsVertices = classic_header.ofsVertices;
    header.nViews = classic_header.nViews;
    header.nColors = classic_header.nColors;
    header.ofsColors = classic_header.ofsColors;
    header.nTextures = classic_header.nTextures;
    header.ofsTextures = classic_header.ofsTextures;
    header.nTransparency = classic_header.nTransparency;
    header.ofsTransparency = classic_header.ofsTransparency;
    header.nTexAnims = classic_header.nTexAnims;
    header.ofsTexAnims = classic_header.ofsTexAnims;
    header.nTexReplace = classic_header.nTexReplace;
    header.ofsTexReplace = classic_header.ofsTexReplace;
    header.nRenderFlags = classic_header.nRenderFlags;
    header.ofsRenderFlags = classic_header.ofsRenderFlags;
    header.nBoneLookup = classic_header.nBoneLookup;
    header.ofsBoneLookup = classic_header.ofsBoneLookup;
    header.nTexLookup = classic_header.nTexLookup;
    header.ofsTexLookup = classic_header.ofsTexLookup;
    header.nTexUnitLookup = classic_header.nTexUnitLookup;
    header.ofsTexUnitLookup = classic_header.ofsTexUnitLookup;
    header.nTransparencyLookup = classic_header.nTransparencyLookup;
    header.ofsTransparencyLookup = classic_header.ofsTransparencyLookup;
    header.nTexAnimLookup = classic_header.nTexAnimLookup;
    header.ofsTexAnimLookup = classic_header.ofsTexAnimLookup;
    header.bounding_box_min = classic_header.bounding_box_min;
    header.bounding_box_max = classic_header.bounding_box_max;
    header.bounding_box_radius = classic_header.bounding_box_radius;
    header.collision_box_min = classic_header.collision_box_min;
    header.collision_box_max = classic_header.collision_box_max;
    header.collision_box_radius = classic_header.collision_box_radius;
    header.nBoundingTriangles = classic_header.nBoundingTriangles;
    header.ofsBoundingTriangles = classic_header.ofsBoundingTriangles;
    header.nBoundingVertices = classic_header.nBoundingVertices;
    header.ofsBoundingVertices = classic_header.ofsBoundingVertices;
    header.nBoundingNormals = classic_header.nBoundingNormals;
    header.ofsBoundingNormals = classic_header.ofsBoundingNormals;
    header.nAttachments = classic_header.nAttachments;
    header.ofsAttachments = classic_header.ofsAttachments;
    header.nAttachLookup = classic_header.nAttachLookup;
    header.ofsAttachLookup = classic_header.ofsAttachLookup;
    header.nEvents = classic_header.nEvents;
    header.ofsEvents = classic_header.ofsEvents;
    header.nLights = classic_header.nLights;
    header.ofsLights = classic_header.ofsLights;
    header.nCameras = classic_header.nCameras;
    header.ofsCameras = classic_header.ofsCameras;
    header.nCameraLookup = classic_header.nCameraLookup;
    header.ofsCameraLookup = classic_header.ofsCameraLookup;
    header.nRibbonEmitters = classic_header.nRibbonEmitters;
    header.ofsRibbonEmitters = classic_header.ofsRibbonEmitters;
    header.nParticleEmitters = classic_header.nParticleEmitters;
    header.ofsParticleEmitters = classic_header.ofsParticleEmitters;
  }

  bool classic_bone_needs_runtime_matrix(std::uint32_t flags)
  {
    return (flags & (0x8 | 0x10 | 0x20 | 0x40 | 0x200)) != 0;
  }

  glm::mat4x4 classic_bone_local_matrix(std::uint32_t /*flags*/, glm::vec3 const& /*pivot*/, glm::mat4x4 const& /*model_view*/)
  {
    // For classic M2 static display in the editor we always return identity.
    // Billboard flags (head tracking etc.) cause the head bone to orbit away
    // from the body when the camera is tilted; in a map editor the bind pose
    // is exactly what we want to show.
    return glm::mat4x4(1.f);
  }

  ModelGeoset translate_classic_geoset(ClassicModelGeoset const& classic_geoset)
  {
    ModelGeoset geoset = {};
    geoset.id = classic_geoset.id;
    geoset.d2 = classic_geoset.d2;
    geoset.vstart = classic_geoset.vstart;
    geoset.vcount = classic_geoset.vcount;
    geoset.istart = classic_geoset.istart;
    geoset.icount = classic_geoset.icount;
    geoset.d3 = classic_geoset.d3;
    geoset.d4 = classic_geoset.d4;
    geoset.d5 = classic_geoset.d5;
    geoset.d6 = classic_geoset.d6;
    geoset.BoundingBox[0] = classic_geoset.center;
    geoset.BoundingBox[1] = classic_geoset.center;
    geoset.radius = 0.f;
    return geoset;
  }

  ModelAttachmentDef translate_classic_attachment(ClassicModelAttachmentDef const& classic_attachment)
  {
    ModelAttachmentDef attachment = {};
    attachment.id = classic_attachment.id;
    attachment.bone = classic_attachment.bone;
    attachment.unknown1 = classic_attachment.unknown1;
    attachment.pos = classic_attachment.pos;
    attachment.Enabled.type = classic_attachment.Enabled.type;
    attachment.Enabled.seq = classic_attachment.Enabled.seq;
    attachment.Enabled.nTimes = classic_attachment.Enabled.nTimes;
    attachment.Enabled.ofsTimes = classic_attachment.Enabled.ofsTimes;
    attachment.Enabled.nKeys = classic_attachment.Enabled.nKeys;
    attachment.Enabled.ofsKeys = classic_attachment.Enabled.ofsKeys;
    return attachment;
  }
}

Model::Model(const std::string& filename, Noggit::NoggitRenderContext context)
  : AsyncObject(filename)
  , _context(context)
  , _renderer(this)
{
  memset(&header, 0, sizeof(ModelHeader));

  // Fishing-pool water-surface effect (see Model::_water_surface_effect). Keyed on the model path;
  // all are World\SkillActivated\TradeskillEnablers\Tradeskill_FishSchool_*.
  std::string lowered = filename;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  _water_surface_effect = lowered.find("fishschool") != std::string::npos;
}

void Model::finishLoading()
{
  BlizzardArchive::ClientFile f(_file_key.filepath(), Noggit::Application::NoggitApplication::instance()->clientData());

  if (f.isEof() || f.getSize() < sizeof(ModelHeader))
  {
    LogError << "Error loading file \"" << _file_key.stringRepr() << "\". Aborting to load model." << std::endl;
    finished = true;
    return;
  }

  memcpy(&header, f.getBuffer(), sizeof(ModelHeader));

  if (std::memcmp(header.id, "MD20", 4) != 0)
  {
    LogError << "Error loading file \"" << _file_key.stringRepr() << "\". Invalid M2 magic." << std::endl;
    finished = true;
    return;
  }

  if (!m2_header_ranges_fit(f, header) && f.getSize() >= sizeof(ClassicModelHeader))
  {
    ClassicModelHeader classic_header;
    std::memcpy(&classic_header, f.getBuffer(), sizeof(ClassicModelHeader));

    ModelHeader translated_header;
    copy_classic_header(classic_header, translated_header);

    // v257 is a rare vanilla-era variant with the same classic layout (the Turtle tree ships a
    // handful); gating at 256 made those fall through to the WotLK parse and misload.
    if (std::memcmp(translated_header.id, "MD20", 4) == 0
        && m2_version(translated_header.version) <= 257
        && range_fits(f, classic_header.ofsViews, classic_header.nViews, sizeof(ClassicModelView))
        && m2_classic_ranges_fit(f, classic_header))
    {
      header = translated_header;
      _embedded_view_offset = classic_header.ofsViews;
      _uses_classic_layout = true;
    }
  }

  if (_uses_classic_layout ? !m2_static_ranges_fit(f, header) : !m2_header_ranges_fit(f, header))
  {
    LogError << "Error loading file \"" << _file_key.stringRepr() << "\". Unsupported or corrupt M2 header." << std::endl;
    finished = true;
    return;
  }

  // blend mode override
  if (!_uses_classic_layout && (header.Flags & 8))
  {
    // go to the end of the header (where the blend override data is)    
    uint32_t const* blend_override_info = reinterpret_cast<uint32_t const*>(f.getBuffer() + sizeof(ModelHeader));
    uint32_t n_blend_override = *blend_override_info++;
    uint32_t ofs_blend_override = *blend_override_info;

    blend_override = M2Array<uint16_t>(f, ofs_blend_override, n_blend_override);
  }

  if (_uses_classic_layout)
  {
    bool const has_classic_runtime_bones = initClassicStaticBones(f);
    // Classic models can have texture animations (UV scroll: lava falls, flowing water,
    // etc.). animTextures was hardcoded false, so the texanim load block in initAnimated
    // never ran -> _texture_animations stayed empty -> tex_matrix was always identity ->
    // every classic UV animation rendered frozen. Drive it from the header instead, and
    // mark the model animated so initAnimated runs even without runtime bones.
    animTextures = header.nTexAnims > 0;
    animated = has_classic_runtime_bones || animTextures;
    animGeometry = has_classic_runtime_bones;
    animBones = has_classic_runtime_bones;
    _per_instance_animation = has_classic_runtime_bones || animTextures;
  }
  else
  {
    animated = isAnimated(f);  // isAnimated will set animGeometry and animTextures
  }

  if (!_logged_layout_summary && classic_m2_debug_enabled())
  {
    _logged_layout_summary = true;
    LogDebug << "M2 runtime layout model='" << _file_key.stringRepr()
             << "' layout=" << (_uses_classic_layout ? "classic" : "wotlk")
             << " version=" << m2_version(header.version)
             << " animated=" << animated
             << " animBones=" << animBones
             << " animGeometry=" << animGeometry
             << " animTextures=" << animTextures
             << " perInstance=" << _per_instance_animation
             << " bones=" << header.nBones
             << " nAnimations=" << header.nAnimations
             << " nTexAnims=" << header.nTexAnims
             << " vertices=" << header.nVertices
             << " views=" << header.nViews
             << (_uses_classic_layout ? " embeddedViewOffset=" : " globalSequences=")
             << (_uses_classic_layout ? _embedded_view_offset : header.nGlobalSequences)
             << std::endl;
  }

  trans = 1.0f;
  _current_anim_seq = 0;

  rad = header.bounding_box_radius;

  if (header.nGlobalSequences)
  {
    _global_sequences = M2Array<int>(f, header.ofsGlobalSequences, header.nGlobalSequences);
  }

  //! \todo  This takes a biiiiiit long. Have a look at this.
  initCommon(f);

  if (animated)
  {
    initAnimated(f);
  }

  f.close();

  finished = true;
  _state_changed.notify_all();
}

bool Model::initClassicStaticBones(const BlizzardArchive::ClientFile& f)
{
  _classic_static_bones.clear();
  bone_matrices.clear();

  if (!_uses_classic_layout || !header.nBones || !range_fits(f, header.ofsBones, header.nBones, sizeof(ClassicModelBoneDef)))
  {
    return false;
  }

  bool has_runtime_bone = false;
  bool has_weighted_vertices = false;
  auto const* model_bones = reinterpret_cast<ClassicModelBoneDef const*>(f.getBuffer() + header.ofsBones);
  _classic_static_bones.reserve(header.nBones);

  for (std::size_t i = 0; i < header.nBones; ++i)
  {
    ClassicStaticBone bone;
    bone.flags = model_bones[i].flags;
    bone.parent = model_bones[i].parent;
    bone.pivot = fixCoordSystem(model_bones[i].pivot);
    _classic_static_bones.push_back(bone);

    has_runtime_bone = has_runtime_bone || classic_bone_needs_runtime_matrix(bone.flags);
  }

  if (header.nVertices && range_fits(f, header.ofsVertices, header.nVertices, sizeof(ModelVertex)))
  {
    auto const* vertices = reinterpret_cast<ModelVertex const*>(f.getBuffer() + header.ofsVertices);
    for (std::size_t i = 0; i < header.nVertices && !has_weighted_vertices; ++i)
    {
      has_weighted_vertices = vertices[i].weights[0] || vertices[i].weights[1]
                           || vertices[i].weights[2] || vertices[i].weights[3];
    }
  }

  if (!has_runtime_bone && !has_weighted_vertices)
  {
    _classic_static_bones.clear();
    return false;
  }

  bone_matrices.assign(_classic_static_bones.size(), glm::mat4x4(1.f));
  return true;
}

void Model::calcClassicStaticBones(glm::mat4x4 const& model_view)
{
  if (_classic_static_bones.empty())
  {
    return;
  }

  // One-time: derive each billboard glow/flame CARD's local texture basis (normal/up/right) from its
  // geometry + UVs -- the same derivation the animated path (calcBones) does. Static classic doodads
  // (Karazhan chandeliers, sconces, candelabras) carry their candle FLAME as a mesh card on a spherical
  // (0x8) or cylindrical-lock (0x10/0x20/0x40) billboard bone; without a basis they'd render flat/sideways.
  if (!_static_bb_bases_computed)
  {
    _static_bb_bases_computed = true;
    for (std::size_t bi = 0; bi < _classic_static_bones.size(); ++bi)
    {
      auto& sb = _classic_static_bones[bi];
      if (!(sb.flags & (0x8u | 0x10u | 0x20u | 0x40u))) continue;

      ModelVertex const* v0 = nullptr; ModelVertex const* v1 = nullptr; ModelVertex const* v2 = nullptr;
      for (auto const& v : _vertices)
      {
        if (v.bones[0] != static_cast<uint8_t>(bi)) continue;
        if (!v0) { v0 = &v; continue; }
        if (!v1 && (v.texcoords[0] != v0->texcoords[0])) { v1 = &v; continue; }
        if (v1 && !v2)
        {
          glm::vec2 const d1 = v1->texcoords[0] - v0->texcoords[0];
          glm::vec2 const d2 = v.texcoords[0] - v0->texcoords[0];
          if (std::abs(d1.x * d2.y - d2.x * d1.y) > 1e-8f) { v2 = &v; break; }
        }
      }
      if (!v0 || !v1 || !v2) continue;

      // _vertices are already fixCoordSystem'd once at load -- do NOT re-apply here (see the calcBones note):
      // a double fixCoordSystem tips the derived basis ~90deg and rendered directional flame cards sideways.
      glm::vec3 const p0 = v0->position;
      glm::vec3 const e1 = v1->position - p0;
      glm::vec3 const e2 = v2->position - p0;
      glm::vec2 const duv1 = v1->texcoords[0] - v0->texcoords[0];
      glm::vec2 const duv2 = v2->texcoords[0] - v0->texcoords[0];
      float const det = duv1.x * duv2.y - duv2.x * duv1.y;
      if (std::abs(det) < 1e-8f) continue;
      float const r = 1.0f / det;
      glm::vec3 const tangent   = (e1 * duv2.y - e2 * duv1.y) * r;
      glm::vec3 const bitangent = (e2 * duv1.x - e1 * duv2.x) * r;
      glm::vec3 normal = glm::cross(e1, e2);
      if (glm::length(normal) < 1e-8f || glm::length(bitangent) < 1e-8f) continue;
      normal = glm::normalize(normal);
      glm::vec3 up = -bitangent;                       // texture V grows downward -> screen-up = -V
      up = up - normal * glm::dot(up, normal);
      if (glm::length(up) < 1e-8f) continue;
      up = glm::normalize(up);
      glm::vec3 right = glm::normalize(glm::cross(up, normal));
      if (glm::dot(right, tangent) < 0.0f) right = -right;

      sb.bb_local_normal = normal; sb.bb_local_up = up; sb.bb_local_right = right; sb.basis_ok = true;
    }
  }

  // Camera basis expressed in this doodad's MODEL space = the ROWS of the (full model->view) 3x3.
  glm::vec3 const camRight = glm::normalize(glm::vec3(model_view[0][0], model_view[1][0], model_view[2][0]));
  glm::vec3 const camUp    = glm::normalize(glm::vec3(model_view[0][1], model_view[1][1], model_view[2][1]));
  glm::vec3 const camFwd   = glm::normalize(glm::vec3(model_view[0][2], model_view[1][2], model_view[2][2]));

  for (std::size_t i = 0; i < _classic_static_bones.size(); ++i)
  {
    auto const& bone = _classic_static_bones[i];
    glm::mat4x4 matrix = classic_bone_local_matrix(bone.flags, bone.pivot, model_view);

    if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < i)
    {
      matrix = bone_matrices[static_cast<std::size_t>(bone.parent)] * matrix;
    }

    // Billboard the flame/glow CARD so it faces the camera instead of rendering in its flat bind pose
    // (Karazhan chandelier candle flames = CANDLEFLAMEORANGE cards on billboard bones, were sideways). Same
    // card-basis math as Bone::calcMatrix's billboard branch, applied to the static hierarchy matrix.
    bool const spherical = (bone.flags & 0x8u) != 0;
    bool const cylindrical = (bone.flags & (0x10u | 0x20u | 0x40u)) != 0;
    if (bone.basis_ok && (spherical || cylindrical))
    {
      glm::mat3 const local(bone.bb_local_normal, bone.bb_local_right, bone.bb_local_up);
      glm::mat3 bb3(1.0f);
      bool apply = true;
      if (spherical)
      {
        // Per-card upright fix (see Bone::calcMatrix's billboard branch): keep the derived up if it points
        // world-up, else re-fixCoordSystem the basis to swing a texture-rotated (horizontal) up vertical.
        bool const refix = std::abs(bone.bb_local_up.y) < std::abs(bone.bb_local_right.y);
        glm::mat3 const local_sph(refix ? fixCoordSystem(bone.bb_local_normal) : bone.bb_local_normal,
                                  refix ? fixCoordSystem(bone.bb_local_right)  : bone.bb_local_right,
                                  refix ? fixCoordSystem(bone.bb_local_up)     : bone.bb_local_up);
        glm::mat3 const cam(camFwd, camRight, camUp);
        bb3 = cam * glm::transpose(local_sph);
      }
      else // cylindrical lock-Z -> render authored REST pose (static), matching in-game
      {
        // Chains (LavaPots) + candle threads use lock-Z; in-game they read static, and any billboard swung
        // the multi-quad mesh to face the camera. User-confirmed rest pose matches in-game. Don't billboard.
        // (flame cards are SPHERICAL and handled by the branch above, so this doesn't affect them.)
        apply = false;
      }
      if (apply)
      {
        glm::vec4 const world_pivot = matrix * glm::vec4(bone.pivot, 1.0f);
        glm::vec3 const bone_scale(glm::length(glm::vec3(matrix[0])),
                                   glm::length(glm::vec3(matrix[1])),
                                   glm::length(glm::vec3(matrix[2])));
        glm::mat4x4 bb(1.0f);
        bb[0] = glm::vec4(bb3[0] * bone_scale.x, 0.0f);
        bb[1] = glm::vec4(bb3[1] * bone_scale.y, 0.0f);
        bb[2] = glm::vec4(bb3[2] * bone_scale.z, 0.0f);
        glm::vec4 const rotated_pivot = bb * glm::vec4(bone.pivot, 1.0f);
        bb[3] = glm::vec4(glm::vec3(world_pivot) - glm::vec3(rotated_pivot), 1.0f);
        matrix = bb;
      }
    }

    bone_matrices[i] = matrix;

    // Particle emitters attach to bones by reading the WotLK-style Bone objects: model->bones[bone].mat
    // (spawn position) and .mrot (emission direction). But the classic static-bone path computes matrices
    // ONLY into bone_matrices (which the BODY shader uses) and leaves bones[].mat at its ZERO default -- so
    // every emitter spawned its particles at the model origin (0,0,0) emitting straight up (verified via the
    // [PARTDBG] log: boneWorldPos/spawnPos were all 0). Mirror the computed matrix into the matching Bone so
    // emitters attach at the correct bone position/orientation. (Bind-pose = identity here, so this puts
    // each emitter at its AUTHORED model-space position -- the arms/shoulders/feet -- instead of the origin.)
    if (i < bones.size())
    {
      bones[i].mat = matrix;
      bones[i].mrot = glm::mat4x4(glm::mat3x3(matrix));
    }
  }
}

void Model::waitForChildrenLoaded()
{
  for (auto& tex : _textures)
  {
    tex.get()->wait_until_loaded();
  }

  for (auto& pair : _replaceTextures)
  {
    pair.second.get()->wait_until_loaded();
  }
}


bool Model::isAnimated(const BlizzardArchive::ClientFile& f)
{
  animGeometry = false;
  animBones = false;
  _per_instance_animation = false;

  ModelVertex const* verts = reinterpret_cast<ModelVertex const*>(f.getBuffer() + header.ofsVertices);
  auto const bone_flags = [this, &f](std::uint8_t bone_index) -> std::uint32_t
  {
    if (bone_index >= header.nBones)
    {
      return 0;
    }

    if (_uses_classic_layout)
    {
      auto const* bones = reinterpret_cast<ClassicModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      return bones[bone_index].flags;
    }

    auto const* bones = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);
    return bones[bone_index].flags;
  };

  for (size_t i = 0; i < header.nVertices && !animGeometry; ++i)
  {
    for (size_t b = 0; b < 4; b++)
    {
      if (verts[i].weights[b] > 0)
      {
        auto const flags = bone_flags(verts[i].bones[b]);
        bool const billboard = (flags & (0x78)); // billboard | billboard_lock_[xyz]

        if ((flags & 0x200) || billboard)
        {
          if (billboard)
          {
            // if we have billboarding, the model will need per-instance animation
            _per_instance_animation = true;
          }
          animGeometry = true;
          break;
        }
      }
    }
  }

  if (animGeometry || header.nParticleEmitters || header.nRibbonEmitters || header.nLights)
  {
    animBones = true;
  }
  else
  {
    if (_uses_classic_layout)
    {
      auto const* bo = reinterpret_cast<ClassicModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      for (size_t i = 0; i < header.nBones; ++i)
      {
        ClassicModelBoneDef const& bb = bo[i];
        if (bb.translation.type || bb.rotation.type || bb.scaling.type)
        {
          animBones = true;
          break;
        }
      }
    }
    else
    {
      auto const* bo = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      for (size_t i = 0; i < header.nBones; ++i)
      {
        ModelBoneDef const& bb = bo[i];
        if (bb.translation.type || bb.rotation.type || bb.scaling.type)
        {
          animBones = true;
          break;
        }
      }
    }
  }

  animTextures = header.nTexAnims > 0;

  // animated colors
  if (header.nColors)
  {
    ModelColorDef const* cols = reinterpret_cast<ModelColorDef const*>(f.getBuffer() + header.ofsColors);
    for (size_t i = 0; i<header.nColors; ++i)
    {
      if (cols[i].color.type != 0 || cols[i].opacity.type != 0)
      {
        return true;
      }
    }
  }

  // animated opacity
  if (header.nTransparency)
  {
    if (_uses_classic_layout)
    {
      ClassicModelTransDef const* trs = reinterpret_cast<ClassicModelTransDef const*>(f.getBuffer() + header.ofsTransparency);
      for (size_t i = 0; i < header.nTransparency; ++i)
      {
        if (trs[i].trans.type != 0)
        {
          return true;
        }
      }
    }
    else
    {
      ModelTransDef const* trs = reinterpret_cast<ModelTransDef const*>(f.getBuffer() + header.ofsTransparency);
      for (size_t i = 0; i < header.nTransparency; ++i)
      {
        if (trs[i].trans.type != 0)
        {
          return true;
        }
      }
    }
  }

  // guess not...
  return animGeometry || animTextures || animBones;
}


glm::vec3 fixCoordSystem(glm::vec3 v)
{
  return glm::vec3(v.x, v.z, -v.y);
}

namespace
{
  glm::vec3 fixCoordSystem2(glm::vec3 v)
  {
    return glm::vec3(v.x, v.z, v.y);
  }

  glm::quat fixCoordSystemQuat(glm::quat v)
  {
    return glm::quat(-v.x, -v.z, v.y, v.w);
  }
}


void Model::initCommon(const BlizzardArchive::ClientFile& f)
{
  // vertices, normals, texcoords
  _vertices = M2Array<ModelVertex>(f, header.ofsVertices, header.nVertices);

  // Ground-footprint radius of the RENDER mesh, computed from the RAW M2 vertices (before the
  // Z-up -> Y-up coord fix below), so the horizontal plane is M2 XY. The client sizes its selection
  // circle from this (particle-free) body extent, not header.bounding_box_radius. Radius is measured
  // from the mesh's horizontal centre (not the origin) so off-centre rigs don't skew it.
  if (!_vertices.empty())
  {
    float xmin = _vertices[0].position.x, xmax = xmin;
    float ymin = _vertices[0].position.y, ymax = ymin;
    for (auto const& v : _vertices)
    {
      xmin = std::min(xmin, v.position.x); xmax = std::max(xmax, v.position.x);
      ymin = std::min(ymin, v.position.y); ymax = std::max(ymax, v.position.y);
    }
    float const cx = 0.5f * (xmin + xmax);
    float const cy = 0.5f * (ymin + ymax);
    float r2 = 0.f;
    for (auto const& v : _vertices)
    {
      float const dx = v.position.x - cx, dy = v.position.y - cy;
      r2 = std::max(r2, dx * dx + dy * dy);
    }
    footprint_radius = std::sqrt(r2);
  }

  // EXACT client selection-circle base radius (see Model.h). Read the STAND animation (anim id 0)
  // sequence's own bounding box from the raw M2 (classic layout: sequence array is 0x44-byte records,
  // bounds_min @ +0x24, bounds_max @ +0x30), then base = sqrt( sqrt(dx^2 + dy^2) * 0.5 ). dx/dy are
  // the M2 (Z-up) X/Y extents, read pre-fixCoordSystem, matching the client's math exactly.
  if (_uses_classic_layout)
  {
    char const* const buf = f.getBuffer();
    std::size_t const fsize = f.getSize();
    std::uint32_t const n_anim = *reinterpret_cast<std::uint32_t const*>(buf + 0x1c);
    std::uint32_t const ofs_anim = *reinterpret_cast<std::uint32_t const*>(buf + 0x20);
    constexpr std::size_t kSeqStride = 0x44;
    if (ofs_anim && n_anim && ofs_anim + static_cast<std::size_t>(n_anim) * kSeqStride <= fsize)
    {
      for (std::uint32_t i = 0; i < n_anim; ++i)
      {
        char const* const seq = buf + ofs_anim + static_cast<std::size_t>(i) * kSeqStride;
        std::uint16_t const anim_id = *reinterpret_cast<std::uint16_t const*>(seq);
        if (anim_id != 0) continue; // stand/idle
        auto const* mn = reinterpret_cast<float const*>(seq + 0x24);
        auto const* mx = reinterpret_cast<float const*>(seq + 0x30);
        float const dx = mx[0] - mn[0];
        float const dy = mx[1] - mn[1];
        float const diag = std::sqrt(dx * dx + dy * dy);
        if (diag > 0.001f)
        {
          selection_base_radius = std::sqrt(diag * 0.5f);
        }
        break;
      }
    }
  }

  for (auto& v : _vertices)
  {
    v.position = fixCoordSystem(v.position);
    v.normal = fixCoordSystem(v.normal);

    // Classic (1.12) M2 vertices have only ONE texcoord; the 8 bytes we read as the second texcoord
    // are unused/garbage (WoW Model Viewer, which renders these correctly, reads a single texcoord +
    // 8 unused bytes). Two-texture passes (e.g. the dusty light-ray cone) sampled that garbage uv2 ->
    // a broken/stretched second layer. Mirror the valid first texcoord into the second for classic.
    if (_uses_classic_layout)
    {
      v.texcoords[1] = v.texcoords[0];
    }
  }

  // textures
  ModelTextureDef const* texdef = reinterpret_cast<ModelTextureDef const*>(f.getBuffer() + header.ofsTextures);
  _textureFilenames.resize(header.nTextures);
  _specialTextures.resize(header.nTextures);
  _texture_flags.resize(header.nTextures);
  int classic_missing_texture_placeholders = 0;

  for (size_t i = 0; i < header.nTextures; ++i)
  {
    _texture_flags[i] = texdef[i].flags;
    if (texdef[i].type == 0)
    {
      _specialTextures[i] = -1;

      if (texdef[i].nameLen == 0)
      {
        _textureFilenames[i] = "tileset/generic/black.blp";
        continue;
      }

      if (texdef[i].nameOfs >= f.getSize() || texdef[i].nameLen > f.getSize() - texdef[i].nameOfs)
      {
        LogError << "Texture " << i << " name is outside model file for '" << _file_key.stringRepr() << "'" << std::endl;
        _textureFilenames[i] = "tileset/generic/black.blp";
        continue;
      }

      const char* blp_ptr = f.getBuffer() + texdef[i].nameOfs;
      // some tools export the size without accounting for the \0
      bool invalid_size = *(blp_ptr + texdef[i].nameLen-1) != '\0';
      _textureFilenames[i] = std::string(blp_ptr, texdef[i].nameLen - (invalid_size ? 0 : 1));

      if (is_null_asset_reference(_textureFilenames[i]))
      {
        _textureFilenames[i] = "tileset/generic/black.blp";
        classic_missing_texture_placeholders++;
        continue;
      }

      if (_uses_classic_layout
          && !Noggit::Application::NoggitApplication::instance()->clientData()->exists(_textureFilenames[i]))
      {
        _textureFilenames[i] = "tileset/generic/black.blp";
        classic_missing_texture_placeholders++;
      }
    }
    else
    {
      if (_uses_classic_layout)
      {
        _specialTextures[i] = texdef[i].type;
        _textureFilenames[i] = "tileset/generic/black.blp";
        continue;
      }

#ifndef NO_REPLACIBLE_TEXTURES_HACK
      // Preserve the actual texture type so that _replaceTextures overrides set
      // by reloadCreatureSpawns() are found in bindTexture.  Covers both monster
      // textures (types 11-13) and humanoid baked-skin textures (type 1) and
      // anything else.  If no override is registered the placeholder is still
      // black.blp, identical to the old behaviour.
      _specialTextures[i] = texdef[i].type;
      _textureFilenames[i] = "tileset/generic/black.blp";
#else
      //! \note special texture - only on characters and such... Noggit should not even render these.
      //! \todo Check if this is actually correct. Or just remove it.

      _specialTextures[i] = texdef[i].type;

      if (texdef[i].type == 3)
      {
        _textureFilenames[i] = "Item\\ObjectComponents\\Weapon\\ArmorReflect4.BLP";
        // a fix for weapons with type-3 textures.
        _replaceTextures.emplace (texdef[i].type, _textureFilenames[i]);
      }
#endif
    }
  }

  if (_uses_classic_layout && classic_missing_texture_placeholders > 0)
  {
    LogError << "Classic M2 texture placeholder " << classic_missing_texture_placeholders
             << " texture(s) for " << _file_key.stringRepr() << std::endl;
  }

  // init colors
  // Classic (1.12) layout carries the same two tracks in the older 28-byte animation-block form;
  // it was previously skipped entirely, which killed the color/alpha pulse on ~10% of vanilla
  // models (portals, glow doodads, colored creature FX). Mirrors the transparency load below.
  if (_uses_classic_layout && header.nColors)
  {
    _colors.reserve(header.nColors);
    ClassicModelColorDef const* colorDefs = reinterpret_cast<ClassicModelColorDef const*>(f.getBuffer() + header.ofsColors);
    for (size_t i = 0; i < header.nColors; ++i)
    {
      _colors.emplace_back (f, colorDefs[i], _global_sequences.data());
    }
  }
  else if (header.nColors)
  {
    _colors.reserve(header.nColors);
    ModelColorDef const* colorDefs = reinterpret_cast<ModelColorDef const*>(f.getBuffer() + header.ofsColors);
    for (size_t i = 0; i < header.nColors; ++i)
    {
      _colors.emplace_back (f, colorDefs[i], _global_sequences.data());
    }
  }

  // init transparency
  _transparency_lookup = M2Array<int16_t>(f, header.ofsTransparencyLookup, header.nTransparencyLookup);

  if (_uses_classic_layout)
  {
    auto const classic_attachments = M2Array<ClassicModelAttachmentDef>(f, header.ofsAttachments, header.nAttachments);
    _attachments.clear();
    _attachments.reserve(classic_attachments.size());

    for (auto const& classic_attachment : classic_attachments)
    {
      _attachments.push_back(translate_classic_attachment(classic_attachment));
    }
  }
  else
  {
    _attachments = M2Array<ModelAttachmentDef>(f, header.ofsAttachments, header.nAttachments);
  }

  _attachment_lookup = M2Array<int16_t>(f, header.ofsAttachLookup, header.nAttachLookup);

  if (_uses_classic_layout
      && classic_m2_debug_enabled()
      && _file_key.hasFilepath()
      && _file_key.filepath().starts_with("character/"))
  {
    std::ostringstream attachment_log;
    attachment_log << "Classic attachment dump model='" << _file_key.stringRepr() << "'"
                   << " attachmentCount=" << _attachments.size()
                   << " lookupCount=" << _attachment_lookup.size()
                   << " lookup[0,1,2,5,6,11,26]={";
    for (int lookup_id : {0, 1, 2, 5, 6, 11, 26})
    {
      if (lookup_id != 0)
      {
        attachment_log << ", ";
      }
      attachment_log << lookup_id << ':';
      if (lookup_id >= 0 && static_cast<std::size_t>(lookup_id) < _attachment_lookup.size())
      {
        attachment_log << _attachment_lookup[lookup_id];
      }
      else
      {
        attachment_log << "<out>";
      }
    }
    attachment_log << "} lookupRecords=";

    bool first_lookup_record = true;
    for (int lookup_id : {0, 1, 2, 5, 6, 11, 26})
    {
      if (lookup_id < 0 || static_cast<std::size_t>(lookup_id) >= _attachment_lookup.size())
      {
        continue;
      }

      auto const lookup_index = _attachment_lookup[lookup_id];
      if (lookup_index < 0 || static_cast<std::size_t>(lookup_index) >= _attachments.size())
      {
        continue;
      }

      auto const& attachment = _attachments[lookup_index];
      if (!first_lookup_record)
      {
        attachment_log << ' ';
      }
      first_lookup_record = false;
      attachment_log << "[lookupId=" << lookup_id
                     << " idx=" << lookup_index
                     << " id=" << attachment.id
                     << " bone=" << attachment.bone
                     << " pos={" << attachment.pos.x << ',' << attachment.pos.y << ',' << attachment.pos.z << "}]";
    }

    attachment_log << " matchingIds=";

    bool first_record = true;
    for (std::size_t index = 0; index < _attachments.size(); ++index)
    {
      auto const& attachment = _attachments[index];
      if (attachment.id != 0 && attachment.id != 1 && attachment.id != 2
          && attachment.id != 5 && attachment.id != 6 && attachment.id != 11
          && attachment.id != 26)
      {
        continue;
      }

      if (!first_record)
      {
        attachment_log << ' ';
      }
      first_record = false;
      attachment_log << "[idx=" << index
                     << " id=" << attachment.id
                     << " bone=" << attachment.bone
                     << " pos={" << attachment.pos.x << ',' << attachment.pos.y << ',' << attachment.pos.z << "}]";
    }

    LogDebug << attachment_log.str() << std::endl;
  }

  if (_uses_classic_layout && header.nTransparency)
  {
    _transparency.reserve(header.nTransparency);
    ClassicModelTransDef const* trDefs = reinterpret_cast<ClassicModelTransDef const*>(f.getBuffer() + header.ofsTransparency);

    for (size_t i = 0; i < header.nTransparency; ++i)
    {
      _transparency.emplace_back(f, trDefs[i], _global_sequences.data());
    }
  }
  else if (header.nTransparency)
  {
    _transparency.reserve(header.nTransparency);
    ModelTransDef const* trDefs = reinterpret_cast<ModelTransDef const*>(f.getBuffer() + header.ofsTransparency);
    for (size_t i = 0; i < header.nTransparency; ++i)
    {
      _transparency.emplace_back (f, trDefs[i], _global_sequences.data());
    }
  }


  // just use the first LOD/view

  if (header.nViews > 0) {
    std::unique_ptr<BlizzardArchive::ClientFile> skin_file;
    BlizzardArchive::ClientFile const* view_file = &f;
    ModelView embedded_view = {};
    ModelView const* view = nullptr;

    if (_embedded_view_offset)
    {
      auto classic_views = reinterpret_cast<ClassicModelView const*>(f.getBuffer() + _embedded_view_offset);
      ClassicModelView const* classic_view = nullptr;
      uint32_t classic_view_index = 0;
      for (uint32_t view_index = 0; view_index < header.nViews; ++view_index)
      {
        ClassicModelView const* candidate = classic_views + view_index;
        if (!range_fits(f, candidate->ofs_index, candidate->n_index, sizeof(uint16_t))
            || !range_fits(f, candidate->ofs_triangle, candidate->n_triangle, sizeof(uint16_t))
            || !range_fits(f, candidate->ofs_vertex_property, candidate->n_vertex_property, sizeof(uint32_t))
            || !range_fits(f, candidate->ofs_submesh, candidate->n_submesh, sizeof(ClassicModelGeoset))
            || !range_fits(f, candidate->ofs_texture_unit, candidate->n_texture_unit, sizeof(ModelTexUnit)))
        {
          continue;
        }

        classic_view = candidate;
        classic_view_index = view_index;
        break;
      }

      if (!classic_view)
      {
        LogError << "No valid embedded skin view for '" << _file_key.stringRepr() << "'" << std::endl;
        return;
      }

      if (classic_m2_debug_enabled())
      {
        LogDebug << "Classic embedded skin view model='" << _file_key.stringRepr()
                 << "' selected=" << classic_view_index
                 << " lod=" << classic_view->lod
                 << " indices=" << classic_view->n_index
                 << " triangles=" << classic_view->n_triangle
                 << " submeshes=" << classic_view->n_submesh
                 << " textureUnits=" << classic_view->n_texture_unit
                 << std::endl;
      }

      std::memcpy(embedded_view.id, "SKIN", 4);
      embedded_view.n_index = classic_view->n_index;
      embedded_view.ofs_index = classic_view->ofs_index;
      embedded_view.n_triangle = classic_view->n_triangle;
      embedded_view.ofs_triangle = classic_view->ofs_triangle;
      embedded_view.n_vertex_property = classic_view->n_vertex_property;
      embedded_view.ofs_vertex_property = classic_view->ofs_vertex_property;
      embedded_view.n_submesh = classic_view->n_submesh;
      embedded_view.ofs_submesh = classic_view->ofs_submesh;
      embedded_view.n_texture_unit = classic_view->n_texture_unit;
      embedded_view.ofs_texture_unit = classic_view->ofs_texture_unit;
      embedded_view.lod = classic_view->lod;
      view = &embedded_view;
    }
    else
    {
      // indices - allocate space, too
      std::string lodname = _file_key.filepath().substr(0, _file_key.filepath().length() - 3);
      lodname.append("00.skin");
      skin_file = std::make_unique<BlizzardArchive::ClientFile>(lodname, Noggit::Application::NoggitApplication::instance()->clientData());
      if (skin_file->isEof()) {
        LogError << "loading skinfile " << lodname << std::endl;
        skin_file->close();
        return;
      }

      view_file = skin_file.get();
      view = reinterpret_cast<ModelView const*>(view_file->getBuffer());
    }

    if (!range_fits(*view_file, view->ofs_index, view->n_index, sizeof(uint16_t))
        || !range_fits(*view_file, view->ofs_triangle, view->n_triangle, sizeof(uint16_t))
        || !range_fits(*view_file, view->ofs_vertex_property, view->n_vertex_property, sizeof(uint32_t))
        || !range_fits(*view_file, view->ofs_submesh, view->n_submesh, _uses_classic_layout ? sizeof(ClassicModelGeoset) : sizeof(ModelGeoset))
        || !range_fits(*view_file, view->ofs_texture_unit, view->n_texture_unit, sizeof(ModelTexUnit)))
    {
      LogError << "Invalid skin/view data for '" << _file_key.stringRepr() << "'" << std::endl;
      return;
    }

    auto indexLookup = reinterpret_cast<uint16_t const*>(view_file->getBuffer() + view->ofs_index);
    auto triangles = reinterpret_cast<uint16_t const*>(view_file->getBuffer() + view->ofs_triangle);

    // render ops
    std::vector<ModelGeoset> classic_model_geosets;
    ModelGeoset const* model_geosets = nullptr;
    if (_uses_classic_layout)
    {
      auto classic_geosets = reinterpret_cast<ClassicModelGeoset const*>(view_file->getBuffer() + view->ofs_submesh);
      classic_model_geosets.reserve(view->n_submesh);
      for (size_t i = 0; i < view->n_submesh; ++i)
      {
        classic_model_geosets.push_back(translate_classic_geoset(classic_geosets[i]));
      }
      model_geosets = classic_model_geosets.data();

      if (!_logged_classic_character_geosets
          && ((file_key().hasFilepath() && file_key().filepath().starts_with("character/"))
              || should_log_classic_skin_model(file_key())))
      {
        _logged_classic_character_geosets = true;
        std::ostringstream geoset_log;
        geoset_log << "Classic skin geosets model='" << file_key().stringRepr() << "'";
        for (size_t geoset_index = 0; geoset_index < classic_model_geosets.size(); ++geoset_index)
        {
          auto const& geoset = classic_model_geosets[geoset_index];
          geoset_log << " [submesh=" << geoset_index
                     << " id=" << geoset.id
                     << " vstart=" << geoset.vstart
                     << " vcount=" << geoset.vcount
                     << " istart=" << geoset.istart
                     << " icount=" << geoset.icount
                     << " boneCount=" << geoset.d3
                     << " boneStart=" << geoset.d4
                     << " boneInfluences=" << geoset.d5
                     << " rootBone=" << geoset.d6
                     << "]";
        }
        LogDebug << geoset_log.str() << std::endl;
      }
    }
    else
    {
      model_geosets = reinterpret_cast<ModelGeoset const*>(view_file->getBuffer() + view->ofs_submesh);
    }
    auto texture_unit = reinterpret_cast<ModelTexUnit const*>(view_file->getBuffer() + view->ofs_texture_unit);

    _indices.resize (view->n_triangle);

    if (_uses_classic_layout)
    {
      std::vector<ModelVertex> global_vertices = std::move(_vertices);
      _vertices.assign(view->n_index, ModelVertex{});

      for (size_t i = 0; i < _vertices.size(); ++i)
      {
        uint16_t const global_vertex = indexLookup[i];
        if (global_vertex < global_vertices.size())
        {
          _vertices[i] = global_vertices[global_vertex];
        }
      }

      if (view->n_vertex_property >= view->n_index
          && range_fits(f, header.ofsBoneLookup, header.nBoneLookup, sizeof(uint16_t)))
      {
        auto vertex_properties = reinterpret_cast<uint8_t const*>(view_file->getBuffer() + view->ofs_vertex_property);
        auto bone_lookup = reinterpret_cast<uint16_t const*>(f.getBuffer() + header.ofsBoneLookup);
        bool const log_classic_skin_details = should_log_classic_skin_model(file_key());

        for (size_t geoset_index = 0; geoset_index < view->n_submesh; ++geoset_index)
        {
          auto const& geoset = model_geosets[geoset_index];
          size_t const vertex_end = std::min<size_t>(view->n_index, static_cast<size_t>(geoset.vstart) + geoset.vcount);
          uint16_t const influences = std::min<uint16_t>(4, geoset.d5);
          std::size_t vertices_with_weights = 0;
          std::size_t vertices_with_zero_weights = 0;
          std::size_t invalid_bone_lookup_count = 0;
          std::size_t remapped_bone_count = 0;
          std::ostringstream remap_detail_log;
          if (log_classic_skin_details)
          {
            remap_detail_log << "Classic skin bone remap details model='" << file_key().stringRepr()
                             << "' submesh=" << geoset_index
                             << " samples=";
          }

          for (size_t vertex = geoset.vstart; vertex < vertex_end; ++vertex)
          {
            auto const original_vertex = _vertices[vertex];
            bool const has_weight = _vertices[vertex].weights[0] || _vertices[vertex].weights[1]
                                 || _vertices[vertex].weights[2] || _vertices[vertex].weights[3];
            if (has_weight)
            {
              ++vertices_with_weights;
            }
            else
            {
              ++vertices_with_zero_weights;
            }

            for (uint16_t bone = 0; bone < influences; ++bone)
            {
              uint16_t const bone_lookup_index = static_cast<uint16_t>(geoset.d4 + vertex_properties[vertex * 4 + bone]);
              if (bone_lookup_index < header.nBoneLookup && bone_lookup[bone_lookup_index] < header.nBones)
              {
                auto remapped_bone = static_cast<uint8_t>(bone_lookup[bone_lookup_index]);
                // Classic creature M2 vertices already carry correct GLOBAL bone indices.
                // The per-view bone-lookup remap appears to corrupt them on these models
                // (it rewrote ~408 body vertices to different bones than their originals),
                // making part of the mesh follow the wrong bone and freeze at a different
                // pose -> the doubled creature. Prefer the vertex's original global bone.
                // Scoped to elementalearth for now to verify before generalizing.
                bool const keep_original_global_bone =
                    file_key().hasFilepath()
                    && file_key().filepath().find("elementalearth") != std::string::npos
                    && _vertices[vertex].bones[bone] < header.nBones;
                if (keep_original_global_bone)
                {
                  remapped_bone = _vertices[vertex].bones[bone];
                }
                else if (remapped_bone == 0
                    && vertex_properties[vertex * 4 + bone] != 0
                    && _vertices[vertex].bones[bone] < header.nBones)
                {
                  remapped_bone = _vertices[vertex].bones[bone];
                }
                if (_vertices[vertex].bones[bone] != remapped_bone)
                {
                  ++remapped_bone_count;
                }
                _vertices[vertex].bones[bone] = remapped_bone;
              }
              else
              {
                ++invalid_bone_lookup_count;
              }
            }

            for (uint16_t bone = influences; bone < 4; ++bone)
            {
              _vertices[vertex].weights[bone] = 0;
              _vertices[vertex].bones[bone] = 0;
            }

            if (log_classic_skin_details && vertex < static_cast<size_t>(geoset.vstart) + 4)
            {
              remap_detail_log << " [v=" << vertex
                               << " src=" << indexLookup[vertex]
                               << " props=("
                               << static_cast<int>(vertex_properties[vertex * 4])
                               << "," << static_cast<int>(vertex_properties[vertex * 4 + 1])
                               << "," << static_cast<int>(vertex_properties[vertex * 4 + 2])
                               << "," << static_cast<int>(vertex_properties[vertex * 4 + 3])
                               << ") oldBones=("
                               << static_cast<int>(original_vertex.bones[0])
                               << "," << static_cast<int>(original_vertex.bones[1])
                               << "," << static_cast<int>(original_vertex.bones[2])
                               << "," << static_cast<int>(original_vertex.bones[3])
                               << ") weights=("
                               << static_cast<int>(original_vertex.weights[0])
                               << "," << static_cast<int>(original_vertex.weights[1])
                               << "," << static_cast<int>(original_vertex.weights[2])
                               << "," << static_cast<int>(original_vertex.weights[3])
                               << ") newBones=("
                               << static_cast<int>(_vertices[vertex].bones[0])
                               << "," << static_cast<int>(_vertices[vertex].bones[1])
                               << "," << static_cast<int>(_vertices[vertex].bones[2])
                               << "," << static_cast<int>(_vertices[vertex].bones[3])
                               << ")]";
            }
          }

          if (log_classic_skin_details)
          {
            LogDebug << "Classic skin bone remap model='" << file_key().stringRepr()
                     << "' submesh=" << geoset_index
                     << " id=" << geoset.id
                     << " vertices=[" << geoset.vstart << ".." << vertex_end << ")"
                     << " boneStart=" << geoset.d4
                     << " boneCount=" << geoset.d3
                     << " influences=" << influences
                     << " rootBone=" << geoset.d6
                     << " weightedVertices=" << vertices_with_weights
                     << " zeroWeightVertices=" << vertices_with_zero_weights
                     << " remappedBones=" << remapped_bone_count
                     << " invalidBoneLookups=" << invalid_bone_lookup_count
                     << std::endl;
            LogDebug << remap_detail_log.str() << std::endl;
          }
        }
      }

      for (size_t i = 0; i < _indices.size(); ++i)
      {
        _indices[i] = triangles[i] < view->n_index ? triangles[i] : 0;
      }
    }
    else
    {
      for (size_t i (0); i < _indices.size(); ++i) {
        _indices[i] = triangles[i] < view->n_index ? indexLookup[triangles[i]] : 0;
      }
    }
    
    _texture_lookup = M2Array<uint16_t>(f, header.ofsTexLookup, header.nTexLookup);
    _texture_animation_lookups = M2Array<int16_t>(f, header.ofsTexAnimLookup, header.nTexAnimLookup);
    _texture_unit_lookup = M2Array<int16_t>(f, header.ofsTexUnitLookup, header.nTexUnitLookup);

    showGeosets.resize (view->n_submesh);
    for (size_t i = 0; i<view->n_submesh; ++i)
    {
      showGeosets[i] = true;
    }

    // Vanilla creature/elementalearth.m2 (Lava Surger/Elemental/Firesworn/Garr/...) is built
    // from a structured "core" body PLUS a redundant outer "rock shell" rooted on independent
    // (parent == -1) bones. The shell's classic translation tracks are all-zero for the idle
    // anim while the core body bobs, so the shell stays frozen at bind and visibly separates =
    // the long-standing "double surger" (one animated, one static). The core already contains
    // the full creature, so drop the duplicate shell. Scoped to this model by filepath; keyed
    // off each submesh's root bone being a parent == -1 rock bone so it survives geoset reorder.
    if (_uses_classic_layout && file_key().hasFilepath()
        && file_key().filepath().find("elementalearth") != std::string::npos
        && range_fits(f, header.ofsBones, header.nBones, sizeof(ClassicModelBoneDef)))
    {
      auto const* raw_bones = reinterpret_cast<ClassicModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      for (size_t i = 0; i < view->n_submesh; ++i)
      {
        uint16_t const root_bone = model_geosets[i].d6;
        if (root_bone < header.nBones && raw_bones[root_bone].parent < 0)
        {
          showGeosets[i] = false;
        }
      }
    }

    // Compact "glow/mist core" placeholder mesh (Anomalus MANAMISTBASE): a high-detail sphere crammed
    // exactly at the model CENTROID that renders as an out-of-place opaque "pill"/orb in the torso. The
    // live 1.12 client does NOT draw it (apitrace of Anomalus: the ~180-tri core submesh is never issued
    // -- the client draws body + additive glows + particles instead). All geoset ids are 0 so there's no
    // visibility flag; the signal is GEOMETRIC.
    //
    // 2026-07-03 REGRESSION FIX: the previous version tested only COMPACT (<20% model extent) + DENSE
    // (>100 tris) + 3D-BLOB (min>30% max). That signature ALSO matches legitimate body parts -- a
    // creature-tree scan found it silently hiding a submesh on 91 models (Medivh's hood, Ragnaros/Illidan
    // body segments, a 2030-tri gryphon-mount body, ...). The reported bug was creature 61958 "Echo of
    // Medivh" losing its hood (hood submesh: 107 tris, compact, roundish -> false positive). The missing
    // discriminator is POSITION: the Anomalus pill sits essentially ON the model centroid
    // (|centroid - modelCenter| = 1.6% of model extent), while every false positive is off-centre --
    // Medivh's hood is up at the head (43.7%). Requiring near-coincidence with the model centre keeps the
    // pill hidden and reveals the ~85 body parts that were wrongly culled. NOTE: this is an ADDED
    // restriction, so it can only ever REVEAL geometry -- it cannot newly hide anything that renders today.
    // A handful of genuinely centre-coincident blobs on other models (FrostLord, gryphon mounts, Ragnaros
    // sub0) stay hidden as before and are unverified -- flagged in the report as follow-up.
    if (_uses_classic_layout && file_key().hasFilepath()
        && file_key().filepath().starts_with("creature/")
        && range_fits(f, header.ofsVertices, header.nVertices, sizeof(ModelVertex)))
    {
      auto const* verts = reinterpret_cast<ModelVertex const*>(f.getBuffer() + header.ofsVertices);
      glm::vec3 model_lo(1e30f), model_hi(-1e30f);
      for (uint32_t v = 0; v < header.nVertices; ++v)
      {
        model_lo = glm::min(model_lo, verts[v].position);
        model_hi = glm::max(model_hi, verts[v].position);
      }
      glm::vec3 const model_size = model_hi - model_lo;
      float const model_extent = std::max({model_size.x, model_size.y, model_size.z});
      glm::vec3 const model_center = (model_lo + model_hi) * 0.5f;

      for (size_t i = 0; i < view->n_submesh; ++i)
      {
        auto const& geo = model_geosets[i];
        if ((geo.icount / 3) <= 100) continue; // dense only -- excludes low-poly body parts / billboards
        if (static_cast<uint32_t>(geo.vstart) + geo.vcount > header.nVertices) continue;
        glm::vec3 lo(1e30f), hi(-1e30f);
        glm::dvec3 centroid_acc(0.0);
        for (uint32_t v = geo.vstart; v < static_cast<uint32_t>(geo.vstart) + geo.vcount; ++v)
        {
          lo = glm::min(lo, verts[v].position);
          hi = glm::max(hi, verts[v].position);
          centroid_acc += glm::dvec3(verts[v].position);
        }
        glm::vec3 const centroid = geo.vcount ? glm::vec3(centroid_acc / double(geo.vcount)) : lo;
        glm::vec3 const size = hi - lo;
        float const maxe = std::max({size.x, size.y, size.z});
        float const mine = std::min({size.x, size.y, size.z});
        float const dist_to_center = glm::length(centroid - model_center);
        if (model_extent > 0.0001f && maxe > 0.0001f
            && maxe < 0.20f * model_extent            // compact vs the whole model
            && mine > 0.30f * maxe                    // 3D blob, not a flat billboard card
            && dist_to_center < 0.05f * model_extent) // sits ON the model centroid (a "core", not a body part)
        {
          showGeosets[i] = false;
        }
      }
    }

    _render_flags = M2Array<ModelRenderFlags>(f, header.ofsRenderFlags, header.nRenderFlags);

    // Mark the model as a light emitter if any material is UNLIT + additive (No_Add_Alpha=3 / Add=4) --
    // the self-illuminated glow layer of fires/braziers/lamps/lava props. Used to synthesize a warm
    // point light for hot doodads that carry no authored M2 light (data-driven, replaces filename keywords).
    for (std::size_t i = 0; i < _render_flags.size(); ++i)
    {
      if (_render_flags[i].flags.unlit && (_render_flags[i].blend == 3 || _render_flags[i].blend == 4))
      {
        _emits_light = true;
        break;
      }
    }

    _renderer.initRenderPasses(view, texture_unit, model_geosets);

    if (skin_file)
    {
      skin_file->close();
    }

    // add fake geometry for selection
    if (_renderer.renderPasses().empty())
    {
      _fake_geometry.emplace(this);
    }
  }  
}


FakeGeometry::FakeGeometry(Model* m)
{
  glm::vec3 min = m->header.bounding_box_min, max = m->header.bounding_box_max;

  vertices.emplace_back(min.x, max.y, min.z);
  vertices.emplace_back(min.x, max.y, max.z);
  vertices.emplace_back(max.x, max.y, max.z);
  vertices.emplace_back(max.x, max.y, min.z);

  vertices.emplace_back(min.x, min.y, min.z);
  vertices.emplace_back(min.x, min.y, max.z);
  vertices.emplace_back(max.x, min.y, max.z);
  vertices.emplace_back(max.x, min.y, min.z);

  indices =
  {
    0,1,2,  2,3,0,
    0,4,5,  5,1,0,
    0,3,7,  7,4,0,
    1,5,6,  6,2,1,
    2,6,7,  7,3,2,
    5,6,7,  7,4,5
  }; 
}


void Model::initAnimated(const BlizzardArchive::ClientFile& f)
{
  std::vector<std::unique_ptr<BlizzardArchive::ClientFile>> animation_files;

  if (header.nAnimations > 0) 
  {
    if (_uses_classic_layout)
    {
      auto const* classic_animations = reinterpret_cast<ClassicModelAnimation const*>(f.getBuffer() + header.ofsAnimations);

      for (std::uint32_t animation_index = 0; animation_index < header.nAnimations; ++animation_index)
      {
        auto const& classic_anim = classic_animations[animation_index];
        ModelAnimation anim = {};
        anim.animID = static_cast<int16_t>(classic_anim.animID);
        anim.subAnimID = static_cast<int16_t>(classic_anim.subAnimID);
        anim.length = classic_anim.endTimestamp > classic_anim.startTimestamp
          ? classic_anim.endTimestamp - classic_anim.startTimestamp
          : 1U;
        anim.moveSpeed = classic_anim.moveSpeed;
        anim.flags = classic_anim.flags;
        anim.boxA = classic_anim.boxA;
        anim.boxB = classic_anim.boxB;
        anim.rad = classic_anim.rad;
        anim.NextAnimation = classic_anim.nextAnimation;
        anim.Index = static_cast<int16_t>(animation_index);

        _animation_length[anim.animID] += anim.length;
        // Classic data can repeat (animID, subAnimID) across sequences -- e.g. ShadeWhite/ManaFiend
        // (the arcane elementals) carry THREE Stand sequences that are all sub 0. Keying by
        // subAnimID alone silently dropped all but the last, while _animation_length still summed
        // every one: the variation walk in animate() then ran past its single kept sequence and
        // sampled beyond its keys -- a ~2.7s whole-body freeze at the end of every Stand cycle
        // (masked before the sampler wrapped tracks; exposed by the client-canon hold). Give
        // colliding sub ids the next free slot so every sequence stays playable.
        {
          auto& bucket = _animations_seq_per_id[anim.animID];
          uint16_t sub_key = anim.subAnimID;
          while (bucket.count(sub_key))
          {
            ++sub_key;
          }
          bucket[sub_key] = anim;
        }

        // Capture the authored scheduling fields (dropped by ModelAnimation) so the idle scheduler can
        // reproduce the client's weighted, replay-count-driven variation timing. File/variation order is
        // preserved (matches the client's variationNext chain walk).
        _anim_variations[static_cast<uint16_t>(anim.animID)].push_back(
          AnimVariation{ anim.Index, anim.length, classic_anim.frequency,
                         classic_anim.minimumRepetitions, classic_anim.maximumRepetitions,
                         classic_anim.blendTime });
      }
    }
    else
    {
      std::vector<ModelAnimation> animations(header.nAnimations);

      memcpy(animations.data(), f.getBuffer() + header.ofsAnimations, header.nAnimations * sizeof(ModelAnimation));

      for (auto& anim : animations)
      {
        anim.length = std::max(anim.length, 1U);

        _animation_length[anim.animID] += anim.length;
        _animations_seq_per_id[anim.animID][anim.subAnimID] = anim;

        std::string lodname = _file_key.filepath().substr(0, _file_key.filepath().length() - 3);
        std::stringstream tempname;
        tempname << lodname << anim.animID << "-" << anim.subAnimID << ".anim";
        if (Noggit::Application::NoggitApplication::instance()->clientData()->exists(tempname.str()))
        {
          animation_files.push_back(std::make_unique<BlizzardArchive::ClientFile>(tempname.str(),
              Noggit::Application::NoggitApplication::instance()->clientData()));
        }
      }
    }
  }

  if (animBones)
  {
    auto sanitize_sequence_id = [this](auto& animation_block)
    {
      if (animation_block.seq != -1
          && (animation_block.seq < 0
              || static_cast<std::size_t>(animation_block.seq) >= _global_sequences.size()))
      {
        animation_block.seq = -1;
      }
    };

    // KeyBoneID per bone, captured to build the weapon-grip finger-overlay set below.
    std::vector<int32_t> key_bone_ids;
    key_bone_ids.reserve(header.nBones);

    if (_uses_classic_layout)
    {
      ClassicModelBoneDef const* mb = reinterpret_cast<ClassicModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      for (size_t i = 0; i<header.nBones; ++i)
      {
        auto bone = mb[i];
        sanitize_sequence_id(bone.translation);
        sanitize_sequence_id(bone.rotation);
        sanitize_sequence_id(bone.scaling);
        key_bone_ids.push_back(mb[i].KeyBoneID);
        bones.emplace_back(f, bone, _global_sequences.data());
      }
    }
    else
    {
      ModelBoneDef const* mb = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);
      for (size_t i = 0; i<header.nBones; ++i)
      {
        auto bone = mb[i];
        sanitize_sequence_id(bone.translation);
        sanitize_sequence_id(bone.rotation);
        sanitize_sequence_id(bone.scaling);
        key_bone_ids.push_back(mb[i].KeyBoneID);
        bones.emplace_back(f, bone, _global_sequences.data(), animation_files);
      }
    }

    for (size_t i = 0; i < bones.size(); ++i)
    {
      auto invalid_parent = [this, i](int parent)
      {
        return parent < 0
            || static_cast<size_t>(parent) >= bones.size()
            || static_cast<size_t>(parent) == i;
      };

      if (bones[i].parent < 0)
      {
        continue;
      }

      bool detach_parent = invalid_parent(bones[i].parent);
      std::set<size_t> visited;
      int parent = bones[i].parent;
      while (!detach_parent && parent >= 0)
      {
        auto const parent_index = static_cast<size_t>(parent);
        if (!visited.insert(parent_index).second)
        {
          detach_parent = true;
          break;
        }

        parent = bones[parent_index].parent;
        if (parent >= 0
            && (static_cast<size_t>(parent) >= bones.size()
                || static_cast<size_t>(parent) == i))
        {
          detach_parent = true;
        }
      }

      if (detach_parent)
      {
        if (classic_m2_debug_enabled())
        {
          LogDebug << "Detaching invalid M2 bone parent model='" << _file_key.stringRepr()
                   << "' bone=" << i
                   << " parent=" << bones[i].parent
                   << std::endl;
        }
        bones[i].parent = -1;
      }
    }

    // Weapon-grip finger-overlay set: the finger bones (KeyBoneID 8..17 = Index/Middle/Pinky/Ring/Thumb,
    // right then left) plus their descendant segments. Built in bone-index (parents-first) order so the
    // overlay pass can rely on each bone's parent already being posed. We select by KEY BONE ID -- NOT by
    // "which bones HandsClosed animates" -- because HandsClosed also keys the arms/shoulders (as a static
    // single-frame pose); including those would freeze the upper body. Fingers only.
    _hand_overlay_bones.clear();
    if (bones.size() == key_bone_ids.size())
    {
      std::vector<uint8_t> in_set(bones.size(), 0);
      for (size_t i = 0; i < bones.size(); ++i)
      {
        int32_t const kb = key_bone_ids[i];
        bool finger = (kb >= 8 && kb <= 17);
        int const parent = bones[i].parent;
        if (!finger && parent >= 0 && static_cast<size_t>(parent) < in_set.size() && in_set[parent])
        {
          finger = true; // descendant of a finger bone (a finger segment)
        }
        if (finger)
        {
          in_set[i] = 1;
          _hand_overlay_bones.push_back(static_cast<uint16_t>(i));
        }
      }
    }

    bone_matrices.resize(bones.size());
  }

  if (animTextures) 
  {
    _texture_animations.reserve(header.nTexAnims);
    if (_uses_classic_layout)
    {
      ClassicModelTexAnimDef const* ta = reinterpret_cast<ClassicModelTexAnimDef const*>(f.getBuffer() + header.ofsTexAnims);
      for (size_t i = 0; i < header.nTexAnims; ++i)
      {
        _texture_animations.emplace_back(f, ta[i], _global_sequences.data());
      }
    }
    else
    {
      ModelTexAnimDef const* ta = reinterpret_cast<ModelTexAnimDef const*>(f.getBuffer() + header.ofsTexAnims);
      for (size_t i = 0; i < header.nTexAnims; ++i)
      {
        _texture_animations.emplace_back(f, ta[i], _global_sequences.data());
      }
    }
  }

  bool parsed_classic_effect_particles = false;
  if (classic_effect_particle_parse_enabled_for_model(this) && header.nParticleEmitters)
  {
    if (range_fits(f, header.ofsParticleEmitters, header.nParticleEmitters, sizeof(ClassicModelParticleEmitterDef)))
    {
      auto const* pdefs = reinterpret_cast<ClassicModelParticleEmitterDef const*>(f.getBuffer() + header.ofsParticleEmitters);
      _particles.reserve(_particles.size() + header.nParticleEmitters);

      auto const resolve_embedded_particle_texture = [&](ClassicModelParticleEmitterDef const& emitter)
        -> std::optional<std::uint16_t>
      {
        auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
        auto const model_path = _file_key.hasFilepath() ? _file_key.filepath() : std::string();

        for (auto const& raw_filename : {
               read_embedded_model_string(f, emitter.ofsParticleFileName, emitter.nParticleFileName),
               read_embedded_model_string(f, emitter.ofsModelFileName, emitter.nModelFileName)})
        {
          auto texture_filename = normalize_embedded_particle_texture_filename(raw_filename, model_path);
          if (texture_filename.empty())
          {
            continue;
          }

          if (!client_data->exists(texture_filename))
          {
            continue;
          }

          auto existing_texture = std::find(_textureFilenames.begin(), _textureFilenames.end(), texture_filename);
          if (existing_texture != _textureFilenames.end())
          {
            return static_cast<std::uint16_t>(std::distance(_textureFilenames.begin(), existing_texture));
          }

          if (_textureFilenames.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
          {
            return std::nullopt;
          }

          _textureFilenames.push_back(texture_filename);
          _specialTextures.push_back(-1);
          return static_cast<std::uint16_t>(_textureFilenames.size() - 1);
        }

        return std::nullopt;
      };

      for (std::size_t i = 0; i < header.nParticleEmitters; ++i)
      {
        ClassicModelParticleEmitterDef emitter = pdefs[i];
        if (emitter.texture < 0
            || static_cast<std::size_t>(emitter.texture) >= _textureFilenames.size())
        {
          if (auto embedded_texture = resolve_embedded_particle_texture(emitter))
          {
            emitter.texture = static_cast<int16_t>(*embedded_texture);
          }
          else
          {
            if (classic_effect_debug_enabled())
            {
              LogDebug << "Skipping classic M2 particle emitter model='" << _file_key.stringRepr()
                       << "' index=" << i
                       << " reason=invalidTexture"
                       << " texture=" << emitter.texture
                       << " textureCount=" << _textureFilenames.size()
                       << " particleFile='"
                       << read_embedded_model_string(f, emitter.ofsParticleFileName, emitter.nParticleFileName)
                       << "' modelFile='"
                       << read_embedded_model_string(f, emitter.ofsModelFileName, emitter.nModelFileName)
                       << "'"
                       << std::endl;
            }
            continue;
          }
        }

        emitter.rows = clamp_classic_particle_int16(emitter.rows, 1, 16);
        emitter.cols = clamp_classic_particle_int16(emitter.cols, 1, 16);
        // Keep blend clamped to 0-4: the hand-tuned dusty light-ray motes (and other classic FX) were
        // balanced around this, and widening it to 0-7 reintroduced wrong blend (black-outlined motes).
        emitter.blend = static_cast<std::uint16_t>(std::clamp(static_cast<int>(emitter.blend), 0, 4));
        // EmitterType 3 = SPLINE (e.g. the MC flamecircle ring): keep it so the spline emission path can
        // run. Only force unknown types to 1 (Plane). 1=Plane, 2=Sphere, 3=Spline.
        if (emitter.EmitterType != 1 && emitter.EmitterType != 2 && emitter.EmitterType != 3)
        {
          emitter.EmitterType = 1;
        }
        sanitize_classic_particle_animation_block(emitter.EmissionSpeed, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.SpeedVariation, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.VerticalRange, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.HorizontalRange, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.Gravity, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.Lifespan, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.EmissionRate, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.EmissionAreaLength, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.EmissionAreaWidth, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.Gravity2, header.nGlobalSequences);
        sanitize_classic_particle_animation_block(emitter.en, header.nGlobalSequences);

        try
        {
          _particles.emplace_back(this, f, emitter, _global_sequences.data(), _context);
          parsed_classic_effect_particles = true;

          if (classic_effect_debug_enabled())
          {
            LogDebug << "Loaded classic M2 particle emitter model='" << _file_key.stringRepr()
                     << "' index=" << i
                     << " texture=" << emitter.texture
                     << " textureFile='" << _textureFilenames[static_cast<std::size_t>(emitter.texture)] << "'"
                     << " type=" << static_cast<int>(emitter.EmitterType)
                     << " particleType=" << static_cast<int>(emitter.ParticleType)
                     << " headOrTail=" << static_cast<int>(emitter.HeadorTail)
                     << " flags=" << emitter.flags
                     << " blend=" << static_cast<int>(emitter.blend)
                     << " tiles=" << emitter.cols << "x" << emitter.rows
                     << std::endl;
          }
        }
        catch (std::exception const& error)
        {
          if (classic_effect_debug_enabled())
          {
            LogDebug << "Skipping classic M2 particle emitter model='" << _file_key.stringRepr()
                     << "' index=" << i
                     << " error=" << error.what()
                     << std::endl;
          }
        }
      }
    }
    else if (classic_effect_debug_enabled())
    {
      LogDebug << "Classic M2 particle range rejected model='" << _file_key.stringRepr()
               << "' particles=" << header.nParticleEmitters
               << " offset=" << header.ofsParticleEmitters
               << " fileSize=" << f.getSize()
               << std::endl;
    }
  }

  if (_uses_classic_layout
      && classic_m2_debug_enabled()
      && (header.nParticleEmitters || header.nRibbonEmitters || header.nLights)
      && _particles.empty())
  {
    LogDebug << "Classic M2 effects not loaded model='" << _file_key.stringRepr()
             << "' particles=" << header.nParticleEmitters
             << " ribbons=" << header.nRibbonEmitters
             << " lights=" << header.nLights << std::endl;
  }

  // particle systems
  if (!_uses_classic_layout && header.nParticleEmitters)
  {
    _particles.reserve(header.nParticleEmitters);
    ModelParticleEmitterDef const* pdefs = reinterpret_cast<ModelParticleEmitterDef const*>(f.getBuffer() + header.ofsParticleEmitters);
    for (size_t i = 0; i<header.nParticleEmitters; ++i) 
    {
      try
      {
        _particles.emplace_back(this, f, pdefs[i], _global_sequences.data(), _context);
      }
      catch (std::logic_error error)
      {
        LogError << "Loading particles for '" << _file_key.stringRepr() << "' " << error.what() << std::endl;
      }      
    }
  }
  

  
  // ribbons
  if (!_uses_classic_layout && header.nRibbonEmitters)
  {
    _ribbons.reserve(header.nRibbonEmitters);
    ModelRibbonEmitterDef const* rdefs = reinterpret_cast<ModelRibbonEmitterDef const*>(f.getBuffer() + header.ofsRibbonEmitters);
    for (size_t i = 0; i<header.nRibbonEmitters; ++i) {
      _ribbons.emplace_back(this, f, rdefs[i], _global_sequences.data(), _context);
    }
  }
  else if (_uses_classic_layout && header.nRibbonEmitters
           && range_fits(f, header.ofsRibbonEmitters, header.nRibbonEmitters, sizeof(ClassicModelRibbonEmitterDef)))
  {
    // Classic 1.12 ribbon trails: mount/weapon/spell trails, phoenix tails, Kael'thas, fire sprites
    // (217 vanilla models). The WotLK path above used the 176-byte def and was gated to !classic, so
    // every vanilla trail was invisible. The 220-byte ClassicModelRibbonEmitterDef carries the same
    // fields with ClassicAnimationBlock tracks. Setup (Model::animate) and draw (ModelRender) iterate
    // _ribbons regardless of layout, so populating it here is all that's needed.
    _ribbons.reserve(header.nRibbonEmitters);
    auto const* rdefs = reinterpret_cast<ClassicModelRibbonEmitterDef const*>(f.getBuffer() + header.ofsRibbonEmitters);
    for (size_t i = 0; i < header.nRibbonEmitters; ++i) {
      _ribbons.emplace_back(this, f, rdefs[i], _global_sequences.data(), _context);
    }
  }
  

  // init lights
  if (!_uses_classic_layout && header.nLights)
  {
    _lights.reserve(header.nLights);
    ModelLightDef const* lDefs = reinterpret_cast<ModelLightDef const*>(f.getBuffer() + header.ofsLights);
    for (size_t i=0; i<header.nLights; ++i)
      _lights.emplace_back (f, lDefs[i], _global_sequences.data());
  }
  // Classic (1.12) lights were never loaded -- so torch/lava/light-ray emitters cast no light. Load
  // them here using the classic light layout (212-byte ClassicModelLightDef). Bound-checked since the
  // classic range validator doesn't cover the light block.
  else if (_uses_classic_layout && header.nLights
           && static_cast<std::uint64_t>(header.ofsLights)
                + static_cast<std::uint64_t>(header.nLights) * sizeof(ClassicModelLightDef)
              <= f.getSize())
  {
    _lights.reserve(header.nLights);
    ClassicModelLightDef const* lDefs =
      reinterpret_cast<ClassicModelLightDef const*>(f.getBuffer() + header.ofsLights);
    for (size_t i = 0; i < header.nLights; ++i)
    {
      _lights.emplace_back (f, lDefs[i], _global_sequences.data());
    }
  }

  animcalc = false;
}

void Model::calcBones(glm::mat4x4 const& model_view
                     , int _anim
                     , int time
                     , int animation_time
                     )
{
  // Derive each billboard glow card's local texture basis (normal / up / right) from its geometry + UVs,
  // once, so the screen-aligned billboard can align the card's authored texture-up with screen up (some
  // cards are authored rolled 90deg in their local plane and were rendered sideways by a fixed mapping).
  if (!_bb_bases_computed)
  {
    _bb_bases_computed = true;
    for (size_t bi = 0; bi < bones.size(); ++bi)
    {
      // Spherical (0x8) AND cylindrical (lock x/y/z) billboards both need the card's texture basis: the
      // cylindrical lock axis is the card's derived UP (the flame's vertical edge), so derive it for those too.
      if (!(bones[bi].flags.billboard
            || bones[bi].flags.cylindrical_billboard_lock_x
            || bones[bi].flags.cylindrical_billboard_lock_y
            || bones[bi].flags.cylindrical_billboard_lock_z)) continue;

      // gather this card's vertices (dominant weight on this bone)
      ModelVertex const* v0 = nullptr;
      ModelVertex const* v1 = nullptr;
      ModelVertex const* v2 = nullptr;
      for (auto const& v : _vertices)
      {
        if (v.bones[0] != static_cast<uint8_t>(bi)) continue;
        if (!v0) { v0 = &v; continue; }
        // pick two more verts that give non-degenerate UV + position triangles
        if (!v1 && (v.texcoords[0] != v0->texcoords[0])) { v1 = &v; continue; }
        if (v1 && !v2)
        {
          glm::vec2 duv1 = v1->texcoords[0] - v0->texcoords[0];
          glm::vec2 duv2 = v.texcoords[0] - v0->texcoords[0];
          float const cross_uv = duv1.x * duv2.y - duv2.x * duv1.y;
          if (std::abs(cross_uv) > 1e-8f) { v2 = &v; break; }
        }
      }
      if (!v0 || !v1 || !v2) continue;

      // _vertices positions are ALREADY fixCoordSystem'd once at load (initCommon:
      // "v.position = fixCoordSystem(v.position)"). Re-applying it here double-transformed the card basis
      // (an extra fixCoordSystem rotation), tipping bb_local_up from vertical (0,1,0) to (0,0,-1) -- a ~90deg
      // roll. That was invisible on symmetric glow halos (Anomalus) but rendered the DIRECTIONAL Karazhan
      // candle flames sideways. Use the already-fixed positions directly so the basis lives in the same
      // (single-fixed) model space as the bone matrix `mat` used below in calcMatrix.
      glm::vec3 const p0 = v0->position;
      glm::vec3 const p1 = v1->position;
      glm::vec3 const p2 = v2->position;
      glm::vec3 const e1 = p1 - p0;
      glm::vec3 const e2 = p2 - p0;
      glm::vec2 const duv1 = v1->texcoords[0] - v0->texcoords[0];
      glm::vec2 const duv2 = v2->texcoords[0] - v0->texcoords[0];

      float const det = duv1.x * duv2.y - duv2.x * duv1.y;
      if (std::abs(det) < 1e-8f) continue;
      float const r = 1.0f / det;
      // tangent = local dir of increasing U; bitangent = local dir of increasing V
      glm::vec3 const tangent   = (e1 * duv2.y - e2 * duv1.y) * r;
      glm::vec3 const bitangent = (e2 * duv1.x - e1 * duv2.x) * r;

      glm::vec3 normal = glm::cross(e1, e2);
      if (glm::length(normal) < 1e-8f || glm::length(bitangent) < 1e-8f) continue;
      normal = glm::normalize(normal);

      // Texture V increases downward in BLPs, so screen-up follows DECREASING V = -bitangent. Orthonormalize
      // against the card normal, then complete a right-handed frame.
      glm::vec3 up = -bitangent;
      up = up - normal * glm::dot(up, normal);
      if (glm::length(up) < 1e-8f) continue;
      up = glm::normalize(up);
      glm::vec3 right = glm::normalize(glm::cross(up, normal));
      // keep 'right' pointing along increasing U so the texture isn't mirrored
      if (glm::dot(right, tangent) < 0.0f) { right = -right; }

      bones[bi].bb_local_normal = normal;
      bones[bi].bb_local_up = up;
      bones[bi].bb_local_right = right;
    }
  }

  for (size_t i = 0; i<header.nBones; ++i)
  {
    bones[i].calc = false;
  }

  for (size_t i = 0; i<header.nBones; ++i)
  {
    if (capture_m2_animation_debug_enabled()
        && _file_key.filepath().find("gnomemachine") != std::string::npos)
    {
      LogDebug << "Model::calcBones bone begin model='" << _file_key.stringRepr()
               << "' bone=" << i
               << " parent=" << bones[i].parent
               << " anim=" << _anim
               << " time=" << time
               << " animtime=" << animation_time
               << std::endl;
    }

    bones[i].calcMatrix(model_view, bones.data(), _file_key.stringRepr(), i, _anim, time, animation_time);

    if (capture_m2_animation_debug_enabled()
        && _file_key.filepath().find("gnomemachine") != std::string::npos)
    {
      LogDebug << "Model::calcBones bone end model='" << _file_key.stringRepr()
               << "' bone=" << i
               << std::endl;
    }
  }

  // Weapon-grip: after the normal body pass, curl the fingers closed from the HandsClosed pose. Only the
  // finger-subtree bones are touched, each anchored to its already-posed (breathing) parent, so the body
  // animation is preserved. Enabled per-draw for creatures holding an in-hand weapon.
  if (_hand_overlay_active)
  {
    applyHandGripOverlay(time, animation_time);
  }
}

void Model::applyHandGripOverlay(int time, int animtime)
{
  if (_hand_overlay_bones.empty())
  {
    return;
  }

  // Resolve the HandsClosed (animID 15) sequence index for this model. Absent -> nothing to overlay
  // (e.g. a weapon-holding creature model that has no HandsClosed pose; it just keeps open hands).
  auto const it = _animations_seq_per_id.find(15);
  if (it == _animations_seq_per_id.end() || it->second.empty())
  {
    return;
  }
  int const seq = it->second.begin()->second.Index;

  // Re-pose each finger-subtree bone from that sequence. _hand_overlay_bones is parents-first, and finger
  // parents (the hand) are NOT in the set, so every bone here reads an already-final parent matrix.
  for (uint16_t const bi : _hand_overlay_bones)
  {
    if (bi < bones.size())
    {
      bones[bi].overrideLocalFromSeq(bones.data(), seq, time, animtime);
    }
  }
}

float Model::idlePoseFootprint()
{
  if (_idle_footprint_radius >= 0.f)
  {
    return _idle_footprint_radius;
  }
  // Default: the bind-pose footprint (correct for creatures without folding appendages).
  _idle_footprint_radius = footprint_radius;

  if (!animBones || _vertices.empty() || bones.empty())
  {
    return _idle_footprint_radius;
  }
  // Locate the stand/idle sequence (anim id 0).
  auto const id_it = _animations_seq_per_id.find(0);
  if (id_it == _animations_seq_per_id.end() || id_it->second.empty())
  {
    return _idle_footprint_radius;
  }
  int const seq = static_cast<int>(id_it->second.begin()->second.Index);

  // Pose the skeleton at idle t=0. model_view is identity: it only affects screen-aligned billboard
  // glow cards, which are a negligible fraction of the body footprint. This transiently overwrites the
  // bone matrices; the next per-frame animate() recomputes them, so it is safe.
  try
  {
    calcBones(glm::mat4x4(1.f), seq, 0, 0);
  }
  catch (...)
  {
    return _idle_footprint_radius;
  }

  // CPU-skin every render vertex with the posed bone matrices (same math the body shader does),
  // then measure the horizontal (XZ, since editor space is Y-up) footprint from its centre.
  float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
  std::vector<glm::vec2> pts;
  pts.reserve(_vertices.size());
  for (auto const& v : _vertices)
  {
    glm::vec4 p(0.f);
    float wsum = 0.f;
    for (int b = 0; b < 4; ++b)
    {
      uint8_t const w = v.weights[b];
      if (!w) continue;
      uint8_t const bi = v.bones[b];
      if (bi >= bones.size()) continue;
      p += (static_cast<float>(w) / 255.f) * (bones[bi].mat * glm::vec4(v.position, 1.f));
      wsum += static_cast<float>(w) / 255.f;
    }
    glm::vec3 const pos = (wsum > 0.01f) ? glm::vec3(p) : v.position;
    xmin = std::min(xmin, pos.x); xmax = std::max(xmax, pos.x);
    zmin = std::min(zmin, pos.z); zmax = std::max(zmax, pos.z);
    pts.emplace_back(pos.x, pos.z);
  }
  if (pts.empty())
  {
    return _idle_footprint_radius;
  }
  float const cx = 0.5f * (xmin + xmax);
  float const cz = 0.5f * (zmin + zmax);
  float r2 = 0.f;
  for (auto const& pt : pts)
  {
    float const dx = pt.x - cx, dz = pt.y - cz;
    r2 = std::max(r2, dx * dx + dz * dz);
  }
  float const r = std::sqrt(r2);
  if (r > 0.01f)
  {
    _idle_footprint_radius = r;
  }
  LogDebug << "[SELCIRCLE] model='" << _file_key.stringRepr() << "' bindFP=" << footprint_radius
           << " idleFP=" << _idle_footprint_radius << std::endl;
  return _idle_footprint_radius;
}

bool Model::advanceIdleSchedule(int anim_id, long long anim_time,
                                int& out_seq, int& out_time,
                                bool& out_do_blend, int& out_blend_seq_from, int& out_blend_time_from,
                                float& out_blend_w)
{
  auto const vit = _anim_variations.find(static_cast<uint16_t>(anim_id));
  if (vit == _anim_variations.end() || vit->second.empty())
  {
    return false;
  }
  std::vector<AnimVariation> const& vars = vit->second;

  IdleSchedule& st = _idle_schedules[_active_idle_key];

  // (Re)initialise on first use or when the played animation id changes. Seed a per-instance PRNG from
  // the instance key so different spawns follow different variation timelines (no synchronized leaning).
  if (!st.init || st.anim_id != anim_id)
  {
    std::uint64_t z = _active_idle_key * 0x9E3779B97F4A7C15ull + 0x123456789abcdefull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27));
    st.rng = static_cast<uint32_t>(z ^ (z >> 32)) | 1u; // non-zero for xorshift
    st.init = true;
    st.anim_id = anim_id;
    st.cur_seq = -1;
    st.prev_seq = -1;
    st.prev_len = 1;
    st.cur_len = 1;
    st.cur_blend = 0;
    st.play_start = anim_time;
    st.play_end = anim_time; // force an immediate roll below
  }

  auto next_rand = [&st]() -> uint32_t
  {
    uint32_t x = st.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; // xorshift32
    st.rng = x;
    return x;
  };

  uint32_t freq_sum = 0;
  for (auto const& v : vars) { freq_sum += v.frequency; }

  auto roll_variation = [&]() -> AnimVariation const&
  {
    if (vars.size() == 1 || freq_sum == 0)
    {
      return vars[next_rand() % vars.size()];
    }
    uint32_t roll = next_rand() % freq_sum;            // roulette over the frequency chain (client-exact)
    for (auto const& v : vars)
    {
      if (roll < v.frequency) { return v; }
      roll -= v.frequency;
    }
    return vars.back();
  };

  // Advance the schedule to cover anim_time. Usually 0-1 iterations; more only after the instance was
  // off-screen for a while and its clock jumped. Guard against pathological zero-length data.
  int guard = 0;
  while (anim_time >= st.play_end && guard++ < 4096)
  {
    AnimVariation const& v = roll_variation();
    uint32_t replay = v.replay_min;
    if (v.replay_max > v.replay_min)                    // replayCount in [replayMin, replayMax-1] (client)
    {
      replay = v.replay_min + (next_rand() % (v.replay_max - v.replay_min));
    }
    if (replay < 1) { replay = 1; }
    long long const dur = static_cast<long long>(std::max<uint32_t>(1, v.length)) * replay;

    st.prev_seq = st.cur_seq;
    st.prev_len = st.cur_len;
    st.cur_seq = v.seq_index;
    st.cur_len = static_cast<int>(std::max<uint32_t>(1, v.length));
    st.cur_blend = static_cast<int>(v.blend_time);
    st.play_start = st.play_end;
    st.play_end = st.play_start + dur;
  }

  out_seq = st.cur_seq;
  long long within = anim_time - st.play_start;
  if (within < 0) { within = 0; }
  out_time = static_cast<int>(within % std::max(1, st.cur_len));

  out_do_blend = false;
  // Blend ONLY across a change to a DIFFERENT sequence. When the scheduler re-rolls the SAME
  // variation (a seamless loop -- e.g. a bird/dragon wing-flap replaying), prev_seq == cur_seq and
  // the old code still cross-faded FROM that variation's FROZEN last frame INTO its restart -- which
  // made the wings hesitate/pause for the blend duration every loop ("sometimes pause mid-animation,
  // bad transition"). A seamless loop needs no blend; the track wraps cleanly on its own.
  if (st.prev_seq >= 0 && st.prev_seq != st.cur_seq && st.cur_blend > 0 && within < st.cur_blend)
  {
    out_blend_seq_from = st.prev_seq;
    out_blend_time_from = std::max(0, st.prev_len - 1); // previous variation sampled at its end
    float const x = static_cast<float>(within) / static_cast<float>(st.cur_blend);
    out_blend_w = x * x * (3.0f - 2.0f * x);            // smoothstep, matches the client
    out_do_blend = true;
  }
  return true;
}

void Model::animate(glm::mat4x4 const& model_view, int anim_id, int anim_time)
{
  if (!_logged_animation_branch && classic_m2_debug_enabled())
  {
    _logged_animation_branch = true;
    LogDebug << "M2 animate branch model='" << _file_key.stringRepr()
             << "' branch=" << (_uses_classic_layout ? "classic" : "wotlk")
             << " animId=" << anim_id
             << " animated=" << animated
             << " animBones=" << animBones
             << " animGeometry=" << animGeometry
             << " animTextures=" << animTextures
             << " classicStaticBones=" << _classic_static_bones.size()
             << " seqBuckets=" << _animations_seq_per_id.size()
             << std::endl;
  }

  if (_uses_classic_layout)
  {
    if (_animations_seq_per_id.empty() || _animations_seq_per_id[anim_id].empty())
    {
      _current_anim_seq = 0;
      _anim_time = 0;
      _global_animtime = anim_time;
      calcClassicStaticBones(model_view);
      _renderer.updateBoneMatrices();
      for (auto& particle : _particles)
      {
        particle.setup(_current_anim_seq, _anim_time, _global_animtime);
      }
      // Texture animations (lava falls, scrolling streams, fire) are usually driven by
      // global sequences and must keep advancing even though this classic model has no
      // skeletal animation sequences. Without this loop they stay frozen.
      for (auto& tex_anim : _texture_animations)
      {
        tex_anim.calc(_current_anim_seq, _anim_time, _global_animtime);
      }
      return;
    }
  }
  else if (_animations_seq_per_id.empty() || _animations_seq_per_id[anim_id].empty())
  {
    return;
  }

  int tmax = _animation_length[anim_id];
  if (tmax <= 0)
  {
    tmax = 1;
  }
  int t = anim_time % tmax;

  // [TMAXDBG] one-time: is the anomalus stand animation long enough for its particle enabled tracks
  // (timestamps up to ~53s) to be reached by the per-animation looped time?
  {
    static int tmaxdbg = 0;
    if (tmaxdbg < 1 && _file_key.hasFilepath()
        && _file_key.filepath().find("anomalus") != std::string::npos)
    {
      ++tmaxdbg;
      LogDebug << "[TMAXDBG] anomalus anim_id=" << anim_id << " tmax=" << tmax
               << " anim_time=" << anim_time << " t=" << t
               << " seq_buckets=" << _animations_seq_per_id[anim_id].size() << std::endl;
      for (auto const& id_kv : _animations_seq_per_id)
      {
        std::ostringstream seqs;
        for (auto const& sub : id_kv.second)
        {
          seqs << "sub" << sub.first << "->seq" << sub.second.Index << "(len" << sub.second.length << "),";
        }
        LogDebug << "[ANIMMAP] anomalus anim_id=" << id_kv.first << " : " << seqs.str() << std::endl;
      }
    }
  }
  int current_sub_anim = 0;
  int time_for_anim = t;
  bool do_blend = false;
  int blend_seq_from = 0;
  int blend_time_from = 0;
  float blend_w = 1.0f; // weight of the TO (current) pose

  auto const& subs = _animations_seq_per_id[anim_id];

  // IDLE-VARIATION SCHEDULER (client-accurate; see Model::advanceIdleSchedule). Replaces the old fixed
  // sequential concatenation of sub-variations. The client rolls a frequency-weighted variation, plays it
  // for length*replayCount, then re-rolls -- so a unit stands mostly neutral and occasionally shifts into
  // a longer-held pose (e.g. HumanMale Stand sub1 "lean", replay 2-7 => held ~5-19s), cross-fading over the
  // authored blendTime (500ms for Stand). Per-instance state keeps spawns desynced.
  int sched_seq = 0;
  int sched_time = 0;
  if (_uses_classic_layout && animBones && anim_transition_blend_enabled()
      && advanceIdleSchedule(anim_id, static_cast<long long>(anim_time),
                             sched_seq, sched_time, do_blend, blend_seq_from, blend_time_from, blend_w))
  {
    _current_anim_seq = sched_seq;
    _anim_time = sched_time;
  }
  else
  {
    // Legacy path (non-classic model, blending disabled, or no variation data): sub-variations
    // concatenated on one timeline.
    for (auto const& sub_animation : subs)
    {
      if (static_cast<int>(sub_animation.second.length) > time_for_anim)
      {
        current_sub_anim = sub_animation.first;
        break;
      }
      time_for_anim -= sub_animation.second.length;
    }
    ModelAnimation const& a = _animations_seq_per_id[anim_id][current_sub_anim];
    _current_anim_seq = a.Index;
    _anim_time = _uses_classic_layout ? time_for_anim : t;
  }

  _global_animtime = anim_time;

  if (animBones)
  {
    if (do_blend)
    {
      // FROM pose (previous sequence at its end) -> snapshot -> TO pose (current) -> lerp in place.
      calcBones(model_view, blend_seq_from, blend_time_from, _global_animtime);
      if (_blend_scratch.size() != bones.size())
      {
        _blend_scratch.resize(bones.size());
      }
      for (std::size_t i = 0; i < bones.size(); ++i)
      {
        _blend_scratch[i] = bones[i].mat;
      }
      calcBones(model_view, _current_anim_seq, _anim_time, _global_animtime);
      float const w_to = blend_w;
      float const w_from = 1.0f - blend_w;
      for (std::size_t i = 0; i < bones.size(); ++i)
      {
        bones[i].mat = _blend_scratch[i] * w_from + bones[i].mat * w_to;
      }
    }
    else
    {
      calcBones(model_view, _current_anim_seq, _anim_time, _global_animtime);
    }
  }

  if (animGeometry || animBones)
  {
    if (capture_m2_animation_debug_enabled()
        && _file_key.filepath().find("gnomemachine") != std::string::npos)
    {
      LogDebug << "Model::animate bone matrix copy begin model='" << _file_key.stringRepr()
               << "' bones=" << bones.size()
               << " matrices=" << bone_matrices.size()
               << std::endl;
    }

    std::size_t bone_counter = 0;
    for (auto& bone : bones)
    {
    	bone_matrices[bone_counter] = bone.mat;
      bone_counter++;
    }

    if (capture_m2_animation_debug_enabled()
        && _file_key.filepath().find("gnomemachine") != std::string::npos)
    {
      LogDebug << "Model::animate bone matrix copy end model='" << _file_key.stringRepr()
               << "' copied=" << bone_counter
               << std::endl;
      LogDebug << "Model::animate bone matrix upload begin model='" << _file_key.stringRepr()
               << "'" << std::endl;
    }

    _renderer.updateBoneMatrices();

    if (capture_m2_animation_debug_enabled()
        && _file_key.filepath().find("gnomemachine") != std::string::npos)
    {
      LogDebug << "Model::animate bone matrix upload end model='" << _file_key.stringRepr()
               << "'" << std::endl;
    }


    // transform vertices

    /*
    _current_vertices = _vertices;

    for (auto& vertex : _current_vertices)
    {
      ::glm::vec3 v(0, 0, 0), n(0, 0, 0);

      for (size_t b (0); b < 4; ++b)
      {
        if (vertex.weights[b] <= 0)
          continue;

        ::glm::vec3 tv = bones[vertex.bones[b]].mat * vertex.position;
        ::glm::vec3 tn = bones[vertex.bones[b]].mrot * vertex.normal;

        v += tv * (static_cast<float> (vertex.weights[b]) / 255.0f);
        n += tn * (static_cast<float> (vertex.weights[b]) / 255.0f);
      }

      vertex.position = v;
      vertex.normal = n.normalized();
    }

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_vertices_buffer);
    gl.bufferData (GL_ARRAY_BUFFER, _current_vertices.size() * sizeof (ModelVertex), _current_vertices.data(), GL_STREAM_DRAW);

     */
  }

  for (size_t i = 0; i < _lights.size(); ++i) 
  {
    if (_lights[i].parent >= 0
        && static_cast<std::size_t>(_lights[i].parent) < bones.size()) 
    {
        _lights[i].tpos = bones[_lights[i].parent].mat * glm::vec4(_lights[i].pos,0);
      _lights[i].tdir = bones[_lights[i].parent].mrot * glm::vec4(_lights[i].dir,0);
    }
  }

  for (auto& particle : _particles)
  {
    // Sample emitter tracks at the model's real animation time -- NO random phase offset. The old
    // WMV `tmax*tofs` shift desynchronized burst-pattern emitters: Ironforge's ForgeLava steam
    // authors its emission rate as bellows-synced BURSTS (rate keys 0..20..0 over the 6667ms anim);
    // the client fires every stacked emitter in sync (puff / gap / puff), but random phases spread
    // the bursts across time so their additive quads overlapped CONTINUOUSLY into a blown-out
    // white core at the plume center.
    particle.setup(_current_anim_seq, t, _global_animtime);
  }

  for (std::size_t i = 0; i < _ribbons.size(); ++i)
  {
    _ribbons[i].setup(_current_anim_seq, t, _global_animtime);
  }

  for (auto& tex_anim : _texture_animations)
  {
    // global-sequence texture anims (lava falls etc.) advance on the continuous global
    // clock, not the model's looped animation time (which getValue ignores for them).
    tex_anim.calc(_current_anim_seq, t, _global_animtime);
  }
}

void TextureAnim::calc(int anim, int time, int animtime)
{
    mat = glm::mat4x4(1);
  if (trans.uses(anim))
  {
      mat = glm::translate(mat, trans.getValue(anim, time, animtime));
  }

  // Rotation and scaling pivot about the tile CENTER (0.5, 0.5) -- the client convention. Without
  // the pivot they orbit/stretch from the UV corner, so rotating portal swirls slid around the
  // origin instead of spinning in place. Pure translation scrolls (lava falls, waterfalls) never
  // enter these branches and are byte-identical to before.
  bool const has_rot = _uses_classic_rotation ? classic_rot.uses(anim) : rot.uses(anim);
  bool const has_scale = scale.uses(anim);
  if (has_rot || has_scale)
  {
      mat = glm::translate(mat, glm::vec3(0.5f, 0.5f, 0.0f));
      if (has_rot)
      {
          glm::quat const q = _uses_classic_rotation ? classic_rot.getValue(anim, time, animtime)
                                                     : rot.getValue(anim, time, animtime);
          mat *= glm::toMat4(q);
      }
      if (has_scale)
      {
          mat = glm::scale(mat, scale.getValue(anim, time, animtime));
      }
      mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, 0.0f));
  }
}

ModelColor::ModelColor(const BlizzardArchive::ClientFile& f, const ModelColorDef &mcd, int *global)
  : color (mcd.color, f, global)
  , opacity(mcd.opacity, f, global)
{}

// Classic (1.12) color: same tracks in the older ClassicAnimationBlock form.
ModelColor::ModelColor(const BlizzardArchive::ClientFile& f, const ClassicModelColorDef &mcd, int *global)
  : color (mcd.color, f, global)
  , opacity(mcd.opacity, f, global)
{}

ModelTransparency::ModelTransparency(const BlizzardArchive::ClientFile& f, const ModelTransDef &mcd, int *global)
  : trans (mcd.trans, f, global)
{}

ModelTransparency::ModelTransparency(const BlizzardArchive::ClientFile& f, const ClassicModelTransDef &mcd, int *global)
  : trans (mcd.trans, f, global)
{}

ModelLight::ModelLight(const BlizzardArchive::ClientFile& f, const ModelLightDef &mld, int *global)
  : type (mld.type)
  , parent (mld.bone)
  , pos (fixCoordSystem(mld.pos))
  , tpos (fixCoordSystem(mld.pos))
  , dir (::glm::vec3(0,1,0))
  , tdir (::glm::vec3(0,1,0)) // obviously wrong
  , diffColor (mld.color, f, global)
  , ambColor (mld.ambColor, f, global)
  , diffIntensity (mld.intensity, f, global)
  , ambIntensity (mld.ambIntensity, f, global)
  , attStart (mld.attStart, f, global)
  , attEnd (mld.attEnd, f, global)
{}

// Classic (1.12) light: same leading fields, animated tracks in the older ClassicAnimationBlock form.
ModelLight::ModelLight(const BlizzardArchive::ClientFile& f, const ClassicModelLightDef &mld, int *global)
  : type (mld.type)
  , parent (mld.bone)
  , pos (fixCoordSystem(mld.pos))
  , tpos (fixCoordSystem(mld.pos))
  , dir (::glm::vec3(0,1,0))
  , tdir (::glm::vec3(0,1,0))
  , diffColor (mld.color, f, global)
  , ambColor (mld.ambColor, f, global)
  , diffIntensity (mld.intensity, f, global)
  , ambIntensity (mld.ambIntensity, f, global)
  , attStart (mld.attStart, f, global)
  , attEnd (mld.attEnd, f, global)
{}

void ModelLight::setup(int time, OpenGL::light, int animtime)
{
	auto ambient = ambColor.getValue(0, time, animtime) * ambIntensity.getValue(0, time, animtime);
    auto diffuse = diffColor.getValue(0, time, animtime) * diffIntensity.getValue(0, time, animtime);

  glm::vec4 ambcol(ambient.x, ambient.y,ambient.z, 1.0f);
  glm::vec4 diffcol(diffuse.x, diffuse.y, diffuse.z, 1.0f);
  glm::vec4 p;

  enum ModelLightTypes {
    MODELLIGHT_DIRECTIONAL = 0,
    MODELLIGHT_POINT
  };

  if (type == MODELLIGHT_DIRECTIONAL) {
    // directional
    p = glm::vec4(tdir.x, tdir.y, tdir.z, 0.0f);
  }
  else if (type == MODELLIGHT_POINT) {
    // point
    p = glm::vec4(tpos.x, tpos.y,tpos.z, 1.0f);
  }
  else {
    p = glm::vec4(tpos.x, tpos.y, tpos.z, 1.0f);
    LogError << "Light type " << type << " is unknown." << std::endl;
  }
 
  // todo: use models' light
}

TextureAnim::TextureAnim (const BlizzardArchive::ClientFile& f, const ModelTexAnimDef &mta, int *global)
  : trans (mta.trans, f, global)
  , rot (mta.rot, f, global)
  , classic_rot ()
  , scale (mta.scale, f, global)
  , _uses_classic_rotation (false)
  , mat (glm::mat4x4())
{}

TextureAnim::TextureAnim (const BlizzardArchive::ClientFile& f, const ClassicModelTexAnimDef &mta, int *global)
  : trans (mta.trans, f, global)
  , rot ()
  // Vanilla rotation keys are float quaternions (like classic bones), NOT WotLK packed int16 --
  // reading them through the packed converter produced garbage tumbling rotations. UV-space
  // rotation needs NO fixCoordSystemQuat (that conversion is for model space).
  , classic_rot (mta.rot, f, global)
  , scale (mta.scale, f, global)
  , _uses_classic_rotation (true)
  , mat (glm::mat4x4())
{}

Bone::Bone( const BlizzardArchive::ClientFile& f,
            const ModelBoneDef &b,
            int *global,
            const std::vector<std::unique_ptr<BlizzardArchive::ClientFile>>& animation_files)
  : trans (b.translation, f, global, animation_files)
  , rot (b.rotation, f, global, animation_files)
  , classic_rot()
  , scale (b.scaling, f, global, animation_files)
  , _uses_classic_rotation(false)
  , pivot (fixCoordSystem (b.pivot))
  , parent (b.parent)
{
  memcpy(&flags, &b.flags, sizeof(uint32_t));

  trans.apply(fixCoordSystem);
  rot.apply(fixCoordSystemQuat);
  scale.apply(fixCoordSystem2);
}

Bone::Bone( const BlizzardArchive::ClientFile& f,
            const ClassicModelBoneDef &b,
            int *global)
  : trans (b.translation, f, global)
  , rot()
  , classic_rot (b.rotation, f, global)
  , scale (b.scaling, f, global)
  , _uses_classic_rotation(true)
  , pivot (fixCoordSystem (b.pivot))
  , parent (b.parent)
{
  memcpy(&flags, &b.flags, sizeof(uint32_t));

  trans.apply(fixCoordSystem);
  classic_rot.apply(fixCoordSystemQuat);
  scale.apply(fixCoordSystem2);
}

void Bone::calcMatrix(glm::mat4x4 const& model_view
                     , Bone *allbones
                     , std::string const& model_name
                     , size_t bone_index
                     , int anim
                     , int time
                     , int animtime
                     )
{

  if (calc) return;

  glm::mat4x4 m = glm::mat4x4(1);
  glm::mat4x4 mr = glm::mat4x4(1);
  bool const has_rotation = _uses_classic_rotation ? classic_rot.uses(anim) : rot.uses(anim);

  if ( flags.transformed
    || flags.billboard
    || flags.cylindrical_billboard_lock_x 
    || flags.cylindrical_billboard_lock_y 
    || flags.cylindrical_billboard_lock_z
      )
  {
  	m = glm::translate(m, pivot);


    if (trans.uses(anim))
    {
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix translation begin model='" << model_name
                 << "' bone=" << bone_index << std::endl;
      }
      m = glm::translate(m, trans.getValue (anim, time, animtime));
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix translation end model='" << model_name
                 << "' bone=" << bone_index << std::endl;
      }
    }

    if (has_rotation)
    {
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix rotation begin model='" << model_name
                 << "' bone=" << bone_index
                 << " classic=" << _uses_classic_rotation
                 << std::endl;
      }
      glm::quat ref = glm::quat_cast(glm::mat4x4(1));
      glm::quat q = _uses_classic_rotation ? classic_rot.getValue(anim, time, animtime) : rot.getValue(anim, time, animtime);
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix rotation value model='" << model_name
                 << "' bone=" << bone_index
                 << " quat=(" << q.w << "," << q.x << "," << q.y << "," << q.z << ")"
                 << std::endl;
      }
      glm::vec3 rot_euler = glm::eulerAngles(q);

      glm::vec3 test_rot_vec = glm::vec3(rot_euler[2],
        -(rot_euler[1] + glm::radians(180.f)),
        -(rot_euler[0] + glm::radians(180.f)));

      mr = glm::eulerAngleXYZ(test_rot_vec.x, test_rot_vec.y, test_rot_vec.z);

      m = m * mr;
    }

    if (scale.uses(anim))
    {
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix scale begin model='" << model_name
                 << "' bone=" << bone_index << std::endl;
      }
      m = glm::scale(m, scale.getValue (anim, time, animtime));
      if (capture_m2_animation_debug_enabled()
          && model_name.find("gnomemachine") != std::string::npos)
      {
        LogDebug << "Bone::calcMatrix scale end model='" << model_name
                 << "' bone=" << bone_index << std::endl;
      }
    }

    m = glm::translate(m, -pivot);
  }

  if (parent >= 0)
  {
    allbones[parent].calcMatrix (model_view, allbones, model_name, static_cast<size_t>(parent), anim, time, animtime);
    mat = allbones[parent].mat * m;
  }
  else
  {
    mat = m;
  }

  // Spherical billboard: applied to the FINAL (model-space) matrix, AFTER the parent hierarchy -- so the
  // card faces the camera no matter how the animated parent bone is oriented. The old code baked the
  // billboard into the LOCAL matrix before the parent multiply, so a moving parent bone (e.g. Anomalus's
  // arm) rotated the Purple_Glow glow card away from the viewer instead of keeping it camera-facing.
  // We keep the pivot wherever the hierarchy places it (so the card still follows the arm) and only
  // overwrite the ORIENTATION with the camera basis. Same vRight/vUp convention as before, so bones whose
  // parent doesn't rotate render identically to the previous behaviour (mathematically equivalent when the
  // parent rotation is identity).
  if (flags.billboard)
  {
    // Screen-aligned spherical billboard on the FINAL model-space matrix -- the card stays in the camera
    // plane (never inheriting the animated arm bone's rotation, which would swing the far-offset card
    // around), while its pivot follows the hierarchy so it stays glued to the body. model_view is the full
    // model->view matrix (camera view * this spawn's world transform), so the camera axes expressed in this
    // instance's MODEL space are the ROWS of its 3x3. Card lies in the bone's local Y/Z plane (normal =
    // local X): map local X -> view axis, local Y -> screen right, local Z -> screen up.
    glm::vec3 const camRight = glm::normalize(glm::vec3(model_view[0][0], model_view[1][0], model_view[2][0]));
    glm::vec3 const camUp    = glm::normalize(glm::vec3(model_view[0][1], model_view[1][1], model_view[2][1]));
    glm::vec3 const camFwd   = glm::normalize(glm::vec3(model_view[0][2], model_view[1][2], model_view[2][2]));

    glm::vec4 const world_pivot = mat * glm::vec4(pivot, 1.0f);

    // Map the card's OWN texture basis to the screen: local normal -> view axis, texture-right -> screen
    // right, texture-up -> screen up. bb = Camera * transpose(Local) since the local basis is orthonormal.
    glm::mat3 const cam(camFwd, camRight, camUp);                                   // columns
    // Both the chandelier flame and Anomalus's energy glow are SPHERICAL billboards, yet they need
    // OPPOSITE basis conventions -- so any single global choice makes one upright and the other sideways
    // (the whack-a-mole). Cause: the card "up" is derived from texture-V, but Anomalus's glow authors the
    // texture rotated 90deg (V horizontal / U vertical) while the chandelier's is upright (V vertical).
    // Decide per-card from the geometry: if the derived up already points world-up (chandelier) keep it;
    // if it came out horizontal (Anomalus) re-apply fixCoordSystem, a det-+1 rotation that swings that
    // horizontal up back to vertical -- i.e. each card gets the exact basis it was confirmed upright with.
    bool const refix = std::abs(bb_local_up.y) < std::abs(bb_local_right.y);
    glm::mat3 const local(refix ? fixCoordSystem(bb_local_normal) : bb_local_normal,
                          refix ? fixCoordSystem(bb_local_right)  : bb_local_right,
                          refix ? fixCoordSystem(bb_local_up)     : bb_local_up);   // columns
    glm::mat3 const bb3 = cam * glm::transpose(local);

    // PRESERVE the animated bone SCALE: `mat` (about to be replaced) carries the hierarchy's scale --
    // e.g. the candle glow cards pulse via a bone scale track on a global sequence, which the pure
    // rotation basis was silently discarding (glow stopped breathing). Per-axis scale = column lengths;
    // reapplied pivot-centred so the card grows/shrinks in place.
    glm::vec3 const bone_scale(glm::length(glm::vec3(mat[0])),
                               glm::length(glm::vec3(mat[1])),
                               glm::length(glm::vec3(mat[2])));

    glm::mat4x4 bb(1.0f);
    bb[0] = glm::vec4(bb3[0] * bone_scale.x, 0.0f);
    bb[1] = glm::vec4(bb3[1] * bone_scale.y, 0.0f);
    bb[2] = glm::vec4(bb3[2] * bone_scale.z, 0.0f);
    glm::vec4 const rotated_pivot = bb * glm::vec4(pivot, 1.0f);
    bb[3] = glm::vec4(glm::vec3(world_pivot) - glm::vec3(rotated_pivot), 1.0f);
    mat = bb;
  }
  else if (flags.cylindrical_billboard_lock_x
        || flags.cylindrical_billboard_lock_y
        || flags.cylindrical_billboard_lock_z)
  {
    // Cylindrical lock-Z geometry in 1.12 is thin VERTICAL structural mesh -- the LavaPots forge CHAINS
    // (MELTINGPOTCHAIN) and candle threads -- NOT flames (flames are SPHERICAL 0x8, handled above; noggit's
    // old comment claiming torches/candles are lock-Z was wrong). In-game these read as STATIC: the client's
    // cylindrical spin around vertical is imperceptible on them, whereas ANY noggit billboard -- the old
    // flame-card-basis remap OR a clean bone-axis spin -- visibly swung the multi-quad chain to face the
    // camera (reported: forge chains "rotate and change orientation"). User-confirmed that rendering them in
    // their authored REST pose matches in-game exactly. So do not billboard cylindrical-lock bones; `mat`
    // already holds the rest pose here.
    (void)model_view;
  }

  // transform matrix for normal vectors ... ??
  if (has_rotation)
  {
    if (parent >= 0)
    {
        mrot = allbones[parent].mrot * mr;
    }
    else
    {
      mrot = mr;
    }
  }
  else
  {
    mrot = glm::mat4x4(1);
  }

  calc = true;
}

void Bone::overrideLocalFromSeq(Bone* allbones, int seq, int time, int animtime)
{
  // Rebuild this bone's LOCAL transform from `seq` (HandsClosed), identical to calcMatrix's local block,
  // then compose against the parent's ALREADY-final matrix. No recursion, no calc flag: the parent (the
  // Stand-posed hand) is untouched, so only this finger bone moves.
  glm::mat4x4 m = glm::mat4x4(1);
  if ( flags.transformed
    || flags.billboard
    || flags.cylindrical_billboard_lock_x
    || flags.cylindrical_billboard_lock_y
    || flags.cylindrical_billboard_lock_z
     )
  {
    m = glm::translate(m, pivot);

    if (trans.uses(seq))
    {
      m = glm::translate(m, trans.getValue(seq, time, animtime));
    }

    bool const has_rotation = _uses_classic_rotation ? classic_rot.uses(seq) : rot.uses(seq);
    if (has_rotation)
    {
      glm::quat q = _uses_classic_rotation ? classic_rot.getValue(seq, time, animtime)
                                           : rot.getValue(seq, time, animtime);
      glm::vec3 rot_euler = glm::eulerAngles(q);
      glm::vec3 test_rot_vec = glm::vec3(rot_euler[2],
        -(rot_euler[1] + glm::radians(180.f)),
        -(rot_euler[0] + glm::radians(180.f)));
      m = m * glm::eulerAngleXYZ(test_rot_vec.x, test_rot_vec.y, test_rot_vec.z);
    }

    if (scale.uses(seq))
    {
      m = glm::scale(m, scale.getValue(seq, time, animtime));
    }

    m = glm::translate(m, -pivot);
  }

  mat = (parent >= 0) ? allbones[parent].mat * m : m;
}


std::vector<std::pair<float, std::tuple<int, int, int>>> Model::intersect (glm::mat4x4 const& model_view, math::ray const& ray, int animtime)
{
  std::vector<std::pair<float, std::tuple<int, int, int>>> results;

  if (!finishedLoading() || loading_failed())
  {
    return results;
  }

  if (animated && (!animcalc || _per_instance_animation))
  {
    animate (model_view, 0, animtime);
    animcalc = true;
  }

  if (use_fake_geometry())
  {
    auto& fake_geom = _fake_geometry.value();

    for (size_t i = 0; i < fake_geom.indices.size(); i += 3)
    {
      if (auto distance
        = ray.intersect_triangle(fake_geom.vertices[fake_geom.indices[i + 0]],
          fake_geom.vertices[fake_geom.indices[i + 1]],
          fake_geom.vertices[fake_geom.indices[i + 2]])
        )
      {
        results.emplace_back (*distance, std::make_tuple(static_cast<int>(i), static_cast<int>(i+1), 1+2));
        return results;
      }
    }

    return results;
  }

  for (auto const& pass : _renderer.renderPasses())
  {
    for (int i (pass.index_start); i < pass.index_start + pass.index_count; i += 3)
    {
      if ( auto distance
          = ray.intersect_triangle( _vertices[_indices[static_cast<std::size_t>(i + 0)]].position,
                                    _vertices[_indices[static_cast<std::size_t>(i + 1)]].position,
                                    _vertices[_indices[static_cast<std::size_t>(i + 2)]].position)
          )
      {
        results.emplace_back (*distance, std::make_tuple(i, i + 1, 1 + 2));
      }
    }
  }

  return results;
}

void Model::lightsOn(OpenGL::light lbase)
{
  // setup lights
  for (unsigned int i=0, l=lbase; i<header.nLights; ++i) _lights[i].setup(_anim_time, l++, _global_animtime);
}

void Model::lightsOff(OpenGL::light lbase)
{
  for (unsigned int i = 0, l = lbase; i<header.nLights; ++i) gl.disable(l++);
}


void Model::swapInstanceEmitterState(std::uint64_t instance_key)
{
  if (_particles.empty())
  {
    return;
  }

  auto& states = _instance_emitter_states[instance_key];
  if (states.size() != _particles.size())
  {
    // First time this spawn is seen: default (empty, not-prewarmed) states. Swapping these in and running
    // update() will pre-warm them to steady state, so the spawn's particles appear populated immediately.
    states.assign(_particles.size(), ParticleSystemLiveState{});
  }

  for (std::size_t i = 0; i < _particles.size(); ++i)
  {
    _particles[i].swapLiveState(states[i]);
  }
}

void Model::updateParticleSystems(float dt)
{
  if (!finished)
  {
    return;
  }
  for (auto& particle : _particles)
  {
    particle.update(dt);
  }
}

void Model::dropInstanceEmitterState(std::uint64_t instance_key)
{
  _instance_emitter_states.erase(instance_key);
}

bool Model::particlesRideParent() const
{
  for (auto const& particle : _particles)
  {
    if (particle.emitterFlags() & 0x10)
    {
      return true;
    }
  }
  return false;
}

void Model::updateEmitters(float dt)
{
  if (finished)
  {
    for (auto& particle : _particles)
    {
      particle.update (dt);
    }

    // Texture animations (lava falls, scrolling streams) are global-sequence driven and
    // must advance every frame. Some classic models (e.g. blackrocklavafalls) are never
    // added to the mesh draw bucket, so Model::animate() never runs for them and their
    // texture-anim matrices stay frozen. updateEmitters runs for every loaded model, so
    // drive the texture animations here on a continuous accumulated clock.
    if (!_texture_animations.empty())
    {
      _emitter_anim_accum_ms += dt * 1000.0f;

      // Feed the same continuous clock as BOTH the local animation time and the global-sequence
      // time. Classic texanims split two ways (verified by parsing the assets against Noggit's
      // ClassicModelHeader offsets):
      //   seq == -1  -> indexed by the track's OWN timestamps (AnimatedValue::getValue does
      //                 time %= max_time). The lava falls (BlackRockLavaFalls01: 6 tracks, 3333ms
      //                 loop, ~0.3 UV/s) and waterfalls (ElwynnTallWaterfall01: 2 tracks, 10000ms)
      //                 are all seq == -1 -- they were frozen only because _anim_time stayed 0.
      //   seq >=  0  -> indexed by the global-sequence duration (animtime % globalSequences[id]).
      //                 The light-ray dust (Lightray_Dusty_01: 1 track, global seq 0 = 133333ms,
      //                 0 -> -5.107 V = ~0.038 UV/s) rides this.
      // The authored track timestamps / global-sequence durations are the real scroll speed -- no
      // path-scoped clock scaling. (Previously: 2x for volumetriclight, which was synthetic.)
      int const clock = static_cast<int>(_emitter_anim_accum_ms);
      for (auto& tex_anim : _texture_animations)
      {
        tex_anim.calc(_current_anim_seq, clock, clock);
      }
    }
  }
}
