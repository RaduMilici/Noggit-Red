// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Misc.h>
#include <noggit/Particle.h>
#include <noggit/Log.h>
#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <opengl/shader.hpp>
#include <ClientFile.hpp>
#include <glm/vec3.hpp>


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

static const unsigned int MAX_PARTICLES = 10000;

namespace
{
  constexpr float CLASSIC_PARTICLE_MAX_SIZE = 20.0f;
  constexpr float CLASSIC_PARTICLE_MAX_RATE = 250.0f;
  constexpr float CLASSIC_PARTICLE_MAX_LIFESPAN = 20.0f;
  constexpr float CLASSIC_PARTICLE_MAX_TRAVEL = 200.0f;

  bool classic_effect_debug_enabled();
  float sane_classic_particle_size(float value, float fallback);

  bool particle_range_fits(BlizzardArchive::ClientFile const& file,
                           std::uint32_t offset,
                           std::uint32_t count,
                           std::size_t element_size)
  {
    return !count || (offset < file.getSize() && count <= (file.getSize() - offset) / element_size);
  }

  void read_particle_life_ramp(BlizzardArchive::ClientFile const& file,
                               ModelParticleParams const& params,
                               std::array<glm::vec4, 3>& colors,
                               std::array<float, 3>& sizes)
  {
    colors = {glm::vec4(1.f), glm::vec4(1.f), glm::vec4(1.f)};
    sizes = {1.f, 1.f, 1.f};

    if (params.colors.nKeys >= 3
        && particle_range_fits(file, params.colors.ofsKeys, 3, sizeof(glm::vec3)))
    {
      glm::vec3 raw_colors[3];
      std::memcpy(raw_colors, file.getBuffer() + params.colors.ofsKeys, sizeof(raw_colors));
      for (std::size_t i = 0; i < 3; ++i)
      {
        auto normalize_color_component = [](float value)
        {
          if (!std::isfinite(value))
          {
            return 1.0f;
          }

          return std::clamp(value > 1.0f ? value / 255.0f : value, 0.0f, 1.0f);
        };

        colors[i].r = normalize_color_component(raw_colors[i].x);
        colors[i].g = normalize_color_component(raw_colors[i].y);
        colors[i].b = normalize_color_component(raw_colors[i].z);
      }
    }

    if (params.opacity.nKeys >= 3
        && particle_range_fits(file, params.opacity.ofsKeys, 3, sizeof(int16_t)))
    {
      for (std::size_t i = 0; i < 3; ++i)
      {
        auto const opacity = *reinterpret_cast<int16_t const*>(file.getBuffer() + params.opacity.ofsKeys + i * sizeof(int16_t));
        colors[i].a = std::clamp(opacity / 32767.0f, 0.0f, 1.0f);
      }
    }

    if (params.sizes.nKeys >= 3
        && particle_range_fits(file, params.sizes.ofsKeys, 3, sizeof(glm::vec2)))
    {
      glm::vec2 raw_sizes[3];
      std::memcpy(raw_sizes, file.getBuffer() + params.sizes.ofsKeys, sizeof(raw_sizes));
      for (std::size_t i = 0; i < 3; ++i)
      {
        float const raw_size = std::max(std::abs(raw_sizes[i].x), std::abs(raw_sizes[i].y));
        float const scale = std::isfinite(params.scales[i]) && std::abs(params.scales[i]) > 0.001f
          ? std::abs(params.scales[i])
          : 1.0f;
        sizes[i] = raw_size * scale;
      }
    }
    else if (params.sizes.nKeys >= 3
             && particle_range_fits(file, params.sizes.ofsKeys, 3, sizeof(float)))
    {
      for (std::size_t i = 0; i < 3; ++i)
      {
        float const raw_size = *reinterpret_cast<float const*>(file.getBuffer() + params.sizes.ofsKeys + i * sizeof(float));
        float const scale = std::isfinite(params.scales[i]) && std::abs(params.scales[i]) > 0.001f
          ? std::abs(params.scales[i])
          : 1.0f;
        sizes[i] = std::abs(raw_size) * scale;
      }
    }
  }

  void read_classic_particle_life_ramp(ClassicModelParticleParams const& params,
                                       float& mid,
                                       std::array<glm::vec4, 3>& colors,
                                       std::array<float, 3>& sizes)
  {
    if (std::isfinite(params.midPoint) && params.midPoint > 0.001f && params.midPoint < 0.999f)
    {
      mid = params.midPoint;
    }

    for (std::size_t i = 0; i < colors.size(); ++i)
    {
      auto const& color = params.colorValues[i];
      colors[i] = glm::vec4(color.blue / 255.0f,
                            color.green / 255.0f,
                            color.red / 255.0f,
                            color.alpha / 255.0f);

      float const burst_multiplier = std::isfinite(params.burstMultiplier) && params.burstMultiplier > 0.001f
        ? params.burstMultiplier
        : 1.0f;
      float const size = params.scalesValues[i] * burst_multiplier;
      if (std::isfinite(size) && size > 0.0f && size < 100.0f)
      {
        sizes[i] = sane_classic_particle_size(size, sizes[i]);
      }
    }
  }

  float sane_particle_value(float value, float fallback)
  {
    return std::isfinite(value) ? value : fallback;
  }

  float sane_positive_particle_value(float value, float fallback)
  {
    return std::isfinite(value) && value > 0.0f ? value : fallback;
  }

  float sane_classic_particle_size(float value, float fallback)
  {
    if (!std::isfinite(value) || value <= 0.0f)
    {
      value = fallback;
    }

    return std::clamp(value, 0.001f, CLASSIC_PARTICLE_MAX_SIZE);
  }

  bool finite_vec3(glm::vec3 const& value)
  {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
  }

  bool finite_vec4(glm::vec4 const& value)
  {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
  }

  glm::vec3 safe_normalize_vec3(glm::vec3 const& value, glm::vec3 const& fallback)
  {
    if (!finite_vec3(value))
    {
      return fallback;
    }

    float const length_squared = glm::dot(value, value);
    if (!std::isfinite(length_squared) || length_squared <= 0.000001f)
    {
      return fallback;
    }

    return glm::normalize(value);
  }

  void log_classic_particle_ramp_probe(Model const* model,
                                       ClassicModelParticleParams const& params,
                                       std::array<glm::vec4, 3> const& colors,
                                       std::array<float, 3> const& sizes)
  {
    if (!classic_effect_debug_enabled())
    {
      return;
    }

    std::ostringstream line;
    line << "Classic effect particle ramp model='"
         << (model ? model->file_key().stringRepr() : std::string("<null>"))
         << "' midPoint=" << params.midPoint
         << " burstMultiplier=" << params.burstMultiplier
         << " drag=" << params.drag
         << " inlineColors=["
         << "{" << static_cast<int>(params.colorValues[0].red) << ", "
         << static_cast<int>(params.colorValues[0].green) << ", "
         << static_cast<int>(params.colorValues[0].blue) << ", "
         << static_cast<int>(params.colorValues[0].alpha) << "}, "
         << "{" << static_cast<int>(params.colorValues[1].red) << ", "
         << static_cast<int>(params.colorValues[1].green) << ", "
         << static_cast<int>(params.colorValues[1].blue) << ", "
         << static_cast<int>(params.colorValues[1].alpha) << "}, "
         << "{" << static_cast<int>(params.colorValues[2].red) << ", "
         << static_cast<int>(params.colorValues[2].green) << ", "
         << static_cast<int>(params.colorValues[2].blue) << ", "
         << static_cast<int>(params.colorValues[2].alpha) << "}]"
         << " inlineScales=[" << params.scalesValues[0] << ", "
         << params.scalesValues[1] << ", "
         << params.scalesValues[2] << "]"
         << " readColors=["
         << "{" << colors[0].r << ", " << colors[0].g << ", " << colors[0].b << ", " << colors[0].a << "}, "
         << "{" << colors[1].r << ", " << colors[1].g << ", " << colors[1].b << ", " << colors[1].a << "}, "
         << "{" << colors[2].r << ", " << colors[2].g << ", " << colors[2].b << ", " << colors[2].a << "}]"
         << " readSizes=[" << sizes[0] << ", " << sizes[1] << ", " << sizes[2] << "]";

    auto const* raw_params = reinterpret_cast<float const*>(&params);
    line << " rawParamF32=[";
    for (std::size_t i = 0; i < 32 && i * sizeof(float) < sizeof(ModelParticleParams); ++i)
    {
      if (i != 0)
      {
        line << ", ";
      }
      line << raw_params[i];
    }
    line << "]";

    LogDebug << line.str() << std::endl;
  }

  Bone* particle_parent_bone(Model* model, int16_t bone)
  {
    if (!model || model->bones.empty())
    {
      throw std::logic_error("particle emitter has no parent bone");
    }

    if (bone < 0 || static_cast<std::size_t>(bone) >= model->bones.size())
    {
      return &model->bones.front();
    }

    return &model->bones[bone];
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

      char const* classic_debug = std::getenv("NOGGIT_CLASSIC_M2_DEBUG");
      return classic_debug && *classic_debug && std::strcmp(classic_debug, "0") != 0;
    }();

