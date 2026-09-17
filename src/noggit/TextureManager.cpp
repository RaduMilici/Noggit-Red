// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/TextureManager.h>
#include <cstdlib>
#include <atomic> // [TEXARRAYDBG] temporary
#include <noggit/Log.h> // LogDebug
#include <noggit/frame_profiler.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtGui/QPixmap>

#include <algorithm>
#include <cctype>
#include <glm/vec2.hpp>

// [EXIT-CRASH FIX 2026-08-29] Intentionally LEAKED: the three object managers reference each other
// from their element destructors (~WMO -> ModelManager::_, ~Model -> TextureManager::_), and their
// cross-TU static destruction order is unspecified, so whichever died first left the others faulting
// on a destroyed mutex/map during process exit. Never destroying them removes the hazard entirely.
Noggit::AsyncObjectMultimap<blp_texture>& TextureManager::_ = *new Noggit::AsyncObjectMultimap<blp_texture>();
decltype (TextureManager::_tex_arrays) TextureManager::_tex_arrays;
decltype (TextureManager::_raw_textures) TextureManager::_raw_textures;
decltype (TextureManager::_raw_textures_mutex) TextureManager::_raw_textures_mutex;

// [perf 2026-08-06] Bumped once per successful blp_texture::upload(). The MDI doodad batcher folds this into
// its per-frame cache signature so a model that only becomes batchable once its texture finishes uploading
// forces a batch rebuild (its (model,count) signature is otherwise unchanged). External linkage; WorldRender
// declares `extern` for it (no header dependency). Reads are approximate (relaxed) -- staleness just costs one
// extra rebuild, never correctness.
std::atomic<unsigned long long> g_texture_upload_epoch{0};

// [perf 2026-08-05] Real multi-layer GL_TEXTURE_2D_ARRAYs (was 1 = every BLP its own single-layer array,
// array_index() always 0). Now up to N BLPs of the SAME (compression,w,h,mips) class share one array object,
// each on its own layer (index_x = n_used/N picks the array, layer = n_used%N). This is the PREREQ for
// Step-3 MDI doodad batching: many doodads that used to force a distinct bind-per-draw can now share one
// array bind + per-instance layer -> collapse into a single glMultiDrawElementsIndirect call.
//   * NO perf gain on its own -- draw-call count is unchanged until Step 3. This bump is a correctness gate:
//     the M2 shader samples texture(tex, vec3(uv, tex1_index)) and the WMO shader carries the layer per-vertex
//     through the render-batch TBO, so both already sample the right layer; verify no cross-layer bleed.
//   * VRAM: each class pre-allocates all N layers up front (immutable array size). ~11MB per 512^2-DXT1
//     64-layer array; worst case ~1GB of tail waste across all loaded classes -- fine on the 24GB target GPU.
//     Watch [MEM] vram= after the bump; dial back if it pressures the eviction path we just stabilised.
// [mem 2026-08-26] 64 -> 32: each NEW (format,size,mips) class preallocates ALL layers up front
// (a raw-RGBA8 1024^2 class was 341 MB the moment ONE such texture appeared). With layer
// recycling (free_slots) a second array per class rarely materializes, so halving the block
// halves the worst-case per-class cost with no visual change.
constexpr unsigned N_ARRAY_TEX = 32;
namespace
{
  constexpr char const* fallback_texture_filename = "tileset/generic/black.blp";

  // Anisotropic filtering (checklist 20.5): the client exposes this as a CVar; noggit had none, so
  // oblique/distant tilesets and model textures were blurrier than in-game. EXT_texture_filter_
  // anisotropic is core-adjacent and universally supported; constants defined locally since the
  // GL headers in use predate them. Level from QSettings render/anisotropic_filtering (default 16),
  // clamped to the hardware max; <= 1 disables. Re-read every call so the Settings slider applies
  // live -- TextureManager::reapply_anisotropy() re-runs it over every loaded array on a change.
  constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_LOCAL = 0x84FE;
  constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_LOCAL = 0x84FF;

