// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/bounding_box.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/Log.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/World.h>
#include <opengl/scoped.hpp>
#include <opengl/shader.hpp>
#include <external/tracy/Tracy.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <util/CurrentFunction.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <math/trig.hpp>

namespace
{
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
}

Model::Model(const std::string& filename, Noggit::NoggitRenderContext context)
  : AsyncObject(filename)
  , _context(context)
  , _renderer(this)
{
  memset(&header, 0, sizeof(ModelHeader));
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

    if (std::memcmp(translated_header.id, "MD20", 4) == 0
        && m2_version(translated_header.version) <= 256
        && range_fits(f, classic_header.ofsViews, classic_header.nViews, sizeof(ClassicModelView))
        && m2_static_ranges_fit(f, translated_header))
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
    animated = has_classic_runtime_bones;
    animGeometry = has_classic_runtime_bones;
    animBones = has_classic_runtime_bones;
    animTextures = false;
    _per_instance_animation = has_classic_runtime_bones;
  }
  else
  {
    animated = isAnimated(f);  // isAnimated will set animGeometry and animTextures
  }

  trans = 1.0f;
  _current_anim_seq = 0;

  rad = header.bounding_box_radius;

  if (!_uses_classic_layout && header.nGlobalSequences)
  {
    _global_sequences = M2Array<int>(f, header.ofsGlobalSequences, header.nGlobalSequences);
  }

  //! \todo  This takes a biiiiiit long. Have a look at this.
  initCommon(f);

  if (animated && !_uses_classic_layout)
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

  if (!_uses_classic_layout || !header.nBones || !range_fits(f, header.ofsBones, header.nBones, sizeof(ModelBoneDef)))
  {
    return false;
  }

  bool has_runtime_bone = false;
  bool has_weighted_vertices = false;
  auto const* model_bones = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);
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

  for (std::size_t i = 0; i < _classic_static_bones.size(); ++i)
  {
    auto const& bone = _classic_static_bones[i];
    glm::mat4x4 matrix = classic_bone_local_matrix(bone.flags, bone.pivot, model_view);

    if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < i)
    {
      matrix = bone_matrices[static_cast<std::size_t>(bone.parent)] * matrix;
    }

    bone_matrices[i] = matrix;
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
  // see if we have any animated bones
  ModelBoneDef const* bo = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);

  animGeometry = false;
  animBones = false;
  _per_instance_animation = false;

  ModelVertex const* verts = reinterpret_cast<ModelVertex const*>(f.getBuffer() + header.ofsVertices);
  for (size_t i = 0; i<header.nVertices && !animGeometry; ++i) 
  {
    for (size_t b = 0; b<4; b++) 
    {
      if (verts[i].weights[b]>0) 
      {
        ModelBoneDef const& bb = bo[verts[i].bones[b]];
        bool billboard = (bb.flags & (0x78)); // billboard | billboard_lock_[xyz]

        if ((bb.flags & 0x200) || billboard) 
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
    for (size_t i = 0; i<header.nBones; ++i)
    {
      ModelBoneDef const& bb = bo[i];
      if (bb.translation.type || bb.rotation.type || bb.scaling.type)
      {
        animBones = true;
        break;
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
    ModelTransDef const* trs = reinterpret_cast<ModelTransDef const*>(f.getBuffer() + header.ofsTransparency);
    for (size_t i = 0; i<header.nTransparency; ++i)
    {
      if (trs[i].trans.type != 0)
      {
        return true;
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

  for (auto& v : _vertices)
  {
    v.position = fixCoordSystem(v.position);
    v.normal = fixCoordSystem(v.normal);
  }

  // textures
  ModelTextureDef const* texdef = reinterpret_cast<ModelTextureDef const*>(f.getBuffer() + header.ofsTextures);
  _textureFilenames.resize(header.nTextures);
  _specialTextures.resize(header.nTextures);
  int classic_missing_texture_fallbacks = 0;

  for (size_t i = 0; i < header.nTextures; ++i)
  {
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

      if (_uses_classic_layout
          && !Noggit::Application::NoggitApplication::instance()->clientData()->exists(_textureFilenames[i]))
      {
        _textureFilenames[i] = "tileset/generic/black.blp";
        classic_missing_texture_fallbacks++;
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
      // anything else.  If no override is registered the fallback is still
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

  if (_uses_classic_layout && classic_missing_texture_fallbacks > 0)
  {
    LogError << "Classic M2 texture fallback " << classic_missing_texture_fallbacks
             << " texture(s) for " << _file_key.stringRepr() << std::endl;
  }

  // init colors
  if (!_uses_classic_layout && header.nColors)
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

  if (_uses_classic_layout && header.nTransparency)
  {
    _classic_transparency_values.reserve(header.nTransparency);
    ModelTransDef const* trDefs = reinterpret_cast<ModelTransDef const*>(f.getBuffer() + header.ofsTransparency);
    Animation::Conversion<int16_t, float> convert_alpha;

    for (size_t i = 0; i < header.nTransparency; ++i)
    {
      float alpha = 1.0f;

      if (trDefs[i].trans.nKeys && range_fits(f, trDefs[i].trans.ofsKeys, trDefs[i].trans.nKeys, sizeof(int16_t)))
      {
        int16_t const* keys = reinterpret_cast<int16_t const*>(f.getBuffer() + trDefs[i].trans.ofsKeys);
        alpha = 0.0f;
        for (size_t key_index = 0; key_index < trDefs[i].trans.nKeys; ++key_index)
        {
          alpha = std::max(alpha, convert_alpha(keys[key_index]));
        }
      }

      _classic_transparency_values.push_back(alpha);
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

        if (!classic_view || candidate->n_triangle > classic_view->n_triangle)
        {
          classic_view = candidate;
        }
      }

      if (!classic_view)
      {
        LogError << "No valid embedded skin view for '" << _file_key.stringRepr() << "'" << std::endl;
        return;
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

        for (size_t geoset_index = 0; geoset_index < view->n_submesh; ++geoset_index)
        {
          auto const& geoset = model_geosets[geoset_index];
          size_t const vertex_end = std::min<size_t>(view->n_index, static_cast<size_t>(geoset.vstart) + geoset.vcount);
          uint16_t const influences = std::min<uint16_t>(4, geoset.d5);

          for (size_t vertex = geoset.vstart; vertex < vertex_end; ++vertex)
          {
            for (uint16_t bone = 0; bone < influences; ++bone)
            {
              uint16_t const bone_lookup_index = static_cast<uint16_t>(geoset.d4 + vertex_properties[vertex * 4 + bone]);
              if (bone_lookup_index < header.nBoneLookup && bone_lookup[bone_lookup_index] < header.nBones)
              {
                _vertices[vertex].bones[bone] = static_cast<uint8_t>(bone_lookup[bone_lookup_index]);
              }
            }

            for (uint16_t bone = influences; bone < 4; ++bone)
            {
              _vertices[vertex].weights[bone] = 0;
              _vertices[vertex].bones[bone] = 0;
            }
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

    _render_flags = M2Array<ModelRenderFlags>(f, header.ofsRenderFlags, header.nRenderFlags);

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

  if (animBones)
  {
    ModelBoneDef const* mb = reinterpret_cast<ModelBoneDef const*>(f.getBuffer() + header.ofsBones);
    for (size_t i = 0; i<header.nBones; ++i)
    {
      bones.emplace_back(f, mb[i], _global_sequences.data(), animation_files);
    }

    bone_matrices.resize(bones.size());
  }  

  if (animTextures) 
  {
    _texture_animations.reserve(header.nTexAnims);
    ModelTexAnimDef const* ta = reinterpret_cast<ModelTexAnimDef const*>(f.getBuffer() + header.ofsTexAnims);
    for (size_t i=0; i<header.nTexAnims; ++i) {
      _texture_animations.emplace_back (f, ta[i], _global_sequences.data());
    }
  }

  
  // particle systems
  if (header.nParticleEmitters)
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
  if (header.nRibbonEmitters)
  {
    _ribbons.reserve(header.nRibbonEmitters);
    ModelRibbonEmitterDef const* rdefs = reinterpret_cast<ModelRibbonEmitterDef const*>(f.getBuffer() + header.ofsRibbonEmitters);
    for (size_t i = 0; i<header.nRibbonEmitters; ++i) {
      _ribbons.emplace_back(this, f, rdefs[i], _global_sequences.data(), _context);
    }
  }
  

  // init lights
  if (header.nLights)
  {
    _lights.reserve(header.nLights);
    ModelLightDef const* lDefs = reinterpret_cast<ModelLightDef const*>(f.getBuffer() + header.ofsLights);
    for (size_t i=0; i<header.nLights; ++i)
      _lights.emplace_back (f, lDefs[i], _global_sequences.data());
  }

  animcalc = false;
}

void Model::calcBones(glm::mat4x4 const& model_view
                     , int _anim
                     , int time
                     , int animation_time
                     )
{
  for (size_t i = 0; i<header.nBones; ++i)
  {
    bones[i].calc = false;
  }

  for (size_t i = 0; i<header.nBones; ++i)
  {
    bones[i].calcMatrix(model_view, bones.data(), _anim, time, animation_time);
  }
}

void Model::animate(glm::mat4x4 const& model_view, int anim_id, int anim_time)
{
  if (_uses_classic_layout)
  {
    calcClassicStaticBones(model_view);
    _renderer.updateBoneMatrices();
    return;
  }

  if (_animations_seq_per_id.empty() || _animations_seq_per_id[anim_id].empty())
  {
    return;
  }

  int tmax = _animation_length[anim_id];
  int t = anim_time % tmax;
  int current_sub_anim = 0;
  int time_for_anim = t;

  for (auto const& sub_animation : _animations_seq_per_id[anim_id])
  {
    if (static_cast<int>(sub_animation.second.length) > time_for_anim)
    {
      current_sub_anim = sub_animation.first;
      break;
    }

    time_for_anim -= sub_animation.second.length;
  }

  ModelAnimation const& a = _animations_seq_per_id[anim_id][current_sub_anim];

  _current_anim_seq = a.Index;//_animations_seq_lookup[anim_id][current_sub_anim];
  _anim_time = t;
  _global_animtime = anim_time;

  if (animBones) 
  {
    calcBones(model_view, _current_anim_seq, t, _global_animtime);
  }

  if (animGeometry || animBones)
  {
    std::size_t bone_counter = 0;
    for (auto& bone : bones)
    {
    	bone_matrices[bone_counter] = bone.mat;
      bone_counter++;
    }

    _renderer.updateBoneMatrices();


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

  for (size_t i=0; i<header.nLights; ++i) 
  {
    if (_lights[i].parent >= 0) 
    {
        _lights[i].tpos = bones[_lights[i].parent].mat * glm::vec4(_lights[i].pos,0);
      _lights[i].tdir = bones[_lights[i].parent].mrot * glm::vec4(_lights[i].dir,0);
    }
  }

  /*
  for (auto& particle : _particles)
  {
    // random time distribution for teh win ..?
    int pt = (t + static_cast<int>(tmax*particle.tofs)) % tmax;
    particle.setup(_current_anim_seq, pt, _global_animtime);
  }

  for (size_t i = 0; i<header.nRibbonEmitters; ++i) 
  {
    _ribbons[i].setup(_current_anim_seq, t, _global_animtime);
  }

   */

  for (auto& tex_anim : _texture_animations)
  {
    tex_anim.calc(_current_anim_seq, t, _anim_time);
  }
}

void TextureAnim::calc(int anim, int time, int animtime)
{
    mat = glm::mat4x4(1);
  if (trans.uses(anim)) 
  {
      mat = glm::translate(mat, trans.getValue(anim, time, animtime));
  }
  if (rot.uses(anim)) 
  {
      mat *= glm::toMat4(rot.getValue(anim, time, animtime));
  }
  if (scale.uses(anim)) 
  {
      mat = glm::scale(mat, scale.getValue(anim, time, animtime));
  }
}

ModelColor::ModelColor(const BlizzardArchive::ClientFile& f, const ModelColorDef &mcd, int *global)
  : color (mcd.color, f, global)
  , opacity(mcd.opacity, f, global)
{}

ModelTransparency::ModelTransparency(const BlizzardArchive::ClientFile& f, const ModelTransDef &mcd, int *global)
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
  , scale (mta.scale, f, global)
  , mat (glm::mat4x4())
{}

Bone::Bone( const BlizzardArchive::ClientFile& f,
            const ModelBoneDef &b,
            int *global,
            const std::vector<std::unique_ptr<BlizzardArchive::ClientFile>>& animation_files)
  : trans (b.translation, f, global, animation_files)
  , rot (b.rotation, f, global, animation_files)
  , scale (b.scaling, f, global, animation_files)
  , pivot (fixCoordSystem (b.pivot))
  , parent (b.parent)
{
  memcpy(&flags, &b.flags, sizeof(uint32_t));

  trans.apply(fixCoordSystem);
  rot.apply(fixCoordSystemQuat);
  scale.apply(fixCoordSystem2);
}

void Bone::calcMatrix(glm::mat4x4 const& model_view
                     , Bone *allbones
                     , int anim
                     , int time
                     , int animtime
                     )
{

  if (calc) return;

  glm::mat4x4 m = glm::mat4x4(1);
  glm::mat4x4 mr = glm::mat4x4(1);

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
      m = glm::translate(m, trans.getValue (anim, time, animtime));
    }

    if (rot.uses(anim))
    {
      glm::quat ref = glm::quat_cast(glm::mat4x4(1));
      glm::quat q = rot.getValue(anim, time, animtime);
      glm::vec3 rot_euler = glm::eulerAngles(q);

      glm::vec3 test_rot_vec = glm::vec3(rot_euler[2], 
        -(rot_euler[1] + glm::radians(180.f)),
        -(rot_euler[0] + glm::radians(180.f)));

      mr = glm::eulerAngleXYZ(test_rot_vec.x, test_rot_vec.y, test_rot_vec.z);

      m = m * mr;
    }

    if (scale.uses(anim))
    {
      m = glm::scale(m, scale.getValue (anim, time, animtime));
    }

    if (flags.billboard)
    {
        glm::vec3 vRight = model_view[0];
        glm::vec3 vUp = model_view[1]; 
    	vRight =  glm::vec3(vRight.x * -1, vRight.y * -1, vRight.z * -1);
        m[0][2] = vRight.x;
        m[1][2] = vRight.y;
        m[2][2] = vRight.z;
        m[0][1] = vUp.x;
        m[1][1] = vUp.y;
        m[2][1] = vUp.z;
    }

    m = glm::translate(m, -pivot);
  }

  if (parent >= 0)
  {
    allbones[parent].calcMatrix (model_view, allbones, anim, time, animtime);
    mat = allbones[parent].mat * m;
  }
  else
  {
    mat = m;
  }
  
  // transform matrix for normal vectors ... ??
  if (rot.uses(anim))
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


void Model::updateEmitters(float dt)
{
  return;

  if (finished)
  {
    for (auto& particle : _particles)
    {
      particle.update (dt);
    }
  }
}