    return enabled;
  }

}

template<class T>
T lifeRamp(float life, float mid, const T &a, const T &b, const T &c)
{
  if (life <= mid) return math::interpolation::linear(life / mid, a, b);
  else return math::interpolation::linear((life - mid) / (1.0f - mid), b, c);
}

ParticleSystem::ParticleSystem(Model* model_
                               , const BlizzardArchive::ClientFile& f
                               , const ModelParticleEmitterDef &mta
                               , int *globals
                               , Noggit::NoggitRenderContext context)
  : model (model_)
  , emitter_type(mta.EmitterType)
  , emitter ( mta.EmitterType == 1 ? std::unique_ptr<ParticleEmitter> (std::make_unique<PlaneParticleEmitter>())
            : mta.EmitterType == 2 ? std::unique_ptr<ParticleEmitter> (std::make_unique<SphereParticleEmitter>())
            : std::unique_ptr<ParticleEmitter> (std::make_unique<PlaneParticleEmitter>())
            )
  , speed (mta.EmissionSpeed, f, globals)
  , variation (mta.SpeedVariation, f, globals)
  , spread (mta.VerticalRange, f, globals)
  , lat (mta.HorizontalRange, f, globals)
  , gravity (mta.Gravity, f, globals)
  , lifespan (mta.Lifespan, f, globals)
  , rate (mta.EmissionRate, f, globals)
  , areal (mta.EmissionAreaLength, f, globals)
  , areaw (mta.EmissionAreaWidth, f, globals)
  , deacceleration (mta.Gravity2, f, globals)
  , enabled (mta.en, f, globals)
  , mid (0.5)
  , slowdown (mta.p.slowdown)
  , pos (fixCoordSystem(mta.pos))
  , _texture_id (mta.texture)
  , blend (mta.blend)
  , order (mta.ParticleType > 0 ? -1 : 0)
  , type (mta.ParticleType)
  , manim (0)
  , mtime (0)
  , manimtime(0)
  , rows (mta.rows)
  , cols (mta.cols)
  , billboard (!(mta.flags & 4096))
  , classic(false)
  , rem(0)
  , parent (particle_parent_bone(model, mta.bone))
  , flags(mta.flags)
  , tofs (misc::frand())
  , _context(context)
{
  _bone_index = mta.bone;
  read_particle_life_ramp(f, mta.p, colors, sizes);

  //transform = mta.flags & 1024;

  // init tiles
  for (int i = 0; i<rows*cols; ++i) {
    TexCoordSet tc;
    initTile(tc.tc, i);
    tiles.push_back(tc);
  }
}

ParticleSystem::ParticleSystem(Model* model_
                               , const BlizzardArchive::ClientFile& f
                               , const ClassicModelParticleEmitterDef& mta
                               , int* globals
                               , Noggit::NoggitRenderContext context)
  : model(model_)
  , emitter_type(mta.EmitterType)
  , emitter(mta.EmitterType == 1 ? std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>())
           : mta.EmitterType == 2 ? std::unique_ptr<ParticleEmitter>(std::make_unique<SphereParticleEmitter>())
           : std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>()))
  , speed(mta.EmissionSpeed, f, globals)
  , variation(mta.SpeedVariation, f, globals)
  , spread(mta.VerticalRange, f, globals)
  , lat(mta.HorizontalRange, f, globals)
  , gravity(mta.Gravity, f, globals)
  , lifespan(mta.Lifespan, f, globals)
  , rate(mta.EmissionRate, f, globals)
  , areal(mta.EmissionAreaLength, f, globals)
  , areaw(mta.EmissionAreaWidth, f, globals)
  , deacceleration(mta.Gravity2, f, globals)
  , enabled(mta.en, f, globals)
  , mid(0.5)
  , slowdown(std::isfinite(mta.p.drag) ? mta.p.drag : 0.0f)
  , pos(fixCoordSystem(mta.pos))
  , _texture_id(mta.texture)
  , blend(mta.blend)
  , order(0)
  , type(0)
  , manim(0)
  , mtime(0)
  , manimtime(0)
  , rows(mta.rows > 0 ? mta.rows : 1)
  , cols(mta.cols > 0 ? mta.cols : 1)
  , billboard(!(mta.flags & 4096))
  , classic(true)
  , rem(0)
  , parent(particle_parent_bone(model, mta.bone))
  , flags(mta.flags)
  , tofs(misc::frand())
  , _context(context)
{
  _bone_index = mta.bone;
  // Emitter spin: for a spline emitter (M2 EmitterType 3) this is how fast the emission point travels
  // along the spline. classic models carry it in params.spin; WotLK params don't.
  _spin = std::isfinite(mta.p.spin) ? mta.p.spin : 0.f;

  // Spline path (the MC flamecircle's ring etc.). The emitter stores nSplinePoints vec3s at
  // ofsSplinePoints; particles emit along this loop instead of from the bone origin. Read & convert to
  // Noggit render space here so newParticle just samples the cached points.
  if (mta.p.nSplinePoints > 0 && mta.p.nSplinePoints < 4096)
  {
    std::size_t const need = static_cast<std::size_t>(mta.p.ofsSplinePoints)
                           + static_cast<std::size_t>(mta.p.nSplinePoints) * sizeof(glm::vec3);
    if (f.getBuffer() && need <= f.getSize())
    {
      auto const* raw = reinterpret_cast<glm::vec3 const*>(f.getBuffer() + mta.p.ofsSplinePoints);
      _spline_points.reserve(mta.p.nSplinePoints);
      for (std::uint32_t i = 0; i < mta.p.nSplinePoints; ++i)
      {
        _spline_points.push_back(fixCoordSystem(raw[i]));
      }
    }
  }

  if (classic_effect_debug_enabled())
  {
    LogDebug << "Classic emitter spin probe model='"
             << (model ? model->file_key().stringRepr() : std::string("<null>"))
             << "' emitterType=" << static_cast<int>(mta.EmitterType)
             << " spin=" << mta.p.spin
             << " nSplinePoints=" << mta.p.nSplinePoints << " ofsSplinePoints=" << mta.p.ofsSplinePoints
             << " splineLoaded=" << _spline_points.size()
             << " EmissionSpeed=" << speed.getValue(0, 0, 0)
             << " emitterPos=(" << pos.x << ", " << pos.y << ", " << pos.z << ")"
             << " bone=" << mta.bone
             << std::endl;
  }

  colors = {glm::vec4(1.f), glm::vec4(1.f), glm::vec4(1.f)};
  sizes = {1.f, 1.f, 1.f};
  read_classic_particle_life_ramp(mta.p, mid, colors, sizes);
  for (float& size : sizes)
  {
    size = sane_classic_particle_size(size, 1.0f);
  }
  log_classic_particle_ramp_probe(model, mta.p, colors, sizes);

  // Flipbook cell animation. The classic params carry lifespanUVAnim/decayUVAnim = [startCell,
  // endCell, repeat]; the texture cell is meant to advance sequentially across the rows*cols sheet
  // over each particle's lifetime (e.g. 4x4 life=(0,7)+decay=(8,16) = a 16-frame explosion; 8x8
  // (0,31)+(32,63) = a full 64-frame animation). Treat the whole [life.start .. furthest end] as one
  // sequence played over rlife. Only enable when the emitter authors a real range on a multi-cell
  // sheet -- degenerate (0,0,1) sheets fall through to the existing random-tile behaviour (variety),
  // so no regression for the ~22% that don't flipbook. Verified by scanning 2195 multi-cell emitters.
  {
    int const cell_count = std::max(1, rows * cols);
    int const seq_start = mta.p.lifespanUVAnim[0];
    int const seq_end = std::max({ static_cast<int>(mta.p.lifespanUVAnim[1])
                                 , static_cast<int>(mta.p.decayUVAnim[0])
                                 , static_cast<int>(mta.p.decayUVAnim[1]) });
    int const repeat = mta.p.lifespanUVAnim[2] > 0 ? mta.p.lifespanUVAnim[2] : 1;
    if (cell_count > 1 && seq_end > seq_start && seq_start >= 0)
    {
      _uv_animated = true;
      _uv_seq_start = seq_start;
      _uv_seq_end = std::min(seq_end, cell_count - 1); // clamp: tile index must stay in [0,cells-1]
      _uv_repeat = repeat;
    }
  }

  // GENERAL tiny-particle visibility floor (replaces the old per-model "volumetriclight" size x10 hack).
  // Some emitters author sub-pixel particle sizes (the volumetric-light dust sparkles are ~0.014 units)
  // that simply vanish in the editor. Scale such emitters up to a minimum readable size, and -- for
  // ADDITIVE blends, where the rendered brightness scales ~size^2 -- scale the alpha DOWN by f^2 so the
  // brightness stays data-faithful (a small visible faint speck, not a bright blob). This is data-derived
  // (the factor comes from the authored size), works for every tiny-particle effect, and needs no
  // per-model tuning.
  {
    float const peak_size = std::max({ sizes[0], sizes[1], sizes[2] });
    float const MIN_VISIBLE_SIZE = 0.06f; // min readable world size; gentler than 0.10 so fire embers
                                          // (peak ~0.03-0.05) aren't over-enlarged into the bright core.
    if (peak_size > 0.0f && peak_size < MIN_VISIBLE_SIZE)
    {
      float const f = MIN_VISIBLE_SIZE / peak_size;
      for (float& s : sizes)
      {
        s *= f;
      }
      // ADDITIVE blends (NoAlphaAdd 3 / Add 4 / InvAlphaAdd 7): rendered brightness scales with the
      // particle AREA (~size^2), so enlarging a small additive speck without compensating makes it
      // BRIGHTER. That blew out dense additive fire -- the Forgebonfire / firepit ember emitters
      // (peak ~0.03-0.05) were floored ~2x and so ~4x too bright, stacking into the white core. Scale the
      // alpha down by 1/f^2 so the integrated brightness stays data-faithful: a faint speck made visible,
      // not a bright blob. (Alpha-blend dust keeps full alpha -- its brightness isn't area-additive.)
      if (blend == 3 || blend == 4 || blend == 7)
      {
        float const inv_area = 1.0f / (f * f);
        for (auto& c : colors)
        {
          c.a *= inv_area;
        }
      }
    }
  }

  // Large-area, alpha-blended ambient dust/fog (e.g. the Timbermaw furbolg dust: emission area ~8-12u,
  // ~270 motes) fills a big volume with many overlapping semi-transparent particles. In the editor's
  // brighter, un-fogged scene that overlap reads as near-opaque, unlike the dark in-game cave where the
  // same motes are subtle. Scale the opacity of big-area alpha-blend emitters down so the cumulative
  // density matches the in-game subtlety. Data-driven (emission area + alpha blend), not a per-model
  // hack -- localized effects (torches, small dust/steam) have small areas and keep their full alpha.
  if (blend == 2)
  {
    float const area = std::max(areal.getValue(0, 0, 0), areaw.getValue(0, 0, 0));
    if (std::isfinite(area) && area > 5.0f)
    {
      for (glm::vec4& ramp_color : colors)
      {
        ramp_color.a *= 0.2f;
      }
    }
  }

  for (int i = 0; i < rows * cols; ++i)
  {
    TexCoordSet tc;
    initTile(tc.tc, i);
    tiles.push_back(tc);
  }
}