  float anisotropy_level()
  {
    // hw_max is a hardware constant -> query it once (the first call happens during array creation,
    // with a live GL context). The requested level is re-read from QSettings every call.
    static float const hw_max = []
    {
      GLfloat m = 1.0f;
      gl.getFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_LOCAL, &m);
      return (m >= 1.0f) ? m : 1.0f; // extension absent or query failed -> no AF
    }();
    float const requested = QSettings().value("render/anisotropic_filtering", 16.0f).toFloat();
    // [VULKAN parity diagnostic] NOGGIT_PARITY_NO_AF isolates anisotropic-filtering implementation
    // differences between GL and VK by turning AF off on BOTH sides for a harness run.
    float const requested_eff = std::getenv("NOGGIT_PARITY_NO_AF") ? 1.0f : requested;
    return std::clamp(requested_eff, 1.0f, hw_max);
  }

  void apply_anisotropy(GLenum target)
  {
    float const level = anisotropy_level();
    if (level > 1.0f)
    {
      gl.texParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_LOCAL, level);
    }
  }

  bool is_null_texture_reference(std::string filename)
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
        return filename.substr(extension_pos) == ".blp";
      }
    }

    return false;
  }

  std::string safe_texture_filename(std::string filename)
  {
    if (is_null_texture_reference(filename))
    {
      return fallback_texture_filename;
    }

    filename = BlizzardArchive::ClientData::normalizeFilenameInternal(std::move(filename));
    return is_null_texture_reference(filename) ? fallback_texture_filename : std::move(filename);
  }

  // "fdid:<n>" -> key by fileDataID; anything else -> key by path
  BlizzardArchive::Listfile::FileKey texture_file_key(std::string const& filename)
  {
    if (filename.rfind("fdid:", 0) == 0)
    {
      std::uint32_t const fdid = static_cast<std::uint32_t>(std::strtoul(filename.c_str() + 5, nullptr, 10));
      if (fdid)
      {
        return BlizzardArchive::Listfile::FileKey(fdid);
      }
    }
    return BlizzardArchive::Listfile::FileKey(filename);
  }

  BlizzardArchive::Listfile::FileKey safe_texture_file_key(BlizzardArchive::Listfile::FileKey const& file_key)
  {
    if (!file_key.hasFilepath())
    {
      return BlizzardArchive::Listfile::FileKey(fallback_texture_filename);
    }

    return BlizzardArchive::Listfile::FileKey(safe_texture_filename(file_key.filepath()));
  }
}

void TextureManager::report()
{
  std::string output = "Still in the Texture manager:\n";
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const& key, blp_texture const&)
            {
              output += " - " + key.stringRepr() + "\n";
            }
          );
  LogDebug << output;
}

void TextureManager::unload_all(Noggit::NoggitRenderContext context)
{
  _.context_aware_apply(
      [&] (BlizzardArchive::Listfile::FileKey const&, blp_texture& blp_texture)
      {
          blp_texture.unload();
      }
      , context
  );

  // cleanup texture arrays
  auto& arrays_for_context = _tex_arrays[context];

  for (auto& pair : arrays_for_context)
  {
    gl.deleteTextures(static_cast<GLuint>(pair.second.arrays.size()), pair.second.arrays.data());
  }

  // Drop the bookkeeping too. Without this the map kept the DELETED GL names and their n_used counters,
  // so every later lookup for the same (format,size,mips) bucket walked past stale entries and the names
  // were never reconciled with reality -- GL is free to hand those exact names back out to new textures.
  // Clearing resets both the name list and n_used. [2026-07-30]
  arrays_for_context.clear();
}

void TextureManager::reapply_anisotropy()
{
  // Live re-apply of the anisotropic-filtering level (Settings -> Anisotropic filtering) to every
  // already-uploaded texture array: models, particles and tilesets all live in _tex_arrays. AF is
  // otherwise set once at array creation, so without this a change wouldn't take effect until the
  // texture reloaded. Must run with a current GL context (called from WorldRender::draw). The level
  // is always set explicitly, including 1 (= off), so lowering the setting actually clears a
  // previously-set higher value on each array.
  float const level = std::max(1.0f, anisotropy_level());
  for (auto& arrays_for_context : _tex_arrays)
  {
    for (auto& pair : arrays_for_context)
    {
      for (GLuint array : pair.second.arrays)
      {
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);
        gl.texParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_LOCAL, level);
      }
    }
  }
}

void TextureManager::register_raw_texture(std::string const& filename, Noggit::NoggitRenderContext context, int width, int height, std::vector<uint32_t> data)
{
  if (filename.empty() || width <= 0 || height <= 0 || data.empty())
  {
    return;
  }

  std::lock_guard<std::mutex> lock(_raw_textures_mutex);
  _raw_textures[{BlizzardArchive::ClientData::normalizeFilenameInternal(filename), static_cast<int>(context)}] = {width, height, std::move(data)};
}