ParticleSystem::ParticleSystem(ParticleSystem const& other)
  : model(other.model)
  , emitter_type(other.emitter_type)
  , emitter( emitter_type == 1 ? std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>())
           : emitter_type == 2 ? std::unique_ptr<ParticleEmitter>(std::make_unique<SphereParticleEmitter>())
           : std::unique_ptr<ParticleEmitter>(std::make_unique<PlaneParticleEmitter>())
           )
  , speed(other.speed)
  , variation(other.variation)
  , spread(other.spread)
  , lat(other.lat)
  , gravity(other.gravity)
  , lifespan(other.lifespan)
  , rate(other.rate)
  , areal(other.areal)
  , areaw(other.areaw)
  , deacceleration(other.deacceleration)
  , enabled(other.enabled)
  , colors(other.colors)
  , sizes(other.sizes)
  , mid(other.mid)
  , slowdown(other.slowdown)
  , _spin(other._spin)
  , _spline_points(other._spline_points)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , particles(other.particles)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(other.tiles)
  , _uv_animated(other._uv_animated)
  , _uv_seq_start(other._uv_seq_start)
  , _uv_seq_end(other._uv_seq_end)
  , _uv_repeat(other._uv_repeat)
  , billboard(other.billboard)
  , classic(other.classic)
  , debug_update_log_count(other.debug_update_log_count)
  , debug_draw_log_count(other.debug_draw_log_count)
  , rem(other.rem)
  , parent(other.parent)
  , flags(other.flags)
  , tofs(other.tofs)
  , _context(other._context)
{

}

ParticleSystem::ParticleSystem(ParticleSystem&& other)
  : model(other.model)
  , emitter_type(other.emitter_type)
  , emitter(std::move(other.emitter))
  , speed(other.speed)
  , variation(other.variation)
  , spread(other.spread)
  , lat(other.lat)
  , gravity(other.gravity)
  , lifespan(other.lifespan)
  , rate(other.rate)
  , areal(other.areal)
  , areaw(other.areaw)
  , deacceleration(other.deacceleration)
  , enabled(other.enabled)
  , colors(other.colors)
  , sizes(other.sizes)
  , mid(other.mid)
  , slowdown(other.slowdown)
  , _spin(other._spin)
  , _spline_points(other._spline_points)
  , pos(other.pos)
  , _texture_id(other._texture_id)
  , particles(other.particles)
  , blend(other.blend)
  , order(other.order)
  , type(other.type)
  , manim(other.manim)
  , mtime(other.mtime)
  , manimtime(other.manimtime)
  , rows(other.rows)
  , cols(other.cols)
  , tiles(other.tiles)
  , _uv_animated(other._uv_animated)
  , _uv_seq_start(other._uv_seq_start)
  , _uv_seq_end(other._uv_seq_end)
  , _uv_repeat(other._uv_repeat)
  , billboard(other.billboard)
  , classic(other.classic)
  , debug_update_log_count(other.debug_update_log_count)
  , debug_draw_log_count(other.debug_draw_log_count)
  , rem(other.rem)
  , parent(other.parent)
  , flags(other.flags)
  , tofs(other.tofs)
  , _context(other._context)
{

}

void ParticleSystem::initTile(glm::vec2 *tc, int num)
{
  glm::vec2 otc[4];
  glm::vec2 a, b;
  int x = num % cols;
  int y = num / cols;
  a.x = x * (1.0f / cols);
  b.x = (x + 1) * (1.0f / cols);
  a.y = y * (1.0f / rows);
  b.y = (y + 1) * (1.0f / rows);

  otc[0] = a;
  otc[2] = b;
  otc[1].x = b.x;
  otc[1].y = a.y;
  otc[3].x = a.x;
  otc[3].y = b.y;

  for (int i = 0; i<4; ++i) {
    tc[(i + 4 - order) & 3] = otc[i];
  }
}