bool TextureManager::load_raw_texture(std::string const& filename, Noggit::NoggitRenderContext context, int& width, int& height, std::map<int, std::vector<uint32_t>>& data)
{
  std::lock_guard<std::mutex> lock(_raw_textures_mutex);
  auto found = _raw_textures.find({BlizzardArchive::ClientData::normalizeFilenameInternal(filename), static_cast<int>(context)});
  if (found == _raw_textures.end())
  {
    return false;
  }

  width = found->second.width;
  height = found->second.height;
  data.clear();
  data.emplace(0, found->second.data);
  return true;
}

TexArrayParams& TextureManager::get_tex_array(int width, int height, int mip_level,
                                              Noggit::NoggitRenderContext context)
{
  TexArrayParams& array_params = _tex_arrays[context][std::make_tuple(-1, width, height, mip_level)];

  GLint n_layers = N_ARRAY_TEX;
  //gl.getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n_layers);

  int index_x = array_params.n_used / n_layers;

  // a recycled slot fits in an EXISTING array -- never grow capacity while one is available
  if (array_params.free_slots.empty() && array_params.arrays.size() <= index_x)
  {
    GLuint array;

    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    array_params.arrays.emplace_back(array);

    int width_ = width;
    int height_ = height;

    for (int i = 0; i < mip_level; ++i)
    {
      gl.texImage3D(GL_TEXTURE_2D_ARRAY, i, GL_RGBA8, width_, height_, n_layers, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                    nullptr);

      width_ = std::max(width_ >> 1, 1);
      height_ = std::max(height_ >> 1, 1);
    }

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 1);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, mip_level > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    apply_anisotropy(GL_TEXTURE_2D_ARRAY);
  }
  else
  {
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array_params.arrays[index_x]);
  }

  return array_params;
}

TexArrayParams& TextureManager::get_tex_array(GLint compression, int width, int height, int mip_level,
                              std::map<int, std::vector<uint8_t>>& comp_data, Noggit::NoggitRenderContext context)
{

  TexArrayParams& array_params = _tex_arrays[context][std::make_tuple(compression, width, height, mip_level)];

  GLint n_layers = N_ARRAY_TEX;
  //gl.getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n_layers);

  int index_x = array_params.n_used / n_layers;

  // a recycled slot fits in an EXISTING array -- never grow capacity while one is available
  if (array_params.free_slots.empty() && array_params.arrays.size() <= index_x)
  {
    GLuint array;

    gl.genTextures(1, &array);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array);

    array_params.arrays.emplace_back(array);

    int width_ = width;
    int height_ = height;

    for (int i = 0; i < mip_level; ++i)
    {
      gl.compressedTexImage3D(GL_TEXTURE_2D_ARRAY, i, compression, width_, height_, n_layers, 0, static_cast<GLsizei>(comp_data[i].size() * n_layers), nullptr);

      width_ = std::max(width_ >> 1, 1);
      height_ = std::max(height_ >> 1, 1);
    }

    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, mip_level - 1);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, mip_level > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    apply_anisotropy(GL_TEXTURE_2D_ARRAY);
  }
  else
  {
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, array_params.arrays[index_x]);
  }

  return array_params;
}

#include <cstdint>
//! \todo Cross-platform syntax for packed structs.
#pragma pack(push,1)
struct BLPHeader
{
  int32_t magix;
  int32_t version;
  uint8_t attr_0_compression;
  uint8_t attr_1_alphadepth;
  uint8_t attr_2_alphatype;
  uint8_t attr_3_mipmaplevels;
  int32_t resx;
  int32_t resy;
  int32_t offsets[16];
  int32_t sizes[16];
};
#pragma pack(pop)

void blp_texture::bind()
{
  if (!finished || loading_failed())
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  if (!_uploaded)
  {
    return;
  }

  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _texture_array);
}

void blp_texture::uploadToArray(unsigned layer)
{
  noggit::perf::Scoped _prof_tex(noggit::perf::Phase::TexUpload); // M2 spike hunt: BLP texture upload
  if (!finished)
  {
    try
    {
      finishLoading();
    }
    catch (...)
    {
      error_on_loading();
      return;
    }
  }

  if (loading_failed() || _width <= 0 || _height <= 0)
  {
    return;
  }

  int width = _width, height = _height;

  if (!_compression_format)
  {
    if (_data.empty())
    {
      return;
    }

    for (int i = 0; i < _data.size(); ++i)
    {
      gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, _data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    _data.clear();

  }
  else
  {
    if (_compressed_data.empty())
    {
      return;
    }

    for (int i = 0; i < _compressed_data.size(); ++i)
    {
      gl.compressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, layer, width, height, 1, _compression_format.value(), static_cast<GLsizei>(_compressed_data[i].size()), _compressed_data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    _compressed_data.clear();
  }
}

void blp_texture::upload()
{
  noggit::perf::Scoped _prof_tex(noggit::perf::Phase::TexUpload); // M2 spike hunt: BLP texture upload
  if (!finished || loading_failed())
  {
    return;
  }

  if (_uploaded)
  {
    return;
  }

  if (_width <= 0 || _height <= 0)
  {
    return;
  }

  // [wrong-texture root cause 2026-08-08] upload() runs LAZILY from inside the draw loop (bindTexture /
  // resolveStaticBatch resolve textures on first sight) and binds ITS array on whatever texture unit is
  // currently active to allocate/subimage. The draw paths cache "what's bound on unit N" CPU-side and
  // skip redundant glBindTexture -- so an upload mid-draw silently clobbers the unit and every cached
  // bind after it samples ANOTHER array (MC lavafalls turning catwalk-metal/molten-steel up close as
  // nearby textures streamed in; same class as the terrain wrong-texture bug). Restore the exact
  // binding we clobber. glGet is sync but uploads only happen on stream-in, never steady-state.
  GLint prev_active = GL_TEXTURE0;
  GLint prev_array = 0;
  gl.getIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  gl.getIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_array);
  struct RestoreBinding
  {
    GLint active;
    GLint array;
    ~RestoreBinding()
    {
      gl.activeTexture(static_cast<GLenum>(active));
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(array));
    }
  } const _restore{prev_active, prev_array};

  int width = _width, height = _height;

  GLint n_layers = N_ARRAY_TEX;
  //gl.getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n_layers);

  if (!_compression_format)
  {
    if (_data.empty())
    {
      return;
    }

    auto& params = TextureManager::get_tex_array( _width, _height, static_cast<int>(_data.size()), _context);

    // recycle a released slot before growing the class (see TexArrayParams::free_slots)
    int slot = params.n_used;
    if (!params.free_slots.empty())
    {
      slot = params.free_slots.back();
      params.free_slots.pop_back();
    }
    int index_x = slot / n_layers;
    int index_y = slot % n_layers;

    _texture_array = params.arrays[index_x];
    _array_index = index_y;
    _array_class = std::make_tuple(static_cast<GLint>(-1), _width, _height, static_cast<int>(_data.size()));
    _array_slot = slot;

    for (int i = 0; i < _data.size(); ++i)
    {
      gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, index_y, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, _data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    if (slot == params.n_used)
    {
      params.n_used++;
    }

    {
      static bool const s_texup_log = std::getenv("NOGGIT_TEXUP_LOG") != nullptr;
      if (s_texup_log)
      {
        LogError << "[TEXUP] " << _file_key.stringRepr() << " " << _width << "x" << _height
                 << " fmt=raw mips=" << _data.size()
                 << " arr=" << _texture_array << " layer=" << _array_index
                 << " ctx=" << static_cast<int>(_context) << std::endl;
      }
    }

    //LogDebug << "Mip level: " << std::to_string(_data.size()) << std::endl;

    _data.clear();
  }
  else
  {
    if (_compressed_data.empty())
    {
      return;
    }

    auto& params = TextureManager::get_tex_array(_compression_format.value(), _width, _height, static_cast<int>(_compressed_data.size()), _compressed_data, _context);

    // recycle a released slot before growing the class (see TexArrayParams::free_slots)
    int slot = params.n_used;
    if (!params.free_slots.empty())
    {
      slot = params.free_slots.back();
      params.free_slots.pop_back();
    }
    int index_x = slot / n_layers;
    int index_y = slot % n_layers;

    _texture_array = params.arrays[index_x];
    _array_index = index_y;
    _array_class = std::make_tuple(_compression_format.value(), _width, _height, static_cast<int>(_compressed_data.size()));
    _array_slot = slot;

    for (int i = 0; i < _compressed_data.size(); ++i)
    {
      gl.compressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, index_y, width, height, 1, _compression_format.value(), static_cast<GLsizei>(_compressed_data[i].size()), _compressed_data[i].data());

      width = std::max(width >> 1, 1);
      height = std::max(height >> 1, 1);
    }

    if (slot == params.n_used)
    {
      params.n_used++;
    }

    // [FALLS-DIAG 2026-08-08] NOGGIT_TEXUP_LOG=1: name every array upload (file -> array/layer) so a
    // layer collision or unexpected writer in a class is visible from one run. Diagnostic only.
    {
      static bool const s_texup_log = std::getenv("NOGGIT_TEXUP_LOG") != nullptr;
      if (s_texup_log)
      {
        LogError << "[TEXUP] " << _file_key.stringRepr() << " " << _width << "x" << _height
                 << " fmt=" << _compression_format.value() << " mips=" << _compressed_data.size()
                 << " arr=" << _texture_array << " layer=" << _array_index
                 << " ctx=" << static_cast<int>(_context) << std::endl;
      }
    }

    //LogDebug << "Mip level (compressed): " << std::to_string(_compressed_data.size()) << std::endl;
    _compressed_data.clear();
  }

  _uploaded = true;
  g_texture_upload_epoch.fetch_add(1, std::memory_order_relaxed); // MDI batcher cache-invalidation signal
}