void ParticleSystem::update(float dt)
{
  // Re-resolve the parent bone from its index (a cached Bone* goes stale after the model's bones vector is
  // rebuilt -> parent->mat was ZERO -> particles spawned at the origin). Do it here where newParticle reads
  // sys->parent->mat/.mrot for the spawn position + emission direction, so particles attach to and follow
  // the animated bone.
  parent = particle_parent_bone(model, _bone_index);

  // Pre-warm continuous emitters to steady state on first tick so ambient effects (dusty light-ray
  // motes, smoke) appear already populated/scattered instead of slowly filling from empty over a
  // full lifespan. Recursive calls see the guard set and skip the pre-warm.
  if (!_prewarmed)
  {
    _prewarmed = true;
    if (emitter)
    {
      float warm_life = sane_positive_particle_value(lifespan.getValue(manim, mtime, manimtime), 0.0f);
      if (classic)
      {
        warm_life = std::min(warm_life, CLASSIC_PARTICLE_MAX_LIFESPAN);
      }
      if (warm_life > 0.05f)
      {
        constexpr int prewarm_steps = 40;
        float const step_dt = warm_life / static_cast<float>(prewarm_steps);
        for (int s = 0; s < prewarm_steps; ++s)
        {
          // Jitter each step so particles don't all spawn at identical intervals (which lands them
          // in evenly-spaced rows -- a visible grid). frand() in [0,1) -> 0.25x..1.75x, averaging 1x
          // so the total simulated span still ~= one lifespan.
          update(step_dt * (0.25f + 1.5f * misc::frand()));
        }
      }
    }
  }

  std::size_t const particles_before_update = particles.size();
  int spawned_this_update = 0;
  float debug_rate = 0.0f;
  float debug_life = 0.0f;

  float grav = sane_particle_value(gravity.getValue(manim, mtime, manimtime), 0.0f);
  float deaccel = sane_particle_value(deacceleration.getValue(manim, mtime, manimtime), 0.0f);

  // spawn new particles
  if (emitter) {
    float frate = std::max(0.0f, sane_particle_value(rate.getValue(manim, mtime, manimtime), 0.0f));

    float const raw_life = lifespan.getValue(manim, mtime, manimtime);
    float flife = sane_positive_particle_value(raw_life, 0.0f);
    if (classic)
    {
      frate = std::min(frate, CLASSIC_PARTICLE_MAX_RATE);
      flife = std::min(flife, CLASSIC_PARTICLE_MAX_LIFESPAN);
    }
    // (Removed the volumetric-light rate x0.2 hand-tune: it suppressed the authored emission so the white
    //  sparkle motes went missing. Use the real authored rate -- the bloom over-brightening that this hack
    //  was compensating for is now fixed at the source, so the data-driven rate is correct.)
    debug_rate = frate;
    debug_life = flife;

    bool const enabled_uses_current_animation = enabled.uses(manim);
    bool const enabled_uses_default_animation = classic && !enabled_uses_current_animation && enabled.uses(0);
    bool en = true;
    if (enabled_uses_current_animation)
    {
      en = enabled.getValue(manim, mtime, manimtime) != 0;
    }
    else if (enabled_uses_default_animation)
    {
      en = enabled.getValue(0, mtime, manimtime) != 0;
    }

    // [ENBUCKET] one-time per anomalus chest emitter: how did the enabled track bucket, and what time are
    // we sampling it at?
    if (classic && _bone_index == 64 && model && model->file_key().hasFilepath()
        && model->file_key().filepath().find("anomalus") != std::string::npos)
    {
      static int enb = 0;
      if (enb < 4)
      {
        ++enb;
        LogDebug << "[ENBUCKET] bone=64 manim=" << manim << " mtime=" << mtime << " manimtime=" << manimtime
                 << " usesCur=" << enabled_uses_current_animation << " usesDef=" << enabled_uses_default_animation
                 << " en=" << en << " { " << enabled.debugBuckets() << " }" << std::endl;
      }
    }

    if (frate <= 0.0f || flife <= 0.0f || !en)
    {
      rem = 0.0f;
      if (classic_effect_debug_enabled() && classic && debug_update_log_count < 12)
      {
        ++debug_update_log_count;
        LogDebug << "Classic effect particle update model='" << model->file_key().stringRepr()
                 << "' source=classic"
                 << " dt=" << dt
                 << " rate=" << debug_rate
                 << " life=" << debug_life
                 << " spawned=0"
                 << " before=" << particles_before_update
                 << " after=" << particles.size()
                 << " rem=" << rem
                 << " texture=" << _texture_id
                 << " type=" << type
                 << " blend=" << blend
                 << " enabled=" << (en ? 1 : 0)
                 << " enabledUsesAnim=" << (enabled_uses_current_animation ? 1 : 0)
                 << " enabledUsesDefault=" << (enabled_uses_default_animation ? 1 : 0)
                 << std::endl;
      }
    }
    else {
      float ftospawn = (dt * (classic ? frate : frate / flife)) + rem;
      if (ftospawn < 1.0f) {
        rem = ftospawn;
        if (rem<0)
          rem = 0;
      }
      else {
        int tospawn = (int)ftospawn;

        if (particles.size() >= MAX_PARTICLES)
        {
          tospawn = 0;
        }
        else if ((static_cast<std::size_t>(tospawn) + particles.size()) > MAX_PARTICLES) // Error check to prevent the program from trying to load insane amounts of particles.
        {
          tospawn = static_cast<int>(MAX_PARTICLES - particles.size());
        }

        rem = ftospawn - static_cast<float>(tospawn);

        float w = sane_particle_value(areal.getValue(manim, mtime, manimtime), 0.0f) * 0.5f;
        float l = sane_particle_value(areaw.getValue(manim, mtime, manimtime), 0.0f) * 0.5f;
        float spd = sane_particle_value(speed.getValue(manim, mtime, manimtime), 0.0f);
        float var = sane_particle_value(variation.getValue(manim, mtime, manimtime), 0.0f);
        float spr = sane_particle_value(spread.getValue(manim, mtime, manimtime), 0.0f);
        float spr2 = sane_particle_value(lat.getValue(manim, mtime, manimtime), 0.0f);

        //rem = 0;
        for (int i = 0; i<tospawn; ++i) {
          Particle p = emitter->newParticle(this, manim, mtime, manimtime, w, l, spd, var, spr, spr2);

          // TEMP [PARTDBG]: where does Noggit actually attach/spawn each Anomalus emitter? Logs the bone's
          // world (model-local) position, the spawned particle position + emission direction, per emitter.
          static int part_dbg_count = 0;
          if (i == 0 && classic_effect_debug_enabled() && model && model->file_key().hasFilepath()
              && model->file_key().filepath().find("anomalus") != std::string::npos
              && part_dbg_count < 80)
          {
            ++part_dbg_count;
            glm::vec3 const bone_pos(parent->mat[3][0], parent->mat[3][1], parent->mat[3][2]);
            LogDebug << "[PARTDBG] tex=" << _texture_id << " type=" << emitter_type
                     << " emitterLocalPos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
                     << " boneWorldPos=(" << bone_pos.x << "," << bone_pos.y << "," << bone_pos.z << ")"
                     << " spawnPos=(" << p.pos.x << "," << p.pos.y << "," << p.pos.z << ")"
                     << " dir=(" << p.dir.x << "," << p.dir.y << "," << p.dir.z << ")"
                     << " speed=" << glm::length(p.speed)
                     << std::endl;
          }

          // sanity check:
          //if (particles.size() < MAX_PARTICLES) // No need to check this every loop iteration. Already checked above.
          particles.push_back(p);
          ++spawned_this_update;
        }
      }
    }
  }

  float mspeed = 1.0f;

  for (ParticleList::iterator it = particles.begin(); it != particles.end();) {
    Particle &p = *it;
    p.speed += p.down * grav * dt - p.dir * deaccel * dt;

    if (slowdown>0) {
      mspeed = expf(-1.0f * slowdown * p.life);
    }
    else if (slowdown < 0.0f) {
      // Negative authored drag = DECELERATION in the live 1.12 client. Verified by apitrace capture of
      // the Anomalus feet smoke (MANAMISTBASE, drag=-0.1): the particles rise bright from the feet then
      // visibly slow and fade to ~8% opacity at the top of their travel (measured alpha 183->20 over the
      // last ~1.3 units of rise). WMV/Noggit's original `slowdown>0` gate dropped negative drag entirely,
      // so the smoke never decelerated -> rose too fast and stayed at full opacity at the top. exp(drag*
      // life) (drag<0 -> decay) reproduces the client's measured speed falloff (exp(-0.1*life) matched the
      // deceleration: ~4.0 units risen in the first 70% of life, only ~1.3 more in the last 30%).
      mspeed = expf(slowdown * p.life);
    }
    else {
      mspeed = 1.0f;
    }
    p.pos += p.speed * mspeed * dt;

    p.life += dt;
    float rlife = p.life / p.maxlife;

    // Flipbook: advance the texture cell sequentially over the particle's life (data-driven from
    // lifespanUVAnim/decayUVAnim). Overwrites the random tile picked at spawn for animated emitters
    // only; the draw path reads p.tile unchanged. Clamped so the index can never leave [0,cells-1]
    // (an out-of-range tile would break the whole draw loop via the size guard).
    if (_uv_animated)
    {
      int const range = _uv_seq_end - _uv_seq_start + 1;
      float const f = std::min(std::max(rlife, 0.0f), 0.99999f);
      int frame = static_cast<int>(f * static_cast<float>(range) * static_cast<float>(_uv_repeat));
      if (range > 0) { frame %= range; }
      int cell = _uv_seq_start + frame;
      if (cell < 0) { cell = 0; }
      if (static_cast<std::size_t>(cell) >= tiles.size() && !tiles.empty())
      {
        cell = static_cast<int>(tiles.size()) - 1;
      }
      p.tile = static_cast<unsigned int>(cell);
    }

    // calculate size and color based on lifetime
    p.size = lifeRamp<float>(rlife, mid, sizes[0], sizes[1], sizes[2]);
    p.color = lifeRamp<glm::vec4>(rlife, mid, colors[0], colors[1], colors[2]);

    if (classic)
    {
      if (!std::isfinite(p.size)
          || !finite_vec3(p.pos)
          || !finite_vec4(p.color)
          || glm::distance(p.pos, p.origin) > CLASSIC_PARTICLE_MAX_TRAVEL)
      {
        it = particles.erase(it);
        continue;
      }

      p.size = sane_classic_particle_size(p.size, 1.0f);
      p.color = glm::clamp(p.color, glm::vec4(0.0f), glm::vec4(1.0f));
    }

    // kill off old particles
    if (rlife >= 1.0f)
    {
      it = particles.erase (it);
    }
    else
    {
      ++it;
    }
  }

  if (classic_effect_debug_enabled() && classic && debug_update_log_count < 12)
  {
    ++debug_update_log_count;
    LogDebug << "Classic effect particle update model='" << model->file_key().stringRepr()
             << "' source=classic"
             << " dt=" << dt
             << " rate=" << debug_rate
             << " life=" << debug_life
             << " spawned=" << spawned_this_update
             << " before=" << particles_before_update
             << " after=" << particles.size()
             << " rem=" << rem
             << " texture=" << _texture_id
             << " type=" << type
             << " blend=" << blend
             << " enabled=1"
             << std::endl;
  }
}

void ParticleSystem::setup(int anim, int time, int animtime)
{
  manim = anim;
  mtime = time;
  manimtime = animtime;

  // Re-resolve the parent bone from its index every frame. A Bone* cached at construction goes stale (the
  // model's bones vector is rebuilt/reallocated after the emitter is built), which made parent->mat read
  // ZERO -> every particle spawned at the model origin emitting straight up. Re-pointing at the CURRENT
  // bones array picks up the animated bone matrix so particles attach at (and follow) the real bone.
  parent = particle_parent_bone(model, _bone_index);
}