// [mem 2026-08-26] layer recycling: without this, array layers were append-only -- every unique
// texture ever seen held its slot for the whole session (the refcounted blp_texture freed, its
// LAYER did not), so long fly-around sessions grew VRAM/driver RAM monotonically until the
// machine paged itself to death. All calls happen on the GL/main thread (same thread that
// uploads), matching the existing unsynchronized _tex_arrays access.
void blp_texture::release_array_slot()
{
  if (_array_slot < 0 || !_array_class)
  {
    return;
  }
  TextureManager::release_layer(_context, *_array_class, _array_slot);
  _array_slot = -1;
  _array_class.reset();
  _texture_array = 0;
  _array_index = -1;
}

blp_texture::~blp_texture()
{
  release_array_slot();
}

void TextureManager::release_layer(Noggit::NoggitRenderContext context,
                                   std::tuple<GLint, int, int, int> const& klass, int slot)
{
  // [2026-08-27 SHIRT BUG] slot REUSE is opt-in (NOGGIT_TEX_RECYCLE=1) until the pre-existing
  // refcount imbalance is fixed: AsyncObjectMultimap::erase's own comment documents tolerated
  // DOUBLE-RELEASES -- a texture can be destroyed while a live instance still draws its layer.
  // Append-only layers made that invisible (dead layers kept their pixels); reuse made every
  // such dangling holder show the slot's NEW occupant (several displays all wearing the same
  // white shirt). With the flag off free_slots stays empty, so upload() never recycles and
  // get_tex_array's growth guard is inert -- exact pre-round-54 layer behaviour.
  static bool const s_recycle = std::getenv("NOGGIT_TEX_RECYCLE") != nullptr;
  if (!s_recycle)
  {
    return;
  }
  auto& per_context = _tex_arrays[static_cast<std::size_t>(context)];
  auto const it = per_context.find(klass);
  if (it != per_context.end())
  {
    it->second.free_slots.push_back(slot);
  }
}

void blp_texture::unload()
{
  release_array_slot(); // the re-upload takes a (possibly different) recycled slot
  _uploaded = false;

  // load data back from file. pretty sad. maybe keep it after loading?
  finishLoading();
}

void blp_texture::loadFromUncompressedData(BLPHeader const* lHeader, char const* lData)
{
  unsigned int const* pal = reinterpret_cast<unsigned int const*>(lData + sizeof(BLPHeader));

  unsigned char const* buf;
  unsigned int *p;
  unsigned char const* c;
  unsigned char const* a;

  int alphabits = lHeader->attr_1_alphadepth;
  bool hasalpha = alphabits != 0;

  int width = _width, height = _height;

  for (int i = 0; i<16; ++i)
  {
    width = std::max(1, width);
    height = std::max(1, height);

    if (lHeader->offsets[i] > 0 && lHeader->sizes[i] > 0)
    {
      buf = reinterpret_cast<unsigned char const*>(&lData[lHeader->offsets[i]]);

      std::vector<uint32_t> data(lHeader->sizes[i]);

      int cnt = 0;
      p = data.data();
      c = buf;
      a = buf + width*height;
      for (int y = 0; y<height; y++)
      {
        for (int x = 0; x<width; x++)
        {
          unsigned int k = pal[*c++];
          k = ((k & 0x00FF0000) >> 16) | ((k & 0x0000FF00)) | ((k & 0x000000FF) << 16);

          int alpha = 0xFF;

          if (_is_tileset && !_is_specular)
          {
            alpha = 0x00;
          }
          else if (hasalpha)
          {
            if (alphabits == 8)
            {
              alpha = (*a++);
            }
            else if (alphabits == 1)
            {
              alpha = (*a & (1 << cnt++)) ? 0xff : 0;
              if (cnt == 8)
              {
                cnt = 0;
                a++;
              }
            }
            else if (alphabits == 4)
            {
              // 4-bit alpha (BLP2 alphaSize=4): two 4-bit alphas packed per byte, low nibble = even pixel;
              // expand the nibble to 8-bit (v<<4|v). Hits exactly ONE Turtle asset
              // (Character\Tauren\Male\TAURENMALESKIN00_20_EXTRA), which without this branch rendered opaque. (7.3)
              int const nib = (*a >> (4 * cnt)) & 0x0F;
              alpha = (nib << 4) | nib;
              if (++cnt == 2)
              {
                cnt = 0;
                a++;
              }
            }
          }

          k |= alpha << 24;
          *p++ = k;
        }
      }

      _data[i] = data;
    }
    else
    {
      return;
    }

    width >>= 1;
    height >>= 1;
  }
}

void blp_texture::loadFromCompressedData(BLPHeader const* lHeader, char const* lData)
{
  //                         0 (0000) & 3 == 0                1 (0001) & 3 == 1                    7 (0111) & 3 == 3
  const int alphatypes[] = { GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT };
  const int blocksizes[] = { 8, 16, 0, 16 };

  int alpha_type = lHeader->attr_2_alphatype & 3;
  GLint format = alphatypes[alpha_type];
  _compression_format = format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? (lHeader->attr_1_alphadepth == 1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT) : format;

  int width = _width, height = _height;

  for (int i = 0; i < 16; ++i)
  {
    if (lHeader->sizes[i] <= 0 || lHeader->offsets[i] <= 0)
    {
      return;
    }

    // make sure the vector is of the right size, blizzard seems to fuck those up for some small mipmaps
    int size = std::floor((width + 3) / 4) * std::floor((height + 3) / 4) * blocksizes[alpha_type];

    if (size < lHeader->sizes[i])
    {
      LogDebug << "mipmap size mismatch in '" << _file_key.stringRepr() << "'" << std::endl;
      return;
    }

    _compressed_data[i].resize(size);

    char const* start = lData + lHeader->offsets[i];
    std::copy(start, start + lHeader->sizes[i], _compressed_data[i].begin());

    width = std::max(width >> 1, 1);
    height = std::max(height >> 1, 1);
  }
}