void ParticleSystem::draw( glm::mat4x4 const& model_view
                         , OpenGL::Scoped::use_program& shader
                         , GLuint const& transform_vbo
                         , int instances_count
)
{
  // Nothing alive: skip entirely. Avoids per-frame zero-size buffer respecifications + degenerate
  // draw submissions for every idle emitter (needless churn for the driver's worker thread).
  if (particles.empty())
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  // setup blend mode
  float alpha_test = -1.f;

  // Bloom-mask alpha: every particle blend below uses glBlendFuncSeparate with the SAME RGB factors as
  // before (so the on-screen colour is byte-for-byte unchanged) plus a fixed ALPHA rule of
  // (GL_ZERO, GL_ONE_MINUS_SRC_ALPHA): dst_alpha' = dst_alpha * (1 - particle_coverage). The scene FBO's
  // alpha channel is the bloom mask, so a bright/opaque particle PULLS THE MASK DOWN where it covers.
  // Why: particles draw over the opaque pass without owning a mask, so a white-hot additive fire drawn
  // over the lava pit (mask ~1.0 = emissive) was inheriting that emissive flag and blooming through the
  // strong emissive path -> the Ironforge forge / firepit plumes blew out to a white screen-filling halo.
  // Now the fire core erases the lava's emissive mask under itself (so it stops over-blooming) while the
  // lava AROUND the flame keeps its mask and still glows. Faint edges (low coverage) barely touch it.
  #define NOGGIT_PARTICLE_BLOOM_ALPHA GL_ZERO, GL_ONE_MINUS_SRC_ALPHA
  switch (blend)
  {
  case 0:
    // Opaque: (ONE, ZERO) replaces dst RGB == the old GL_BLEND-disabled path; alpha still pulls the mask.
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_ONE, GL_ZERO, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 1:
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_ONE, GL_ZERO, NOGGIT_PARTICLE_BLOOM_ALPHA);
    alpha_test = 0.5f;
    break;
  case 2:
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 3:
    // NoAlphaAdd: pure additive, ignores src alpha. Canonical M2 blendingType 3 = ONE/ONE.
    // (Was SRC_COLOR/ONE -- only 2 emitters in 1 file use blend 3, asset-scan verified.)
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_ONE, GL_ONE, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 4:
    // Add: additive scaled by src alpha. The dominant glow/fire mode (10k+ emitters across
    // 2691 files all use blend 4 -- fire/flare/ember/lensflare). Asset evidence proves blend 4
    // is additive, NOT Mod: the RE §6.10 table is off-by-one (missing NoAlphaAdd@3).
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 5:
    // Mod: plain multiply. Canonical M2 blendingType 5 = DST_COLOR/ZERO (was lumped with Mod2x).
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_DST_COLOR, GL_ZERO, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 6:
    // Mod2x
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_DST_COLOR, GL_SRC_COLOR, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  case 7:
    gl.enable(GL_BLEND);
    gl.blendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, NOGGIT_PARTICLE_BLOOM_ALPHA);
    break;
  }
  #undef NOGGIT_PARTICLE_BLOOM_ALPHA

  // Depth-sort translucent particles back-to-front (client behaviour, RE handoff §6.10): order-dependent
  // blends -- Alpha(2) and Mod(5/6) -- must composite farthest-first or overlapping particles stack
  // wrong (the Timbermaw dust/fog reads far heavier than in-game without this). Additive (3/4/7) is
  // order-independent, so skip the sort for those. Sort by camera-space depth (most-negative view z =
  // farthest = drawn first).
  if ((blend == 2 || blend == 5 || blend == 6) && particles.size() > 1)
  {
    particles.sort([&model_view] (Particle const& a, Particle const& b)
    {
      return (model_view * glm::vec4(a.pos, 1.0f)).z < (model_view * glm::vec4(b.pos, 1.0f)).z;
    });
  }

  if (_texture_id < model->_textures.size())
  {
    auto& texture = model->_textures[_texture_id];
    if (!texture->finishedLoading())
    {
      try
      {
        texture->finishLoading();
      }
      catch (std::exception const& error)
      {
        if (classic_effect_debug_enabled() && classic)
        {
          LogDebug << "Classic effect particle texture load failed model='"
                   << model->file_key().stringRepr()
                   << "' source=classic"
                   << " texture=" << _texture_id
                   << " error='" << error.what() << "'"
                   << std::endl;
        }
      }
      catch (...)
      {
        if (classic_effect_debug_enabled() && classic)
        {
          LogDebug << "Classic effect particle texture load failed model='"
                   << model->file_key().stringRepr()
                   << "' source=classic"
                   << " texture=" << _texture_id
                   << " error='<unknown>'"
                   << std::endl;
        }
      }
    }

    texture->bind();
    shader.uniform("tex_index", texture->array_index());
  }
  else
  {
    shader.uniform("tex_index", 0);
  }

  glm::vec3 vRight(1, 0, 0);
  glm::vec3 vUp(0, 1, 0);

  // position stuff
  const float f = 1;//0.707106781f; // sqrt(2)/2
  glm::vec3 bv0 = glm::vec3(-f, +f, 0);
  glm::vec3 bv1 = glm::vec3(+f, +f, 0);

  std::vector<std::uint16_t> indices;
  std::vector<glm::vec3> vertices;
  std::vector<glm::vec3> offsets;
  std::vector<glm::vec4> colors_data;
  std::vector<glm::vec2> texcoords;

  std::uint16_t indice = 0;

  if (billboard) 
  {
    vRight = glm::normalize(glm::vec3(model_view[0]));
    vUp = glm::normalize(glm::vec3(model_view[1]));

    //vRight = glm::vec3(model_view[0][0], model_view[1][0], model_view[2][0]);
    //vUp = glm::vec3(model_view[0][1], model_view[1][1], model_view[2][1]); // Spherical billboarding
    //vUp = glm::vec3(0,1,0); // Cylindrical billboarding
  }

  auto add_quad_indices([] (std::vector<std::uint16_t>& indices, std::uint16_t& start)
  {
    indices.push_back(start + 0);
    indices.push_back(start + 1);
    indices.push_back(start + 2);

    indices.push_back(start + 2);
    indices.push_back(start + 3);
    indices.push_back(start + 0);

    start += 4;
  });

  /*
  * type:
  * 0   "normal" particle
  * 1  large quad from the particle's origin to its position (used in Moonwell water effects)
  * 2  seems to be the same as 0 (found some in the Deeprun Tram blinky-lights-sign thing)
  */
  if (type == 0 || type == 2) 
  {
    //! \todo figure out type 2 (deeprun tram subway sign)
    // - doesn't seem to be any different from 0 -_-
    // regular particles

    if (billboard)
    {
      // Per-particle quad rotation: authored params.spin (radians/sec) rotates the billboard around
      // its center over the particle's life -- positive = counterclockwise on screen (e.g. the arcane
      // elementals' feet smoke, spin=2.0). Spline emitters keep _spin as their emission-path travel
      // speed instead (the MC flamecircle), so no quad rotation for those.
      bool const quad_spin = (_spin != 0.0f) && _spline_points.empty();

      for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it)
      {
        if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
        {
          break;
        }

        const float size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;// / 2;
        if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec4(it->color))
        {
          continue;
        }

        glm::vec3 quad_right = vRight;
        glm::vec3 quad_up = vUp;
        if (quad_spin)
        {
          float const ang = _spin * it->life;
          float const c = std::cos(ang);
          float const s = std::sin(ang);
          quad_right = vRight * c + vUp * s;
          quad_up = vUp * c - vRight * s;
        }

        texcoords.push_back(tiles[it->tile].tc[0]);
        vertices.push_back(it->pos);
        offsets.push_back(-(quad_right + quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[1]);
        vertices.push_back(it->pos);
        offsets.push_back((quad_right - quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[2]);
        vertices.push_back(it->pos);
        offsets.push_back((quad_right + quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[3]);
        vertices.push_back(it->pos);
        offsets.push_back(-(quad_right - quad_up) * size);
        colors_data.push_back(it->color);

        add_quad_indices(indices, indice);
      }
    }
    else 
    {
      for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it) 
      {
        if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
        {
          break;
        }

        const float size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;
        if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec4(it->color))
        {
          continue;
        }

        texcoords.push_back(tiles[it->tile].tc[0]);
        vertices.push_back(it->pos + it->corners[0] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[1]);
        vertices.push_back(it->pos + it->corners[1] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[2]);
        vertices.push_back(it->pos + it->corners[2] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[3]);
        vertices.push_back(it->pos + it->corners[3] * size);
        colors_data.push_back(it->color);

        add_quad_indices(indices, indice);
      }
    }
  }  
  else if (type == 1) 
  { // Sphere particles
    // particles from origin to position
    /*
    bv0 = mbb * glm::vec3(0,-1.0f,0);
    bv1 = mbb * glm::vec3(0,+1.0f,0);


    bv0 = mbb * glm::vec3(-1.0f,0,0);
    bv1 = mbb * glm::vec3(1.0f,0,0);
    */

    for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it) 
    {
      if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
      {
        break;
      }

      const float size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;
      if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec3(it->origin) || !finite_vec4(it->color))
      {
        continue;
      }

      texcoords.push_back(tiles[it->tile].tc[0]);
      vertices.push_back(it->pos + bv0 * size);
      colors_data.push_back(it->color);

      texcoords.push_back(tiles[it->tile].tc[1]);
      vertices.push_back(it->pos + bv1 * size);
      colors_data.push_back(it->color);

      texcoords.push_back(tiles[it->tile].tc[2]);
      vertices.push_back(it->origin + bv1 * size);
      colors_data.push_back(it->color);

      texcoords.push_back(tiles[it->tile].tc[3]);
      vertices.push_back(it->origin + bv0 * size);
      colors_data.push_back(it->color);

      add_quad_indices(indices, indice);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec4>(_colors_vbo, colors_data, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoord_vbo, texcoords, GL_STREAM_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STREAM_DRAW);

  if (classic_effect_debug_enabled() && classic && debug_draw_log_count < 12)
  {
    float min_size = std::numeric_limits<float>::max();
    float max_size = 0.0f;
    glm::vec3 min_pos(std::numeric_limits<float>::max());
    glm::vec3 max_pos(std::numeric_limits<float>::lowest());
    for (Particle const& particle : particles)
    {
      min_size = std::min(min_size, particle.size);
      max_size = std::max(max_size, particle.size);
      min_pos = glm::min(min_pos, particle.pos);
      max_pos = glm::max(max_pos, particle.pos);
    }
    if (particles.empty())
    {
      min_size = 0.0f;
      min_pos = glm::vec3(0.0f);
      max_pos = glm::vec3(0.0f);
    }
    float first_life = 0.0f;
    float first_maxlife = 0.0f;
    glm::vec4 first_color(0.0f);
    if (!particles.empty())
    {
      first_life = particles.front().life;
      first_maxlife = particles.front().maxlife;
      first_color = particles.front().color;
    }

    ++debug_draw_log_count;
    LogDebug << "Classic effect particle draw model='" << model->file_key().stringRepr()
             << "' source=classic"
             << " instances=" << instances_count
             << " live=" << particles.size()
             << " vertices=" << vertices.size()
             << " indices=" << indices.size()
             << " texture=" << _texture_id
             << " textureCount=" << model->_textures.size()
             << " billboard=" << (billboard ? 1 : 0)
             << " type=" << type
             << " blend=" << blend
             << " rampSizes=[" << sizes[0] << ", " << sizes[1] << ", " << sizes[2] << "]"
             << " sizeRange=[" << min_size << ", " << max_size << "]"
             << " firstLife=" << first_life
             << " firstMaxLife=" << first_maxlife
             << " firstColor={" << first_color.r << ", " << first_color.g << ", " << first_color.b << ", " << first_color.a << "}"
             << " posMin={" << min_pos.x << ", " << min_pos.y << ", " << min_pos.z << "}"
             << " posMax={" << max_pos.x << ", " << max_pos.y << ", " << max_pos.z << "}"
             << std::endl;
  }

  shader.uniform("alpha_test", alpha_test);
  shader.uniform("billboard", (int)billboard);
  shader.uniform("particle_blend", static_cast<int>(blend)); // for blend-aware fog in the shader
  // Emitter flag 0x8 = particle SIZE scales with the model's scale. Trace-verified on Anomalus
  // (creature_template.scale 5): his aura flare (flags 0x29) draws at authored size x5 in the live
  // client, while his feet smoke (0x11) and rising stars (0x1) draw at authored size x1.
  shader.uniform("scale_with_instance", (flags & 0x8) ? 1 : 0);

  OpenGL::Scoped::vao_binder const _ (_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("position", 0);
  }
  if(billboard)
  {
    gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_offsets_vbo, offsets, GL_STREAM_DRAW);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const offset_binder (_offsets_vbo);
    shader.attrib("offset", 3, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("offset", 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const texcoord_binder (_texcoord_vbo);
    shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("uv", 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const colors_binder (_colors_vbo);
    shader.attrib("color", 4, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("color", 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (transform_vbo);
    shader.attrib("transform", 0, 1);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder (_indices_vbo);
  gl.drawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr, instances_count);

}

void ParticleSystem::upload()
{
  _vertex_array.upload();
  _buffers.upload();
  _uploaded = true;
}

void ParticleSystem::unload()
{
  _vertex_array.unload();
  _buffers.unload();
  _uploaded = false;
}

namespace
{
  //Generates the rotation matrix based on spread
  glm::mat4x4 CalcSpreadMatrix(float Spread1, float Spread2, float w, float l)
  {
    int i, j;
    float a[2], c[2], s[2];

    glm::mat4x4 SpreadMat = glm::mat4x4(1);

    a[0] = misc::randfloat(-Spread1, Spread1) / 2.0f;
    a[1] = misc::randfloat(-Spread2, Spread2) / 2.0f;

    /*SpreadMat.m[0][0]*=l;
    SpreadMat.m[1][1]*=l;
    SpreadMat.m[2][2]*=w;*/

    for (i = 0; i<2; ++i)
    {
      c[i] = cos(a[i]);
      s[i] = sin(a[i]);
    }

    {
        glm::mat4x4 Temp = glm::mat4x4(1);
        Temp[1][1] = c[0];
        Temp[2][1] = s[0];
        Temp[2][2] = c[0];
        Temp[1][2] = -s[0];

       SpreadMat = SpreadMat * Temp;
    }

    {
        glm::mat4x4 Temp = glm::mat4x4(1);
      Temp[0][0]= c[1];
      Temp[1][0]= s[1];
      Temp[1][1]= c[1];
      Temp[0][1]= -s[1];

      SpreadMat = SpreadMat*Temp;
    }

    float Size = std::abs(c[0])*l + std::abs(s[0])*w;
    for (i = 0; i<3; ++i)
      for (j = 0; j<3; j++)
        SpreadMat[i][j] = SpreadMat[i][j] * Size;

    return SpreadMat;
  }
}

Particle PlaneParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2)
{
  // Model Flags - *shrug* gotta write this down somewhere.
  // 0x1 =
  // 0x2 =
  // 0x4 =
  // 0x8 =
  // 0x10 =
  // 19 = 0x13 = blue ball in thunderfury = should be billboarded?

  // Particle Flags
  // 0x0  / 0    = Basilisk has no flags?
  // 0x1  / 1    = Pretty much everything I know of except Basilisks have this flag..  Billboard?
  // 0x2  / 2    =
  // 0x4  / 4    =
  // 0x8  / 8    =
  // 0x10  / 16  = Position Relative to bone pivot?
  // 0x20  / 32  =
  // 0x40  / 64  =
  // 0x80 / 128  =
  // 0x100 / 256  =
  // 0x200 / 512  =
  // 0x400 / 1024 =
  // 0x800 / 2048 =
  // 0x1000/ 4096 =
  // 0x0000/ 1593 = [1,8,16,32,512,1024]"Warp Storm" - aura type particle effect
  // 0x419 / 1049 = [1,8,16,1024] Forest Wind shoulders
  // 0x411 / 1041 = [1,16,1024] Halo
  // 0x000 / 541  = [1,4,8,16,512] Staff glow
  // 0x000 / 537 = "Warp Storm"
  // 0x31 / 49 = [1,16,32] particle moving up?
  // 0x00 / 41 = [1,8,32] Blood elf broom, dust spread out on the ground (X, Z axis)
  // 0x1D / 29 = [1,4,8,16] particle being static
  // 0x19 / 25 = [1,8,16] flame on weapon - move up/along the weapon
  // 17 = 0x11 = [1,16] glow on weapon - static, random direction.  - Aurastone Hammer
  // 1 = 0x1 = perdition blade
  // 4121 = water ele
  // 4097 = water elemental
  // 1041 = Transcendance Halo
  // 1039 = water ele

  Particle p{};
  p.speed = glm::vec3(0.0f);
  p.down = glm::vec3(0.0f, -1.0f, 0.0f);
  p.dir = glm::vec3(0.0f, 1.0f, 0.0f);
  p.color = glm::vec4(1.0f);
  p.size = 1.0f;
  p.maxlife = sys->classic ? 2.4f : 1.0f;

  //Spread Calculation
  auto mrot = sys->parent->mrot*CalcSpreadMatrix(spr, spr, 1.0f, 1.0f);

  if (!sys->_spline_points.empty()) { // Spline emitter (M2 EmitterType 3) -- e.g. the MC flamecircle ring
    // Emit along the closed spline loop; the emission point travels around it at _spin revolutions/sec
    // (so flamecircle's two emitters, spin 0.5 and -2, run the ring at their authored speeds/directions).
    // Particles then rise off the ring at EmissionSpeed, giving fire/smoke that runs the circle.
    std::size_t const n = sys->_spline_points.size();
    float u = sys->_spin * (static_cast<float>(animtime) / 1000.0f);
    u -= std::floor(u); // wrap to [0,1)
    float const fseg = u * static_cast<float>(n);
    std::size_t const i0 = static_cast<std::size_t>(fseg) % n;
    std::size_t const i1 = (i0 + 1) % n;
    float const t = fseg - std::floor(fseg);
    glm::vec3 local = glm::mix(sys->_spline_points[i0], sys->_spline_points[i1], t)
                    + glm::vec3(misc::randfloat(-l, l), 0.0f, misc::randfloat(-w, w));
    p.pos = sys->parent->mat * glm::vec4(local, 1.0f);

    glm::vec3 dir = mrot * glm::vec4(0, 1, 0, 0);
    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    p.down = glm::vec3(0, -1.0f, 0);
    p.speed = p.dir * spd * (1.0f + misc::randfloat(-var, var));
  }
  else if (sys->flags == 1041) { // Trans Halo
    p.pos = sys->parent->mat * (glm::vec4(sys->pos, 1) + glm::vec4(misc::randfloat(-l, l), 0, misc::randfloat(-w, w), 0));

    const float t = misc::randfloat(0.0f, 2.0f * glm::pi<float>());

    p.pos = glm::vec3(0.0f, sys->pos.y + 0.15f, sys->pos.z) + glm::vec3(cos(t) / 8, 0.0f, sin(t) / 8); // Need to manually correct for the halo - why?

    // var isn't being used, which is set to 1.0f,  whats the importance of this?
    // why does this set of values differ from other particles

    glm::vec3 dir(0.0f, 1.0f, 0.0f);
    p.dir = dir;

    p.speed = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f)) * spd * misc::randfloat(0, var);
  }
  else if (sys->flags == 25 && sys->parent->parent<1) { // Weapon Flame
    p.pos = sys->parent->pivot + (sys->pos + glm::vec3(misc::randfloat(-l, l), misc::randfloat(-l, l), misc::randfloat(-w, w)));
    glm::vec3 dir = mrot * glm::vec4(0.0f, 1.0f, 0.0f,0.0f);
    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    //glm::vec3 dir = sys->model->bones[sys->parent->parent].mrot * sys->parent->mrot * glm::vec3(0.0f, 1.0f, 0.0f);
    //p.speed = dir.normalize() * spd;

  }
  else if (sys->flags == 25 && sys->parent->parent > 0) { // Weapon with built-in Flame (Avenger lightsaber!)
    p.pos = sys->parent->mat * (glm::vec4(sys->pos, 1) + glm::vec4(misc::randfloat(-l, l), misc::randfloat(-l, l), misc::randfloat(-w, w), 0));
    glm::vec3 dir = glm::vec4(sys->parent->mat[1][0], sys->parent->mat[1][1], sys->parent->mat [1][2],0.0f) + glm::vec4(0.0f, 1.0f, 0.0f,0.0f);
    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    p.speed = p.dir * spd * misc::randfloat(0, var * 2);

  }
  else if (sys->flags == 17 && sys->parent->parent<1) { // Weapon Glow
    p.pos = sys->parent->pivot + (sys->pos + glm::vec3(misc::randfloat(-l, l), misc::randfloat(-l, l), misc::randfloat(-w, w)));
    glm::vec3 dir = mrot * glm::vec4(0, 1, 0,0);
    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));

  }
  else {
    // CLIENT-FAITHFUL plane emission (RE'd from 1.12 CParticleEmitter2 plane CreateParticle,
    // FUN_007b8890): spawn on the authored rect -- model X in +-areaLength/2 (w), model Y in
    // +-areaWidth/2 (l) -- and emit with a velocity TILTED from straight-up by theta in
    // +-VerticalRange (spr) at azimuth phi in +-HorizontalRange (spr2). The old WMV spread-matrix
    // used the vertical range for BOTH axes and ignored the horizontal range entirely.
    glm::vec3 local = sys->pos + glm::vec3(misc::randfloat(-w, w), 0, misc::randfloat(-l, l));
    p.pos = sys->parent->mat * glm::vec4(local, 1);

    float const theta = misc::randfloat(-spr, spr);    // tilt from the emitter's up axis
    float const phi = misc::randfloat(-spr2, spr2);    // azimuth around the up axis

    // client model space is z-up: dir = (cos(phi)sin(theta), sin(phi)sin(theta), cos(theta));
    // converted to noggit's y-up space like fixCoordSystem.
    glm::vec3 const tilted(std::cos(phi) * std::sin(theta),
                           std::cos(theta),
                           -std::sin(phi) * std::sin(theta));
    glm::vec3 dir = sys->parent->mrot * glm::vec4(tilted, 0);

    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    p.down = glm::vec3(0, -1.0f, 0);
    p.speed = p.dir * spd * (1.0f + misc::randfloat(-var, var));
  }

  if (!sys->billboard)  {
    p.corners[0] = mrot * glm::vec4(-1, 0, +1, 0);
    p.corners[1] = mrot * glm::vec4(+1, 0, +1, 0);
    p.corners[2] = mrot * glm::vec4(+1, 0, -1, 0);
    p.corners[3] = mrot * glm::vec4(-1, 0, -1, 0);
  }

  p.life = 0;
  p.maxlife = sane_positive_particle_value(sys->lifespan.getValue(anim, time, animtime),
                                           sys->classic ? 2.4f : 1.0f);
  if (sys->classic)
  {
    p.maxlife = std::min(p.maxlife, CLASSIC_PARTICLE_MAX_LIFESPAN);
  }

  p.origin = p.pos;

  p.tile = misc::randint(0, sys->rows*sys->cols - 1);
  return p;
}

Particle SphereParticleEmitter::newParticle(ParticleSystem* sys, int anim, int time, int animtime, float w, float l, float spd, float var, float spr, float spr2)
{
  Particle p{};
  p.speed = glm::vec3(0.0f);
  p.down = glm::vec3(0.0f, -1.0f, 0.0f);
  p.dir = glm::vec3(0.0f, 1.0f, 0.0f);
  p.color = glm::vec4(1.0f);
  p.size = 1.0f;
  p.maxlife = sys->classic ? 2.4f : 1.0f;
  glm::vec3 dir(0.0f, 1.0f, 0.0f);
  // Spawn between the inner (l = EmissionAreaWidth/2) and outer (w = EmissionAreaLength/2) radius, so
  // particles appear ACROSS the authored emission ring/sphere -- the "max range" of the area -- instead
  // of a fixed unit radius (the old `randfloat(0,1)` ignored EmissionArea entirely). For a shell where
  // inner==outer (e.g. the arcane-elemental smoke ring, area 2.08 -> r 1.04) they spawn on the surface
  // and then converge inward via the negative EmissionSpeed, matching the in-game "spawn at the edge,
  // pull into the model" look. (Faith-Halo branch already used w/l; this fixes the general branch.)
  float const r_inner = std::min(std::fabs(w), std::fabs(l));
  float const r_outer = std::max(std::fabs(w), std::fabs(l));
  float radius = (r_outer > 0.0f) ? (r_inner + (r_outer - r_inner) * misc::randfloat(0, 1))
                                  : misc::randfloat(0, 1);

  // Old method
  //float t = misc::randfloat(0,2*math::constants::pi);

  // New
  // Spread should never be zero for sphere particles ?
  math::radians t (0);
  if (spr == 0)
    t._ = misc::randfloat(-glm::pi<float>(), glm::pi<float>());
  else
    t._ = misc::randfloat(-spr, spr);

  //Spread Calculation
  auto mrot =  sys->parent->mrot*CalcSpreadMatrix(spr * 2, spr2 * 2, w, l);

  // New
  // Length should never technically be zero ?
  //if (l==0)
  //  l = w;

  // New method
  // glm::vec3 bdir(w*math::cos(t), 0.0f, l*math::sin(t));
  // --

  //! \todo fix shpere emitters to work properly
  /* // Old Method
  //glm::vec3 bdir(l*math::cos(t), 0, w*math::sin(t));
  //glm::vec3 bdir(0, w*math::cos(t), l*math::sin(t));


  float theta_range = sys->spread.getValue(anim, time, animtime);
  float theta = -0.5f* theta_range + misc::randfloat(0, theta_range);
  glm::vec3 bdir(0, l*math::cos(theta), w*math::sin(theta));

  float phi_range = sys->lat.getValue(anim, time, animtime);
  float phi = misc::randfloat(0, phi_range);
  rotate(0,0, &bdir.z, &bdir.x, phi);
  */

  if (sys->flags == 57 || sys->flags == 313) { // Faith Halo
    glm::vec3 bdir(w*glm::cos(t._)*1.6f, 0.0f, l*glm::sin(t._)*1.6f);

    p.pos = sys->pos + bdir;
    p.pos = sys->parent->mat * glm::vec4(p.pos, 1);

    if (glm::length(bdir) * glm::length(bdir) == 0)
      p.speed = glm::vec3(0, 0, 0);
    else {
      dir = sys->parent->mrot * glm::vec4((glm::normalize(bdir)),0);//mrot * glm::vec3(0, 1.0f,0);
      p.speed = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f)) * spd * (1.0f + misc::randfloat(-var, var));   // ?
    }

  }
  else {
    // CLIENT-FAITHFUL sphere emission (RE'd from 1.12 CParticleEmitter2 sphere CreateParticle,
    // FUN_007b8d70): pick elevation theta in +-VerticalRange and azimuth phi in +-HorizontalRange,
    // spawn at (radial unit vector x radius), velocity = the SAME radial direction x EmissionSpeed
    // (negative speed = inward -- e.g. the arcane elementals' feet smoke ring converges up the legs).
    // Radius: for sphere emitters areal/areaw are the MIN/MAX radius -- but update() halves them for
    // plane semantics (rect half-extents), so undo the halving here. The old WMV-derived code emitted
    // along a spread-matrix-rotated Y axis at HALF the authored radius, which put the Anomalus smoke
    // at his feet instead of on the authored 2.08-radius ring around him.
    float const r_min = 2.0f * std::min(std::fabs(w), std::fabs(l));
    float const r_max = 2.0f * std::max(std::fabs(w), std::fabs(l));
    float const r = r_min + (r_max - r_min) * misc::randfloat(0, 1);

    float const theta = misc::randfloat(-spr, spr);   // elevation from the horizontal plane
    float const phi = misc::randfloat(-spr2, spr2);   // azimuth around the emitter axis

    // radial unit direction in noggit space (up = +Y; model z-up converted like fixCoordSystem)
    glm::vec3 const radial(std::cos(theta) * std::cos(phi),
                           std::sin(theta),
                           -std::cos(theta) * std::sin(phi));

    p.pos = sys->parent->mat * glm::vec4(sys->pos + radial * r, 1);

    if (sys->flags & 0x100)
    {
      dir = sys->parent->mrot * glm::vec4(0, 1, 0, 0);
    }
    else
    {
      dir = sys->parent->mrot * glm::vec4(radial, 0);
    }

    p.speed = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f)) * spd * (1.0f + misc::randfloat(-var, var));
  }

  p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));//mrot * glm::vec3(0, 1.0f,0);
  p.down = glm::vec3(0, -1.0f, 0);

  p.life = 0;
  p.maxlife = sane_positive_particle_value(sys->lifespan.getValue(anim, time, animtime),
                                           sys->classic ? 2.4f : 1.0f);
  if (sys->classic)
  {
    p.maxlife = std::min(p.maxlife, CLASSIC_PARTICLE_MAX_LIFESPAN);
  }

  p.origin = p.pos;

  p.tile = misc::randint(0, sys->rows*sys->cols - 1);
  return p;
}