blp_texture::blp_texture(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : AsyncObject(safe_texture_file_key(file_key))
  , _context(context)
{
}

void blp_texture::finishLoading()
{
  auto const texture_filename = _file_key.hasFilepath()
    ? safe_texture_filename(_file_key.filepath())
    : std::string(fallback_texture_filename);

  if (TextureManager::load_raw_texture(texture_filename, _context, _width, _height, _data))
  {
    finished = true;
    _state_changed.notify_all();
    return;
  }

  bool exists = Noggit::Application::NoggitApplication::instance()->clientData()->exists(texture_file_key(texture_filename));
  if (!exists)
  {
    LogError << "file not found: '" <<  _file_key.stringRepr() << "'" << std::endl;
  }

  std::string spec_filename;
  bool has_specular = false;

  if (texture_filename.starts_with("tileset/"))
  {
    _is_tileset = true;

    spec_filename = texture_filename.substr(0, texture_filename.find_last_of(".")) + "_s.blp";
    has_specular = Noggit::Application::NoggitApplication::instance()->clientData()->exists(spec_filename);

    if (has_specular)
    {
      _is_specular = true;
    }
  }

  // "fdid:<n>" names a texture by fileDataID (modern CASC clients: MDID / TXID / MOMT ids the listfile
  // does not know). Every cache in here stays keyed by that string; only the archive open changes.
  BlizzardArchive::ClientFile f(
      texture_file_key(exists ? (has_specular ? spec_filename : texture_filename) : "textures/shanecube.blp")
      , Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof() || f.getSize() < sizeof(BLPHeader))
  {
    finished = true;
    throw std::runtime_error ("File " + _file_key.stringRepr() + " does not exist");
  }

  char const* lData = f.getPointer();
  BLPHeader const* lHeader = reinterpret_cast<BLPHeader const*>(lData);
  _width = lHeader->resx;
  _height = lHeader->resy;

  if (lHeader->attr_0_compression == 1)
  {
    loadFromUncompressedData(lHeader, lData);
  }
  else if (lHeader->attr_0_compression == 2)
  {
    loadFromCompressedData(lHeader, lData);
  }
  else if (lHeader->attr_0_compression == 3)
  {
    // Uncompressed 32-bit BLP (D3DFMT_A8R8G8B8 -> BGRA bytes per pixel, e.g. Textures\sunGlare.blp).
    // Convert to RGBA for the GL_RGBA/GL_UNSIGNED_BYTE array upload (same target as the palettized path).
    int width = _width, height = _height;
    for (int i = 0; i < 16; ++i)
    {
      width = std::max(1, width);
      height = std::max(1, height);
      if (lHeader->offsets[i] > 0 && lHeader->sizes[i] > 0)
      {
        uint32_t const* src = reinterpret_cast<uint32_t const*>(&lData[lHeader->offsets[i]]);
        int const n = width * height;
        std::vector<uint32_t> data(n);
        for (int j = 0; j < n; ++j)
        {
          uint32_t const s = src[j]; // 0xAARRGGBB (BGRA byte order in memory)
          data[j] = ((s >> 16) & 0x000000FFu)  // R -> byte 0
                  | (s & 0x0000FF00u)           // G stays byte 1
                  | ((s & 0x000000FFu) << 16)   // B -> byte 2
                  | (s & 0xFF000000u);          // A stays byte 3
        }
        _data[i] = std::move(data);
      }
      else
      {
        break;
      }
      width >>= 1;
      height >>= 1;
    }
  }
  else
  {
    finished = true;
    throw std::logic_error ("unimplemented BLP colorEncoding");

  }

  f.close();
  finished = true;
  _state_changed.notify_all();
}

namespace Noggit
{

  QPixmap* BLPRenderer::render_blp_to_pixmap ( std::string const& blp_filename
                                               , int width
                                               , int height
                                               )
  {
    if (!_uploaded)
    [[unlikely]]
    {
      upload();
    }

    std::tuple<std::string, int, int> const curEntry{blp_filename, width, height};
    auto it{_cache.find(curEntry)};

    if(it != _cache.end())
      return &it->second;

    OpenGL::context::save_current_context const context_save (::gl);

    _context->makeCurrent(_surface.get());

    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    gl.activeTexture(GL_TEXTURE0);
    blp_texture texture(blp_filename, Noggit::NoggitRenderContext::BLP_RENDERER);
    try
    {
      texture.finishLoading();
      texture.upload();
    }
    catch (std::exception const& e)
    {
      // The texture (or the fallback the loader substitutes) cannot be read from this client -- the
      // Forever Beta ships a partial store with TACT-keyed content (docs/client_re/42 sec 17.1). The
      // painter's preview swatch used to take the whole map view down with an uncaught exception here.
      LogError << "BLPRenderer: '" << blp_filename << "' cannot be rendered (" << e.what() << "); black swatch" << std::endl;
      QPixmap blank(width == -1 ? 128 : width, height == -1 ? 128 : height);
      blank.fill(Qt::black);
      return &(_cache[curEntry] = std::move(blank));
    }

    width = width == -1 ? texture.width() : width;
    height = height == -1 ? texture.height() : height;

    float h = static_cast<float>(height);
    float w = static_cast<float>(width);

    QOpenGLFramebufferObject pixel_buffer(width, height, *_fmt.get());
    pixel_buffer.bind();

    gl.viewport(0, 0, w, h);
    gl.clearColor(.0f, .0f, .0f, 1.f);
    gl.clear(GL_COLOR_BUFFER_BIT);
    
    OpenGL::Scoped::use_program shader (*_program.get());

    shader.uniform("tex", 0);
    shader.uniform("width", w);
    shader.uniform("height", h);

    gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture.texture_array());
    shader.uniform("tex_index", texture.array_index());

    OpenGL::Scoped::vao_binder const _ (_vao[0]);
    
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_buffers[0]);

    gl.drawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    QPixmap result{};
    result = std::move(QPixmap::fromImage(pixel_buffer.toImage()));
    pixel_buffer.release();

    if (result.isNull())
    {
      throw std::runtime_error
        ("failed rendering " + blp_filename + " to pixmap");
    }

    return &(_cache[curEntry] = std::move(result));
  }

  void BLPRenderer::upload()
  {
    _cache = {};

    OpenGL::context::save_current_context const context_save (::gl);

    _context = std::make_unique<QOpenGLContext>();
    _fmt = std::make_unique<QOpenGLFramebufferObjectFormat>();
    _surface = std::make_unique<QOffscreenSurface>();

    _context->create();

    _fmt->setSamples(1);
    _fmt->setInternalTextureFormat(GL_RGBA8);

    _surface->create();
    _context->makeCurrent(_surface.get());

    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> cull;
    OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> depth;

    _vao.upload();
    _buffers.upload();

    GLuint const& indices_vbo = _buffers[0];
    GLuint const& vertices_vbo = _buffers[1];
    GLuint const& texcoords_vbo = _buffers[2];

    std::vector<glm::vec2> vertices =
        {
             {-1.0f, -1.0f}
            ,{-1.0f, 1.0f}
            ,{ 1.0f, 1.0f}
            ,{ 1.0f, -1.0f}
        };
    std::vector<glm::vec2> texcoords =
        {
             {0.f, 0.f}
            ,{0.f, 1.0f}
            ,{1.0f, 1.0f}
            ,{1.0f, 0.f}
        };
    std::vector<std::uint16_t> indices = {0,1,2, 2,3,0};

    gl.bufferData<GL_ARRAY_BUFFER,glm::vec2>(vertices_vbo, vertices, GL_STATIC_DRAW);
    gl.bufferData<GL_ARRAY_BUFFER,glm::vec2>(texcoords_vbo, texcoords, GL_STATIC_DRAW);
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(indices_vbo, indices, GL_STATIC_DRAW);


    _program.reset(new OpenGL::program
                       (
                           {
                               {
                                   GL_VERTEX_SHADER, R"code(
                                  #version 330 core

                                  in vec4 position;
                                  in vec2 tex_coord;
                                  out vec2 f_tex_coord;

                                  uniform float width;
                                  uniform float height;

                                  void main()
                                  {
                                    f_tex_coord = vec2(tex_coord.x * width, -tex_coord.y * height);
                                    gl_Position = vec4(position.x * width / 2, position.y * height / 2, position.z, 1.0);
                                  }
                                  )code"
                               },
                               {
                                   GL_FRAGMENT_SHADER, R"code(
                                  #version 330 core

                                  uniform sampler2DArray tex;
                                  uniform int tex_index;

                                  in vec2 f_tex_coord;

                                  layout(location = 0) out vec4 out_color;

                                  void main()
                                  {
                                    out_color = texture(tex, vec3(f_tex_coord/2.f + vec2(0.5), tex_index));
                                  }
                                  )code"
                               }
                           }
                       ));

    OpenGL::Scoped::use_program shader (*_program.get());

    OpenGL::Scoped::vao_binder const _ (_vao[0]);

    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> vertices_binder (vertices_vbo);
      shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
    }
    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> texcoords_binder (texcoords_vbo);
      shader.attrib("tex_coord", 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    _uploaded = true;
  }

  void BLPRenderer::unload()
  {
    OpenGL::context::save_current_context const context_save (::gl);
    _context->makeCurrent(_surface.get());
    OpenGL::context::scoped_setter const context_set (::gl, _context.get());

    _cache.clear();
    _vao.unload();
    _buffers.unload();
    _program.reset();
    _surface.reset();
    _fmt.reset();
    _context.reset();

    _uploaded = false;
  }

}

scoped_blp_texture_reference::scoped_blp_texture_reference (std::string const& filename, Noggit::NoggitRenderContext context)
  : _blp_texture(TextureManager::_.emplace(safe_texture_filename(filename), context))
  , _context(context)
{}

scoped_blp_texture_reference::scoped_blp_texture_reference (scoped_blp_texture_reference const& other)
  : _blp_texture(other._blp_texture ? TextureManager::_.emplace(safe_texture_filename(other._blp_texture->file_key().filepath()), other._context) : nullptr)
  , _context(other._context)
{}

void scoped_blp_texture_reference::Deleter::operator() (blp_texture* texture) const
{
  TextureManager::_.erase(texture->file_key().filepath(), texture->getContext());
}

blp_texture* scoped_blp_texture_reference::operator->() const
{
  return _blp_texture.get();
}

blp_texture* scoped_blp_texture_reference::get() const
{
  return _blp_texture.get();
}

bool scoped_blp_texture_reference::operator== (scoped_blp_texture_reference const& other) const
{
  return std::tie(_blp_texture) == std::tie(other._blp_texture);
}