RibbonEmitter::RibbonEmitter(Model* model_
                             , const BlizzardArchive::ClientFile &f
                             , ModelRibbonEmitterDef const& mta
                             , int *globals
                             , Noggit::NoggitRenderContext context)
  : model (model_)
  , color (mta.color, f, globals)
  , opacity (mta.opacity, f, globals)
  , above (mta.above, f, globals)
  , below (mta.below, f, globals)
  , parent (&model->bones[mta.bone])
  , pos (fixCoordSystem(mta.pos))
  , seglen (mta.length)
  , length (mta.res * seglen)
   // just use the first texture for now; most models I've checked only had one
  , tpos (fixCoordSystem(mta.pos))
   //! \todo  figure out actual correct way to calculate length
   // in BFD, res is 60 and len is 0.6, the trails are very short (too long here)
   // in CoT, res and len are like 10 but the trails are supposed to be much longer (too short here)
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);

   // create first segment
  segs.emplace_back(tpos, 0);

}

RibbonEmitter::RibbonEmitter(Model* model_
                             , const BlizzardArchive::ClientFile &f
                             , ClassicModelRibbonEmitterDef const& mta
                             , int *globals
                             , Noggit::NoggitRenderContext context)
  : model (model_)
  , color (mta.color, f, globals)
  , opacity (mta.opacity, f, globals)
  , above (mta.above, f, globals)
  , below (mta.below, f, globals)
  // Classic effect/creature trails reference a bone index; clamp to a valid bone (mirrors the
  // particle-emitter bone guard) so a stray index can't read past the bones vector and crash.
  , parent (&model->bones[(mta.bone >= 0 && static_cast<std::size_t>(mta.bone) < model->bones.size()) ? mta.bone : 0])
  , pos (fixCoordSystem(mta.pos))
  , seglen (mta.length)
  , length (mta.res * seglen)
  , tpos (fixCoordSystem(mta.pos))
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);

  segs.emplace_back(tpos, 0);
}

RibbonEmitter::RibbonEmitter(RibbonEmitter const& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , seglen(other.seglen)
  , length(other.length)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , segs(other.segs)
  , _context(other._context)
{

}

RibbonEmitter::RibbonEmitter(RibbonEmitter&& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , seglen(other.seglen)
  , length(other.length)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , segs(other.segs)
  , _context(other._context)
{

}

void RibbonEmitter::setup(int anim, int time, int animtime)
{
  glm::vec3 ntpos = parent->mat * glm::vec4(pos, 1);
  glm::vec3 ntup = parent->mat * (glm::vec4(pos, 1) + glm::vec4(0, 0, 1, 0));
  ntup -= ntpos;
  ntup = glm::normalize(ntup);
  float dlen = glm::distance(ntpos, tpos);

  manim = anim;
  mtime = time;

  // move first segment
  RibbonSegment &first = *segs.begin();
  if (first.len > seglen) {
    // add new segment
    first.back = glm::normalize((tpos - ntpos));
    first.len0 = first.len;
    RibbonSegment newseg (ntpos, dlen);
    newseg.up = ntup;
    segs.push_front(newseg);
  }
  else {
    first.up = ntup;
    first.pos = ntpos;
    first.len += dlen;
  }

  // kill stuff from the end TODO: occasional crashes here
  float l = 0;
  bool erasemode = false;
  for (std::list<RibbonSegment>::iterator it = segs.begin(); it != segs.end();) {
    if (!erasemode) {
      l += it->len;
      if (l > length) {
        it->len = l - length;
        erasemode = true;
      }
    }
    else {
      segs.erase(it);
    }
    ++it;
  }

  tpos = ntpos;
  auto col = color.getValue(anim, time, animtime);
  tcolor = glm::vec4(col.x,col.y,col.z, opacity.getValue(anim, time, animtime));

  tabove = above.getValue(anim, time, animtime);
  tbelow = below.getValue(anim, time, animtime);
}

void RibbonEmitter::draw( OpenGL::Scoped::use_program& shader
                        , GLuint const& transform_vbo
                        , int instances_count
                        )
{
  if (!_uploaded)
  {
    upload();
  }

  std::vector<std::uint16_t> indices;
  std::vector<glm::vec3> vertices;
  std::vector<glm::vec2> texcoords;

  if (!_texture_ids.empty() && _texture_ids[0] < model->_textures.size())
  {
    model->_textures[_texture_ids[0]]->bind();
    shader.uniform("tex_index", model->_textures[_texture_ids[0]]->array_index());
  }
  else
  {
    shader.uniform("tex_index", 0);
  }

  gl.enable(GL_BLEND);
  
  shader.uniform("color", tcolor);

  std::uint16_t indice = 0;
  auto add_quad_indices([] (std::vector<std::uint16_t>& indices, std::uint16_t& start)
  {
    indices.push_back(start + 0);
    indices.push_back(start + 1);
    indices.push_back(start + 2);

    indices.push_back(start + 2);
    indices.push_back(start + 1);
    indices.push_back(start + 3);

    start += 2;
  });

  std::list<RibbonSegment>::iterator it = segs.begin();
  float l = 0;
  for (; it != segs.end(); ++it) 
  {
    float u = l / length;

    texcoords.emplace_back(u, 0);
    vertices.push_back(it->pos + tabove * it->up);
    texcoords.emplace_back(u, 1);
    vertices.push_back(it->pos - tbelow * it->up);

    l += it->len;

    add_quad_indices(indices, indice);
  }

  if (segs.size() > 1) 
  {
    // last segment...?
    --it;
    texcoords.emplace_back(1, 0);
    vertices.push_back(it->pos + tabove * it->up + (it->len / it->len0) * it->back);
    texcoords.emplace_back(1, 1);
    vertices.push_back(it->pos - tbelow * it->up + (it->len / it->len0) * it->back);
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STREAM_DRAW);
  gl.bufferData<GL_ARRAY_BUFFER, glm::vec2>(_texcoord_vbo, texcoords, GL_STREAM_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STREAM_DRAW);

  OpenGL::Scoped::vao_binder const _(_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const vertices_binder(_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("position", 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const texcoord_binder(_texcoord_vbo);
    shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, 0, 0);
    shader.attrib_divisor("uv", 0);
  }
  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(transform_vbo);
    shader.attrib("transform", 0, 1);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder(_indices_vbo);
  gl.drawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr, instances_count);
}

void RibbonEmitter::upload()
{
  _vertex_array.upload();
  _buffers.upload();
  _uploaded = true;
}

void RibbonEmitter::unload()
{
  _vertex_array.unload();
  _buffers.unload();
  _uploaded = false;
}
