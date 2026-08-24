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
#include <cctype>
#include <cmath>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <cstring>
#include <functional>
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

  // Client twinkle noise table (CParticleEmitter2 §5): the 1.12 client fills DAT_00cf58f0 once at load
  // with 128 uniform-random floats in [0,1) and every twinkling sprite indexes it by
  // (floor(twinkleSpeed*age)+slot)&0x7f. The exact values are irrelevant (they're just white noise);
  // we build a stable deterministic table (fixed-seed LCG) so the shimmer is identical run to run.
  std::array<float, 128> const& twinkle_noise_table()
  {
    static std::array<float, 128> const table = []
    {
      std::array<float, 128> t{};
      std::uint32_t s = 0x1234567u;
      for (float& v : t)
      {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>(s >> 8) * (1.0f / 16777216.0f); // [0,1)
      }
      return t;
    }();
    return table;
  }

  bool particle_range_fits(BlizzardArchive::ClientFile const& file,
                           std::uint32_t offset,
                           std::uint32_t count,
                           std::size_t element_size)
  {
    return !count || (offset < file.getSize() && count <= (file.getSize() - offset) / element_size);
  }

  // Read one M2PartTrack (keyed times + values) as a normalised-life curve. Times are fixed16 over the
  // particle's life (0..32767), the same encoding the flipbook cell track uses.
  //
  // A track is only accepted when it spans real time. Default-filled tracks (every timestamp 0) are
  // common in the wotlk data and carry no authoring; taking one at face value would pin every particle
  // to its last value -- black, or size zero -- so those fall through to the three-key ramp instead.
  template<typename Value, typename Decode>
  bool read_particle_life_track(BlizzardArchive::ClientFile const& file,
                                FakeAnimationBlock const& track,
                                std::size_t raw_value_size,
                                std::vector<float>& times_out,
                                std::vector<Value>& values_out,
                                Decode decode)
  {
    times_out.clear();
    values_out.clear();

    if (track.nKeys < 2 || track.nKeys != track.nTimes || track.nKeys >= 4096)
    {
      return false;
    }
    if (!particle_range_fits(file, track.ofsTimes, track.nTimes, sizeof(std::uint16_t))
        || !particle_range_fits(file, track.ofsKeys, track.nKeys, raw_value_size))
    {
      return false;
    }

    times_out.reserve(track.nKeys);
    values_out.reserve(track.nKeys);

    for (std::uint32_t i = 0; i < track.nKeys; ++i)
    {
      auto const t = *reinterpret_cast<std::uint16_t const*>(
        file.getBuffer() + track.ofsTimes + i * sizeof(std::uint16_t));
      times_out.push_back(std::clamp(static_cast<float>(t) / 32767.0f, 0.0f, 1.0f));
      values_out.push_back(decode(file.getBuffer() + track.ofsKeys + i * raw_value_size));
    }

    if (!(times_out.back() > times_out.front()))
    {
      times_out.clear();
      values_out.clear();
      return false; // no time extent -> default-filled, not authored
    }
    return true;
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
        // The scale track values ARE the authored sizes. This used to be multiplied by params.scales[i],
        // which the legacy struct mislabelled as a per-key size scale -- those three floats are really
        // {twinkleScaleMin, twinkleScaleMax, burstMultiplier}. Harmless on the 88% of emitters authoring
        // twinkleScale (1,1), but it shrank the start size 5x on emitters authoring e.g. (0.2, 1.0).
        sizes[i] = std::max(std::abs(raw_sizes[i].x), std::abs(raw_sizes[i].y));
      }
    }
    else if (params.sizes.nKeys >= 3
             && particle_range_fits(file, params.sizes.ofsKeys, 3, sizeof(float)))
    {
      for (std::size_t i = 0; i < 3; ++i)
      {
        float const raw_size = *reinterpret_cast<float const*>(file.getBuffer() + params.sizes.ofsKeys + i * sizeof(float));
        sizes[i] = std::abs(raw_size);
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

      // Client size ramp = scalesValues DIRECTLY (CParticleEmitter2 render fill FUN_007b2a50); burstMultiplier
      // is NOT applied to size in 1.12 (nor to spawn count -- 14.5% of Turtle emitters author burst=0.0 yet
      // emit). Dropping the x burst removes a minor non-client size artifact on the 1.1% authoring burst!=1. (12.15)
      float const size = params.scalesValues[i];
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

// Evaluate a keyed life track at normalised life. Keys are few (2-16 in the wotlk data), so a linear
// walk beats anything cleverer. Outside the authored range the curve holds its end value, which is
// what the client does -- it does not extrapolate.
template<typename T>
T lifeTrack(std::vector<float> const& times, std::vector<T> const& values, float life)
{
  if (life <= times.front()) return values.front();
  if (life >= times.back()) return values.back();

  std::size_t hi = 1;
  while (hi + 1 < times.size() && times[hi] < life)
  {
    ++hi;
  }

  float const span = times[hi] - times[hi - 1];
  float const t = span > 1e-6f ? (life - times[hi - 1]) / span : 0.0f;
  return math::interpolation::linear(t, values[hi - 1], values[hi]);
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
  , slowdown (std::isfinite(mta.p.drag) ? mta.p.drag : 0.0f)
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

  // ...and, where the emitter actually authors keyed tracks, the full curves. These take precedence
  // over the three-key ramp above, which stays as the fallback for tracks that are too short or are
  // default-filled. Each of the three is independent -- an emitter may key its colour while leaving
  // size on the ramp.
  {
    auto const normalize_color_component = [](float value)
    {
      if (!std::isfinite(value))
      {
        return 1.0f;
      }
      return std::clamp(value > 1.0f ? value / 255.0f : value, 0.0f, 1.0f);
    };

    read_particle_life_track<glm::vec3>(f, mta.p.colors, sizeof(glm::vec3), _color_times, _color_values,
      [&](auto const* raw)
      {
        glm::vec3 c;
        std::memcpy(&c, raw, sizeof(c));
        return glm::vec3(normalize_color_component(c.x)
                        ,normalize_color_component(c.y)
                        ,normalize_color_component(c.z));
      });

    read_particle_life_track<float>(f, mta.p.opacity, sizeof(std::int16_t), _alpha_times, _alpha_values,
      [](auto const* raw)
      {
        std::int16_t v;
        std::memcpy(&v, raw, sizeof(v));
        return std::clamp(static_cast<float>(v) / 32767.0f, 0.0f, 1.0f);
      });

    // Size keys are C2Vector; the ramp reader takes the larger axis and so does this. NOT multiplied by
    // params.scaleVary / twinkleScale -- those are separate effects, see read_particle_life_ramp.
    read_particle_life_track<float>(f, mta.p.sizes, sizeof(glm::vec2), _size_times, _size_values,
      [](auto const* raw)
      {
        glm::vec2 s;
        std::memcpy(&s, raw, sizeof(s));
        if (!std::isfinite(s.x) || !std::isfinite(s.y))
        {
          return 1.0f;
        }
        return std::max(std::abs(s.x), std::abs(s.y));
      });
  }

  //transform = mta.flags & 1024;

  // Spin / wind / twinkle / tail. These are the same authored fields the classic ctor already reads --
  // the WotLK path simply never read them, because the legacy struct buried them under unk*/rotation
  // names. Measured in the 3.3.5a client: spin is authored on real emitters (SteamGeyser 1-3 rad/s),
  // twinkleScale min!=max on ~3000 emitters, and windVector on only 2 (so wind is near-dead here, but
  // it costs nothing to read correctly). Guard non-finite authored floats exactly as classic does.
  auto const sane = [](float v, float fb) { return std::isfinite(v) ? v : fb; };
  _spin = sane(mta.p.spin, 0.f);
  _wind = fixCoordSystem(glm::vec3(sane(mta.p.windVector.x, 0.f),
                                   sane(mta.p.windVector.y, 0.f),
                                   sane(mta.p.windVector.z, 0.f)));
  _wind_time = std::max(0.0f, sane(mta.p.windTime, 0.f));
  _twinkle_speed = sane(mta.p.twinkleSpeed, 0.f);
  _twinkle_percent = sane(mta.p.twinklePercent, 1.f);
  _twinkle_scale_min = sane(mta.p.twinkleScaleMin, 1.f);
  _twinkle_scale_max = sane(mta.p.twinkleScaleMax, 1.f);
  // NOT derived from flag 0x8000 here, unlike the classic ctor. That mapping is 1.12's: in 3.3.5a the
  // emitter setup (client FUN_00832ea0) remaps every authored M2 flag to a different runtime bit, and
  // authored 0x8000 is the one flag that CLEARS a runtime bit (`&= ~0x1`) rather than setting one --
  // it has nothing to do with spin. Whatever drives the alternating spin sign in 3.3.5a has not been
  // pinned yet, so leave it off rather than apply the 1.12 meaning to WotLK data.
  // See RE_notes/12_particle_emitter_flags.md for the full authored->runtime remap table.
  _spin_alternate = false;
  _tail_length = std::max(0.0f, sane(mta.p.tailLength, 0.f));
  // Likewise 0x400: the 3.3.5a setup never tests it. Harmless either way -- headOrTail is 0 on all
  // 26030 emitters in this client (checklist 12.8), so _tail_length is 0 and the clamp is inert.
  _tail_clamp_age = (mta.flags & 0x400) != 0;

  // Flipbook cell animation. Where classic stores a [startCell, endCell, repeat] triple, WotLK keys the
  // cell index over the particle's life in an M2PartTrack: times are fixed16 (0..32767 == 0..1 of life)
  // and values are uint16 cell indices into the rows*cols sheet. Authored on 13004 of 26030 emitters in
  // the 3.3.5a client, 3333 of which actually vary (e.g. CloudSwampGas 8x8, times [0,16384,16384,32767]
  // cells [6,33,34,59] -- cells 6->33 over the first half, then 34->59). The duplicated middle timestamp
  // is what makes the lifespan->decay hand-off an instant jump. 4 keys must cover ~26 cells, so the
  // client interpolates between keys rather than stepping; we do the same and truncate to a cell.
  {
    auto const& cell_track = mta.p.headCellTrack;
    if (cell_track.nKeys >= 2
        && cell_track.nKeys == cell_track.nTimes
        && cell_track.nKeys < 4096
        && particle_range_fits(f, cell_track.ofsKeys, cell_track.nKeys, sizeof(std::uint16_t))
        && particle_range_fits(f, cell_track.ofsTimes, cell_track.nTimes, sizeof(std::uint16_t)))
    {
      int const cell_count = std::max(1, rows * cols);
      std::vector<float> times;
      std::vector<int> cells;
      times.reserve(cell_track.nKeys);
      cells.reserve(cell_track.nKeys);

      for (std::uint32_t i = 0; i < cell_track.nKeys; ++i)
      {
        auto const t = *reinterpret_cast<std::uint16_t const*>(
          f.getBuffer() + cell_track.ofsTimes + i * sizeof(std::uint16_t));
        auto const c = *reinterpret_cast<std::uint16_t const*>(
          f.getBuffer() + cell_track.ofsKeys + i * sizeof(std::uint16_t));
        times.push_back(std::clamp(static_cast<float>(t) / 32767.0f, 0.0f, 1.0f));
        cells.push_back(std::min(static_cast<int>(c), cell_count - 1));
      }

      // Only animate when the sheet has more than one cell AND the track actually moves. A constant
      // track is NOT "always show this cell": of the 115 multi-cell emitters with one, 111 are
      // [0,0,0,0] on a 64-cell sheet (CelestialHorse, ColdWraith, FacelessOne...) -- a default-filled
      // track, not authoring. Those keep the random tile they had before, which is the variety the
      // sheet exists for; pinning cell 0 would freeze all of them on the top-left cell.
      bool const varies = std::adjacent_find(cells.begin(), cells.end(), std::not_equal_to<>()) != cells.end();
      if (cell_count > 1 && varies)
      {
        _cell_times = std::move(times);
        _cell_values = std::move(cells);
        _uv_animated = true;
      }
    }
  }

  // Spline emitter (M2 EmitterType 3). The emission path itself is version-agnostic -- newParticle walks
  // _spline_points whenever it is non-empty -- only the point LOADING was classic-only, so wotlk type-3
  // emitters silently degraded to a plane. 136 emitters in the 3.3.5a client author splines (ring
  // effects: FlameCircleEffect, Fel_FlameCircleEffect, Circle_of_Renewal, WellOfSouls), and within any
  // sampled subset that count matches the emitterType==3 count exactly -- which is what validates these
  // struct offsets. Same read as the classic ctor, bounds-checked.
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

  // init tiles
  for (int i = 0; i<rows*cols; ++i) {
    TexCoordSet tc;
    initTile(tc.tc, i);
    tiles.push_back(tc);
  }
  _particle_color_index = mta.ParticleColor;
  _authored_colors = colors;
  read_geometry_model_filename(f, mta.nModelFileName, mta.ofsModelFileName);
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
  // ParticleType (file 0x2c) IS the Head/Tail selector: 0=Head, 1=Tail, 2=Both (RE'd from the client
  // M2 loader FUN_0070ebd0). The classic ctor used to hardcode type(0), forcing every classic particle
  // to render as a Head billboard -- so Tail emitters (e.g. a core hound's dripping drool) never
  // stretched. The wotlk ctor already read mta.ParticleType; match it here.
  , order(mta.ParticleType > 0 ? -1 : 0)
  , type(mta.ParticleType)
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

  // Wind / twinkle / spin-sign (client-exact, CParticleEmitter2 RE). Guard non-finite authored floats.
  auto const sane = [](float v, float fb) { return std::isfinite(v) ? v : fb; };
  _wind = fixCoordSystem(glm::vec3(sane(mta.p.windVector.x, 0.f),
                                   sane(mta.p.windVector.y, 0.f),
                                   sane(mta.p.windVector.z, 0.f)));
  _wind_time = std::max(0.0f, sane(mta.p.windTime, 0.f));
  _twinkle_speed = sane(mta.p.twinkleSpeed, 0.f);
  _twinkle_percent = sane(mta.p.twinklePercent, 1.f);
  _twinkle_scale_min = sane(mta.p.twinkleScaleMin, 1.f);
  _twinkle_scale_max = sane(mta.p.twinkleScaleMax, 1.f);
  _spin_alternate = (mta.flags & 0x8000) != 0;

  // Tail streak (Head/Tail=1/2): tailLength (file 0x17c) = streak length as a fraction of velocity.
  // Emitter flag 0x400 = clamp the tail to the particle's age (young particles have a short growing tail).
  _tail_length = std::max(0.0f, sane(mta.p.tailLength, 0.f));
  _tail_clamp_age = (mta.flags & 0x400) != 0;

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

  // REMOVED (trace-verified, RE_notes/18): the old "tiny-particle visibility floor" scaled any
  // sub-0.06-unit particle UP and its alpha DOWN -- but the client renders authored sizes verbatim:
  // the inn candle flames are 0.02-0.05 unit quads (5-20 px on screen) drawn at FULL authored alpha,
  // stacking additively into small bright yellow licks. The floor made ours bigger AND dimmer -- the
  // opposite. Authored data passes through untouched now.

  // Editor vs in-game: the editor's brighter, un-fogged scene makes overlapping alpha-blended smoke/
  // dust read as near-opaque and crisp, where in the dark, foggy game the same particles are soft and
  // see-through. Scale alpha-blend opacity down so it reads translucent like in-game. Large-area
  // ambient fog (Timbermaw dust ~8-12u) overlaps hardest -> strongest cut; smaller volumetric smoke
  // (energy-elemental feet mist etc.) still gets a meaningful cut so it stops looking too clear/solid.
  // Additive fire/glow (blend 3/4) is left alone. Data-driven (blend + emission area), not a per-model hack.
  if (blend == 2)
  {
    float const area = std::max(areal.getValue(0, 0, 0), areaw.getValue(0, 0, 0));
    float const soften = (std::isfinite(area) && area > 5.0f) ? 0.2f : 0.5f;
    for (glm::vec4& ramp_color : colors)
    {
      ramp_color.a *= soften;
    }
  }

  for (int i = 0; i < rows * cols; ++i)
  {
    TexCoordSet tc;
    initTile(tc.tc, i);
    tiles.push_back(tc);
  }
  // classic defs carry no ParticleColor index (wotlk-only field); keep -1 = no recolor
  _authored_colors = colors;
  read_geometry_model_filename(f, mta.nModelFileName, mta.ofsModelFileName);
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
  , _particle_color_index(other._particle_color_index)
  , _authored_colors(other._authored_colors)
  , _geometry_model_path(other._geometry_model_path)
  , sizes(other.sizes)
  , mid(other.mid)
  , slowdown(other.slowdown)
  , _spin(other._spin)
  , _spline_points(other._spline_points)
  , _wind(other._wind)
  , _wind_time(other._wind_time)
  , _twinkle_speed(other._twinkle_speed)
  , _twinkle_percent(other._twinkle_percent)
  , _twinkle_scale_min(other._twinkle_scale_min)
  , _twinkle_scale_max(other._twinkle_scale_max)
  , _spin_alternate(other._spin_alternate)
  , _spawn_seq(other._spawn_seq)
  , _tail_length(other._tail_length)
  , _tail_clamp_age(other._tail_clamp_age)
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
  , _cell_times(other._cell_times)
  , _cell_values(other._cell_values)
  , _color_times(other._color_times)
  , _color_values(other._color_values)
  , _alpha_times(other._alpha_times)
  , _alpha_values(other._alpha_values)
  , _size_times(other._size_times)
  , _size_values(other._size_values)
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
  , _wind(other._wind)
  , _wind_time(other._wind_time)
  , _twinkle_speed(other._twinkle_speed)
  , _twinkle_percent(other._twinkle_percent)
  , _twinkle_scale_min(other._twinkle_scale_min)
  , _twinkle_scale_max(other._twinkle_scale_max)
  , _spin_alternate(other._spin_alternate)
  , _spawn_seq(other._spawn_seq)
  , _tail_length(other._tail_length)
  , _tail_clamp_age(other._tail_clamp_age)
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
  , _cell_times(other._cell_times)
  , _cell_values(other._cell_values)
  , _color_times(other._color_times)
  , _color_values(other._color_values)
  , _alpha_times(other._alpha_times)
  , _alpha_values(other._alpha_values)
  , _size_times(other._size_times)
  , _size_values(other._size_values)
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

  // Flip V so the tile's TOP row (min v) lands on the +up screen corners. The 1.12 client draws every
  // FFP particle sprite with the texture's top at the screen top (trace-verified on the Karazhan candle
  // flame-lick 4x4 flipbook, RE_notes/18: min-v -> screen TOP, min-u -> screen LEFT, no rotation). Noggit
  // uploads DXT mip data verbatim (GL v=0 = the BLP's top row, same as D3D's convention) but this quad
  // previously mapped the tile's MAX v to the +up corners, drawing every billboard sprite UPSIDE-DOWN.
  // Invisible on the radially-symmetric + spinning smoke/glow sprites (their fixed orientation is masked),
  // but the fixed, upright, non-spinning candle flames rendered inverted -- the "sideways" chandelier
  // flames. Emitting min-v at the top corners makes all sprites match the client.
  otc[0].x = a.x; otc[0].y = b.y; // screen bottom-left  <- tile bottom (max v)
  otc[1].x = b.x; otc[1].y = b.y; // screen bottom-right <- tile bottom (max v)
  otc[2].x = b.x; otc[2].y = a.y; // screen top-right    <- tile top    (min v)
  otc[3].x = a.x; otc[3].y = a.y; // screen top-left     <- tile top    (min v)

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
            LogError << "[PARTDBG-SPAWN] tex=" << _texture_id << " emitterType=" << emitter_type
                     << " bone=" << _bone_index << " ridesParent=" << (ridesParent() ? 1 : 0)
                     << " emitterLocalPos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
                     << " boneWorldPos=(" << bone_pos.x << "," << bone_pos.y << "," << bone_pos.z << ")"
                     << " spawnPos=(" << p.pos.x << "," << p.pos.y << "," << p.pos.z << ")"
                     << " spawnRadiusFromEmitter=" << glm::length(glm::vec3(p.pos.x - pos.x, 0.0f, p.pos.z - pos.z))
                     << " dir=(" << p.dir.x << "," << p.dir.y << "," << p.dir.z << ")"
                     << " speed=" << glm::length(p.speed)
                     << std::endl;
          }

          // Stable per-particle slot (client memory-slot index): drives twinkle phase + spin-sign parity.
          p.slot = (_spawn_seq++) & 0x7fu;

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

    // Wind (client CParticleEmitter2 §4): a YOUNG-ONLY acceleration -- while the particle's age is below
    // windTime the wind vector is added to velocity each step, so the drift ramps up to windVector*windTime
    // then freezes. Added to velocity (not position) so it integrates + is damped by drag like any force.
    if (_wind_time > 0.0f && p.life < _wind_time)
    {
      p.speed += _wind * dt;
    }

    if (slowdown>0) {
      mspeed = expf(-1.0f * slowdown * p.life);
    }
    else if (slowdown < 0.0f) {
      // NEGATIVE authored drag = ANTI-drag: the particle ACCELERATES exponentially over its life -- it gets
      // progressively faster and looks "SUCKED IN" toward the model at the end. User-observed on the live
      // 1.12 client for the Anomalus feet mist (drag=-0.1): the particles converge on his body and speed up
      // exponentially into it just before they fade -- start-to-finish the travel speed is NOT constant, it
      // ramps up hard at the end. exp(-slowdown*life) = exp(+|drag|*life) grows past 1 (same formula the
      // positive branch uses, just the anti-drag sign). The old code used exp(slowdown*life) (a DECAY), which
      // decelerated negative-drag particles, so ours drifted and slowed at the end instead of being sucked in.
      mspeed = expf(-slowdown * p.life);
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
    if (_uv_animated && !_cell_values.empty())
    {
      // WotLK keyed cell track: find the segment containing rlife and interpolate the cell index across
      // it. Times are non-decreasing; duplicated timestamps (the lifespan->decay hand-off) give a zero
      // width segment, which we treat as an instant jump to the later key. Past the last key the cell
      // holds -- emitters whose track ends early (last time < 32767) freeze on their final cell.
      float const f = std::clamp(rlife, 0.0f, 1.0f);
      int cell = _cell_values.back();

      if (f <= _cell_times.front())
      {
        cell = _cell_values.front();
      }
      else
      {
        for (std::size_t k = 1; k < _cell_times.size(); ++k)
        {
          if (f > _cell_times[k]) { continue; }

          float const span = _cell_times[k] - _cell_times[k - 1];
          float const t = span > 1e-6f ? (f - _cell_times[k - 1]) / span : 1.0f;
          cell = static_cast<int>(static_cast<float>(_cell_values[k - 1])
                                  + t * static_cast<float>(_cell_values[k] - _cell_values[k - 1]));
          break;
        }
      }

      if (cell < 0) { cell = 0; }
      if (!tiles.empty() && static_cast<std::size_t>(cell) >= tiles.size())
      {
        cell = static_cast<int>(tiles.size()) - 1;
      }
      p.tile = static_cast<unsigned int>(cell);
    }
    else if (_uv_animated)
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

    // calculate size and color based on lifetime. Keyed tracks win where the emitter authored them
    // (wotlk); everything else falls back to the classic three-key ramp through `mid`. The three
    // curves are independent, so an emitter can key its colour and still ramp its size.
    p.size = _size_times.empty()
      ? lifeRamp<float>(rlife, mid, sizes[0], sizes[1], sizes[2])
      : lifeTrack<float>(_size_times, _size_values, rlife);

    p.color = lifeRamp<glm::vec4>(rlife, mid, colors[0], colors[1], colors[2]);
    if (!_color_times.empty())
    {
      glm::vec3 const rgb = lifeTrack<glm::vec3>(_color_times, _color_values, rlife);
      p.color.r = rgb.r;
      p.color.g = rgb.g;
      p.color.b = rgb.b;
    }
    if (!_alpha_times.empty())
    {
      p.color.a = lifeTrack<float>(_alpha_times, _alpha_values, rlife);
    }

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

  // [PARTDBG-TRAVEL] Anomalus FEET SMOKE (the sphere emitter) cloud snapshot -- lets us diff noggit's ACTUAL
  // particle travel against the client trace instead of a sim. Trace ground truth (world yd): particles are
  // born from rest on a wide ~8-yd-radius ring at his feet, converge inward to ~2.5 yd while rising ~4.1 yd,
  // ~39 alive, size(edge) 0.42->3.27, alpha ~140->184->11. Positions here are bone-local (rideParent), so
  // world ~= model * 3.5 (Anomalus display scale 3.5). Set NOGGIT_CLASSIC_EFFECT_DEBUG=1 and stand near him.
  if (classic_effect_debug_enabled() && !particles.empty()
      && model && model->file_key().hasFilepath()
      && model->file_key().filepath().find("anomalus") != std::string::npos)
  {
    static int travel_frame = 0;
    static int travel_logs = 0;
    if ((travel_frame++ % 15) == 0 && travel_logs < 64)
    {
      ++travel_logs;
      float rise_min = 1e9f, rise_max = -1e9f, rise_sum = 0.0f;
      float rad_min = 1e9f, rad_max = -1e9f, rad_sum = 0.0f;
      float size_min = 1e9f, size_max = -1e9f, a_min = 1e9f, a_max = -1e9f, life_max = 0.0f;
      // Mean rise + mean vertical SPEED per life-quartile -> the acceleration SHAPE. If the plume speeds up
      // toward the end, both the rise-gaps AND vy should GROW across quartiles (q0<q1<q2<q3). Flat vy = it
      // rises at a constant rate (no speed-up); shrinking vy = it decelerates.
      float qrise[4] = {0.f, 0.f, 0.f, 0.f}, qvy[4] = {0.f, 0.f, 0.f, 0.f};
      int   qn[4] = {0, 0, 0, 0};
      for (auto const& q : particles)
      {
        float const rise = q.pos.y - q.origin.y;
        float const rad = glm::length(glm::vec3(q.pos.x - pos.x, 0.0f, q.pos.z - pos.z));
        rise_min = std::min(rise_min, rise); rise_max = std::max(rise_max, rise); rise_sum += rise;
        rad_min = std::min(rad_min, rad); rad_max = std::max(rad_max, rad); rad_sum += rad;
        size_min = std::min(size_min, q.size); size_max = std::max(size_max, q.size);
        a_min = std::min(a_min, q.color.a); a_max = std::max(a_max, q.color.a);
        life_max = std::max(life_max, q.life);
        float const rl = (q.maxlife > 0.001f) ? (q.life / q.maxlife) : 0.0f;
        int const b = std::min(3, std::max(0, static_cast<int>(rl * 4.0f)));
        qrise[b] += rise; qvy[b] += q.speed.y; qn[b] += 1;   // q.speed.y = current vertical velocity
      }
      float const n = static_cast<float>(particles.size());
      auto qr = [&](int b) { return qn[b] > 0 ? qrise[b] / qn[b] : 0.0f; };
      auto qv = [&](int b) { return qn[b] > 0 ? qvy[b] / qn[b] : 0.0f; };
      LogError << "[PARTDBG-TRAVEL] anomalus emitter tex=" << _texture_id << " bone=" << _bone_index
               << " etype=" << emitter_type << " blend=" << blend << " n=" << static_cast<int>(n)
               << " grav=" << grav << " lifeMax=" << life_max
               << " | rise model(min/mean/max)=" << rise_min << "/" << (rise_sum / n) << "/" << rise_max
               << " -> world.max=" << (rise_max * 3.5f) << " (trace ~4.1)"
               << " | rise/quartile=" << qr(0) << "/" << qr(1) << "/" << qr(2) << "/" << qr(3)
               << " | vy/quartile=" << qv(0) << "/" << qv(1) << "/" << qv(2) << "/" << qv(3)
               << " (grow=speeds up at end)"
               << " | radius model(min/mean/max)=" << rad_min << "/" << (rad_sum / n) << "/" << rad_max
               << " -> world(min/max)=" << (rad_min * 3.5f) << "/" << (rad_max * 3.5f) << " (trace 2.5->8)"
               << " | size(min/max)=" << size_min << "/" << size_max << " edge.world=" << (size_max * 2.0f)
               << " | alpha(min/max)=" << a_min << "/" << a_max
               << std::endl;
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
  if (!_geometry_model_path.empty())
  {
    return; // geometry-model particles draw as mesh instances (WorldRender pass), never as quads
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

  // Tail (type 1/2) is billboarded about the velocity axis -> it also needs the view right/up vectors.
  if (billboard || type == 1 || type == 2)
  {
    vRight = glm::normalize(glm::vec3(model_view[0]));
    vUp = glm::normalize(glm::vec3(model_view[1]));

    //vRight = glm::vec3(model_view[0][0], model_view[1][0], model_view[2][0]);
    //vUp = glm::vec3(model_view[0][1], model_view[1][1], model_view[2][1]); // Spherical billboarding
    //vUp = glm::vec3(0,1,0); // Cylindrical billboarding
  }

  // Flag 0x10 (ride parent): particles are stored BONE-LOCAL (client CParticle2 convention, RE
  // handoff 6.4) -- transform by the bone's CURRENT matrix here so live particles follow the
  // animated bone every frame (the instance portal's swirl rotates with its spinning bone like a
  // wheel instead of freezing where each puff spawned).
  bool const riding = ridesParent();
  glm::mat4x4 const ride_mat = riding ? parent->mat : glm::mat4x4(1.0f);
  glm::mat3 const ride_rot = glm::mat3(ride_mat);
  auto const ride_pos = [&](glm::vec3 const& v) -> glm::vec3
  {
    return riding ? glm::vec3(ride_mat * glm::vec4(v, 1.0f)) : v;
  };

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

    if (billboard || type == 2)
    {
      // Per-particle quad rotation: authored params.spin (radians/sec) rotates the billboard around
      // its center over the particle's life -- positive = counterclockwise on screen (e.g. the arcane
      // elementals' feet smoke, spin=2.0). Spline emitters keep _spin as their emission-path travel
      // speed instead (the MC flamecircle), so no quad rotation for those.
      bool const quad_spin = (_spin != 0.0f) && _spline_points.empty();
      // Twinkle (client render-time §5): only do the work when the emitter authors it.
      // Active only when it actually does something: cull (percent<1) or size shimmer (min!=max).
      // twinkleSpeed alone (percent=1, min==max, e.g. Anomalus feet smoke) is inert -> skip the work.
      bool const twinkle_active = (_twinkle_percent < 1.0f) || (_twinkle_scale_min != _twinkle_scale_max);
      std::array<float, 128> const& twinkle_tbl = twinkle_noise_table();

      for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it)
      {
        if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
        {
          break;
        }

        float size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;// / 2;
        if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec4(it->color))
        {
          continue;
        }

        // Twinkle: sample the shared noise table at this particle's advancing phase; the client CULLS
        // the sprite this frame when twinklePercent < t (the blink-out) and shimmers the size by
        // lerp(scaleMin,scaleMax,t). idx = (floor(twinkleSpeed*age)+slot) & 0x7f.
        if (twinkle_active)
        {
          float const phase = std::min(std::max(_twinkle_speed * it->life, 0.0f), 255.0f);
          int const idx = (static_cast<int>(phase) + static_cast<int>(it->slot)) & 0x7f;
          float const t = twinkle_tbl[idx];
          if (_twinkle_percent < 1.0f && _twinkle_percent < t)
          {
            continue; // blinked out this frame
          }
          if (_twinkle_scale_min != _twinkle_scale_max)
          {
            size *= _twinkle_scale_min + t * (_twinkle_scale_max - _twinkle_scale_min);
          }
        }

        glm::vec3 quad_right = vRight;
        glm::vec3 quad_up = vUp;
        if (quad_spin)
        {
          // Alternating spin direction when emitter flag 0x8000 is set (the client's per-slot "random"
          // sign); without the flag every sprite spins the same way (angle = spin * age).
          float const sign = (_spin_alternate && (it->slot & 1u)) ? -1.0f : 1.0f;
          float const ang = sign * _spin * it->life;
          float const c = std::cos(ang);
          float const s = std::sin(ang);
          quad_right = vRight * c + vUp * s;
          quad_up = vUp * c - vRight * s;
        }

        glm::vec3 const ppos = ride_pos(it->pos);

        texcoords.push_back(tiles[it->tile].tc[0]);
        vertices.push_back(ppos);
        offsets.push_back(-(quad_right + quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[1]);
        vertices.push_back(ppos);
        offsets.push_back((quad_right - quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[2]);
        vertices.push_back(ppos);
        offsets.push_back((quad_right + quad_up) * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[3]);
        vertices.push_back(ppos);
        offsets.push_back(-(quad_right - quad_up) * size);
        colors_data.push_back(it->color);

        add_quad_indices(indices, indice);
      }
    }
    else
    {
      // Active only when it actually does something: cull (percent<1) or size shimmer (min!=max).
      // twinkleSpeed alone (percent=1, min==max, e.g. Anomalus feet smoke) is inert -> skip the work.
      bool const twinkle_active = (_twinkle_percent < 1.0f) || (_twinkle_scale_min != _twinkle_scale_max);
      std::array<float, 128> const& twinkle_tbl = twinkle_noise_table();

      for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it)
      {
        if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
        {
          break;
        }

        float size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;
        if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec4(it->color))
        {
          continue;
        }

        // Twinkle (see billboard branch): cull + size shimmer from the shared noise table.
        if (twinkle_active)
        {
          float const phase = std::min(std::max(_twinkle_speed * it->life, 0.0f), 255.0f);
          int const idx = (static_cast<int>(phase) + static_cast<int>(it->slot)) & 0x7f;
          float const t = twinkle_tbl[idx];
          if (_twinkle_percent < 1.0f && _twinkle_percent < t)
          {
            continue;
          }
          if (_twinkle_scale_min != _twinkle_scale_max)
          {
            size *= _twinkle_scale_min + t * (_twinkle_scale_max - _twinkle_scale_min);
          }
        }

        glm::vec3 const ppos = ride_pos(it->pos);

        texcoords.push_back(tiles[it->tile].tc[0]);
        vertices.push_back(ppos + ride_rot * it->corners[0] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[1]);
        vertices.push_back(ppos + ride_rot * it->corners[1] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[2]);
        vertices.push_back(ppos + ride_rot * it->corners[2] * size);
        colors_data.push_back(it->color);

        texcoords.push_back(tiles[it->tile].tc[3]);
        vertices.push_back(ppos + ride_rot * it->corners[3] * size);
        colors_data.push_back(it->color);

        add_quad_indices(indices, indice);
      }
    }
  }  
  if (type == 1 || type == 2)
  { // TAIL particles (ParticleType 1=Tail, 2=Both). Client-exact (RE'd from wow.exe render fill
    // FUN_007b2a50 tail branch @0x7b3041): each live particle is a MOTION STREAK of tailLength * the
    // velocity, extending BEHIND the particle (along -velocity), billboarded about the velocity axis
    // with width = the particle size. The old path drew a spawn-origin->position ribbon (grew to the
    // full fall length + fixed model-space width) which is why drool never stretched right. Uses the
    // billboard OFFSET path (position = streak ends in model space so the streak follows the velocity
    // under any instance transform; offset = the view-aligned width so it stays screen-facing).
    float const min_stretch_sq = 0.000771605f; // client short-streak->head fallback threshold @0x80c744
    for (ParticleList::iterator it = particles.begin(); it != particles.end(); ++it)
    {
      if (tiles.size() - 1 < it->tile) // Alfred, 2009.08.07, error prevent
      {
        break;
      }

      float const size = classic ? sane_classic_particle_size(it->size, 1.0f) : it->size;
      if (!std::isfinite(size) || !finite_vec3(it->pos) || !finite_vec3(it->speed) || !finite_vec4(it->color))
      {
        continue;
      }

      // Streak length = tailLength, clamped to the particle's age when the emitter opts in (flag 0x400)
      // so a just-spawned drip has a short tail that grows to full length instead of popping.
      float len = _tail_length;
      if (_tail_clamp_age && it->life < len) { len = it->life; }

      glm::vec3 const vel = ride_rot * it->speed;   // sim-space velocity (ride_rot = identity outdoors)
      glm::vec3 const streak = -len * vel;          // trails BEHIND the motion
      glm::vec3 const head = ride_pos(it->pos);
      glm::vec3 const tail = head + streak;

      // Project the streak onto the screen (view) plane; width is perpendicular to it, magnitude = size.
      float const sx = glm::dot(streak, vRight);
      float const sy = glm::dot(streak, vUp);
      float const len_xy_sq = sx * sx + sy * sy;

      if (len_xy_sq < min_stretch_sq)
      {
        // Streak too short on screen -> plain head billboard (client fallback), size square at the head.
        texcoords.push_back(tiles[it->tile].tc[0]); vertices.push_back(head); offsets.push_back(-(vRight + vUp) * size); colors_data.push_back(it->color);
        texcoords.push_back(tiles[it->tile].tc[1]); vertices.push_back(head); offsets.push_back(( vRight - vUp) * size); colors_data.push_back(it->color);
        texcoords.push_back(tiles[it->tile].tc[2]); vertices.push_back(head); offsets.push_back(( vRight + vUp) * size); colors_data.push_back(it->color);
        texcoords.push_back(tiles[it->tile].tc[3]); vertices.push_back(head); offsets.push_back(-(vRight - vUp) * size); colors_data.push_back(it->color);
        add_quad_indices(indices, indice);
        continue;
      }

      // Perpendicular to the on-screen streak, |perp| = size, view-aligned (added as a billboard offset).
      glm::vec3 const perp = (size / std::sqrt(len_xy_sq)) * (-sy * vRight + sx * vUp);

      // 4 corners around the quad: head+perp, head-perp, tail-perp, tail+perp. UVs map the tile square
      // onto the streak (head edge tc0-tc1, tail edge tc3-tc2), so the texture stretches along the tail.
      texcoords.push_back(tiles[it->tile].tc[0]); vertices.push_back(head); offsets.push_back( perp); colors_data.push_back(it->color);
      texcoords.push_back(tiles[it->tile].tc[1]); vertices.push_back(head); offsets.push_back(-perp); colors_data.push_back(it->color);
      texcoords.push_back(tiles[it->tile].tc[2]); vertices.push_back(tail); offsets.push_back(-perp); colors_data.push_back(it->color);
      texcoords.push_back(tiles[it->tile].tc[3]); vertices.push_back(tail); offsets.push_back( perp); colors_data.push_back(it->color);
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
    std::string bound_tex = "<none>";
    int bound_layer = -1;
    if (_texture_id < model->_textures.size() && model->_textures[_texture_id].get())
    {
      bound_tex = model->_textures[_texture_id]->file_key().stringRepr();
      bound_layer = model->_textures[_texture_id]->array_index();
    }
    std::string tile0 = "<none>";
    if (!tiles.empty())
    {
      std::ostringstream t0;
      t0 << "{(" << tiles[0].tc[0].x << "," << tiles[0].tc[0].y << ")(" << tiles[0].tc[1].x << "," << tiles[0].tc[1].y
         << ")(" << tiles[0].tc[2].x << "," << tiles[0].tc[2].y << ")(" << tiles[0].tc[3].x << "," << tiles[0].tc[3].y << ")}";
      tile0 = t0.str();
    }
    LogDebug << "Classic effect particle draw model='" << model->file_key().stringRepr()
             << "' source=classic"
             << " boundTex='" << bound_tex << "' layer=" << bound_layer
             << " tile0=" << tile0
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
  // Tail particles (type 1/2) are built with the billboard OFFSET path (streak ends in `position` + a
  // view-aligned width in `offset`), so they need billboard=1 and the offset buffer bound even when the
  // emitter's own billboard flag is clear.
  bool const use_offsets = billboard || type == 1 || type == 2;
  shader.uniform("billboard", use_offsets ? 1 : 0);
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
  if(use_offsets)
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
  // Ride-parent (flag 0x10) systems spawn in BONE-LOCAL space: draw() applies the bone's current
  // matrix, so the spawn-time bone rotation must NOT be baked into directions/corners either.
  auto mrot = (sys->ridesParent() ? glm::mat4x4(1.0f) : glm::mat4x4(sys->parent->mrot))
            * CalcSpreadMatrix(spr, spr, 1.0f, 1.0f);

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
    // Flag 0x10 (ride parent): spawn LOCAL, draw() applies the bone's current matrix.
    p.pos = sys->ridesParent() ? local : glm::vec3(sys->parent->mat * glm::vec4(local, 1.0f));

    glm::vec3 dir = mrot * glm::vec4(0, 1, 0, 0);
    p.dir = safe_normalize_vec3(dir, glm::vec3(0.0f, 1.0f, 0.0f));
    p.down = glm::vec3(0, -1.0f, 0);
    p.speed = p.dir * spd * (1.0f + misc::randfloat(-var, var));
  }
  // (12.14, 2026-07-19) REMOVED the WMV whole-word magic-number branches -- Trans Halo (flags==1041),
  // Weapon Flame (==25, x2), Weapon Glow (==17). RE_notes/12 (decompiled CParticleEmitter2): the 1.12
  // client copies the emitter flags VERBATIM and runs ONE general plane CreateParticle (FUN_007b8890) for
  // every plane emitter -- there is NO per-model flag dispatch. These were WoW-Model-Viewer per-model
  // approximations that (a) fell to generic behavior on any near-miss flag combo and (b) diverged from the
  // client (the halo even carried a "manually correct - why?" fudge). Every plane emitter now takes the
  // client-faithful general path below; the sphere magic-numbers (Faith-Halo 57/313) were removed the same
  // way for the portal fix. Individual flag BITS are still honored where they matter (0x10 ride-parent,
  // 0x400 tail-clamp, 0x8000 spin-sign). VERIFY in-game: flaming/glowing weapons + halo/aura rings.
  else {
    // CLIENT-FAITHFUL plane emission (RE'd from 1.12 CParticleEmitter2 plane CreateParticle,
    // FUN_007b8890): spawn on the authored rect -- model X in +-areaLength/2 (w), model Y in
    // +-areaWidth/2 (l) -- and emit with a velocity TILTED from straight-up by theta in
    // +-VerticalRange (spr) at azimuth phi in +-HorizontalRange (spr2). The old WMV spread-matrix
    // used the vertical range for BOTH axes and ignored the horizontal range entirely.
    glm::vec3 local = sys->pos + glm::vec3(misc::randfloat(-w, w), 0, misc::randfloat(-l, l));
    // Flag 0x10 (ride parent): spawn LOCAL, draw() applies the bone's current matrix (see sphere).
    bool const rides = sys->ridesParent();
    p.pos = rides ? local : glm::vec3(sys->parent->mat * glm::vec4(local, 1));

    float const theta = misc::randfloat(-spr, spr);    // tilt from the emitter's up axis
    float const phi = misc::randfloat(-spr2, spr2);    // azimuth around the up axis

    // client model space is z-up: dir = (cos(phi)sin(theta), sin(phi)sin(theta), cos(theta));
    // converted to noggit's y-up space like fixCoordSystem.
    glm::vec3 const tilted(std::cos(phi) * std::sin(theta),
                           std::cos(theta),
                           -std::sin(phi) * std::sin(theta));
    glm::vec3 dir = rides ? tilted : glm::vec3(sys->parent->mrot * glm::vec4(tilted, 0));

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

  // NOTE: WMV carried a "Faith Halo" override here for emitters whose whole flags word equaled
  // 57 or 313 (ring at 1.6x radius in the local XZ plane, radial speed). The REAL 1.12 client has
  // exactly ONE sphere CreateParticle (FUN_007b8d70, RE'd + trace-verified below) with no such
  // flags dispatch -- and the override is what made instance portals (flags=57) tumble like a
  // flipping coin instead of spinning in the portal plane. Removed 2026-07-03; all sphere
  // emitters now take the client-faithful path. (Full canon flag-bit semantics = checklist 12.14.)
  {
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

    // radial unit direction in noggit space (up = +Y; model z-up converted like fixCoordSystem).
    // AZIMUTH PHASE (2026-07-03, empirically derived from InstancePortal): azimuth 0 points along
    // model +Y, not +X. With HorizontalRange=0 + VerticalRange=pi the ring then lies in the model
    // YZ plane -- the portal's octagon plane, whose normal (X) is the spinning bone's axis -- so
    // the swirl rotates like a WHEEL in its own plane. With the old +X phase the ring lay in the
    // XZ meridian and the bone spin tumbled it like a flipping coin. Full-azimuth emitters
    // (HorizontalRange ~ pi, e.g. the arcane elementals' smoke ring) are statistically identical
    // under any phase, so this only affects constrained-azimuth emitters.
    // wow-space radial = (-cos(theta)*sin(phi), cos(theta)*cos(phi), sin(theta)) -> noggit y-up:
    glm::vec3 const radial(-std::cos(theta) * std::sin(phi),
                           std::sin(theta),
                           -std::cos(theta) * std::cos(phi));

    // Flag 0x10 (ride parent): keep spawn LOCAL to the bone -- draw() applies the bone's current
    // matrix each frame so live particles rotate with it (portal swirl wheel). Otherwise bake the
    // bone matrix at spawn (client leaves non-riding particles behind).
    bool const rides = sys->ridesParent();
    glm::vec3 const local_pos = sys->pos + radial * r;
    p.pos = rides ? local_pos : glm::vec3(sys->parent->mat * glm::vec4(local_pos, 1));

    if (sys->flags & 0x100)
    {
      dir = rides ? glm::vec3(0, 1, 0) : glm::vec3(sys->parent->mrot * glm::vec4(0, 1, 0, 0));
    }
    else
    {
      dir = rides ? radial : glm::vec3(sys->parent->mrot * glm::vec4(radial, 0));
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
  // unk1/unk2 = texture-slot (int16) and visibility (uint8) tracks (client RE note 25)
  , tex_slot_track (mta.unk1, f, globals)
  , visibility_track (mta.unk2, f, globals)
  , parent (&model->bones[mta.bone])
  , pos (fixCoordSystem(mta.pos))
  // CLIENT-EXACT field semantics (CRibbonEmitter::Initialize RE, docs/client_re/25):
  // res = edgesPerSecond, length = edgeLifetime (seconds), Emissionangle = gravity,
  // s1/s2 = texture slot rows/cols. The old res*length "length in yards" was dimensionally wrong
  // (BFD too long / CoT too short).
  , edges_per_sec (std::max(mta.res, 1.0f))
  , edge_lifetime (std::max(mta.length, 0.05f))
  , gravity (mta.Emissionangle)
  , tex_rows (std::max<int>(mta.s1, 1))
  , tex_cols (std::max<int>(mta.s2, 1))
  , tpos (fixCoordSystem(mta.pos))
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);
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
  , tex_slot_track (mta.unk1, f, globals)
  , visibility_track (mta.unk2, f, globals)
  // Classic effect/creature trails reference a bone index; clamp to a valid bone (mirrors the
  // particle-emitter bone guard) so a stray index can't read past the bones vector and crash.
  , parent (&model->bones[(mta.bone >= 0 && static_cast<std::size_t>(mta.bone) < model->bones.size()) ? mta.bone : 0])
  , pos (fixCoordSystem(mta.pos))
  , edges_per_sec (std::max(mta.res, 1.0f))
  , edge_lifetime (std::max(mta.length, 0.05f))
  , gravity (mta.Emissionangle)
  , tex_rows (std::max<int>(mta.s1, 1))
  , tex_cols (std::max<int>(mta.s2, 1))
  , tpos (fixCoordSystem(mta.pos))
  , _context(context)
{
  _texture_ids = Model::M2Array<uint16_t>(f, mta.ofsTextures, mta.nTextures);
  _material_ids = Model::M2Array<uint16_t>(f, mta.ofsMaterials, mta.nMaterials);
}

RibbonEmitter::RibbonEmitter(RibbonEmitter const& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , tex_slot_track(other.tex_slot_track)
  , visibility_track(other.visibility_track)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , edges_per_sec(other.edges_per_sec)
  , edge_lifetime(other.edge_lifetime)
  , gravity(other.gravity)
  , tex_rows(other.tex_rows)
  , tex_cols(other.tex_cols)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _emit_accum(other._emit_accum)
  , _have_prev(other._have_prev)
  , _prev_above(other._prev_above)
  , _prev_below(other._prev_below)
  , _cur_above(other._cur_above)
  , _cur_below(other._cur_below)
  , _last_animtime(other._last_animtime)
  , _cur_slot(other._cur_slot)
  , _visible(other._visible)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , edges(other.edges)
  , _context(other._context)
{

}

RibbonEmitter::RibbonEmitter(RibbonEmitter&& other)
  : model(other.model)
  , color(other.color)
  , opacity(other.opacity)
  , above(other.above)
  , below(other.below)
  , tex_slot_track(other.tex_slot_track)
  , visibility_track(other.visibility_track)
  , parent(other.parent)
  , pos(other.pos)
  , manim(other.manim)
  , mtime(other.mtime)
  , edges_per_sec(other.edges_per_sec)
  , edge_lifetime(other.edge_lifetime)
  , gravity(other.gravity)
  , tex_rows(other.tex_rows)
  , tex_cols(other.tex_cols)
  , tpos(other.tpos)
  , tcolor(other.tcolor)
  , tabove(other.tabove)
  , tbelow(other.tbelow)
  , _emit_accum(other._emit_accum)
  , _have_prev(other._have_prev)
  , _prev_above(other._prev_above)
  , _prev_below(other._prev_below)
  , _cur_above(other._cur_above)
  , _cur_below(other._cur_below)
  , _last_animtime(other._last_animtime)
  , _cur_slot(other._cur_slot)
  , _visible(other._visible)
  , _texture_ids(other._texture_ids)
  , _material_ids(other._material_ids)
  , edges(other.edges)
  , _context(other._context)
{

}

void RibbonEmitter::setup(int anim, int time, int animtime)
{
  // CLIENT-EXACT update (CRibbonEmitter::Update @007b7e60, docs/client_re/25):
  //   1. expire edges whose age + dt exceeds edgeLifetime
  //   2. emit dt*edgesPerSecond + accumulator edges, sub-frame interpolated between the previous
  //      and current anchor points, ages backdated so the trail is continuous at any framerate
  //   3. age every edge and apply gravity as the exact integral: dh = (2*age + dt) * dt * g
  // The head edge is virtual: draw() prepends the CURRENT anchors at age 0 every frame.
  manim = anim;
  mtime = time;

  float dt = 0.0f;
  if (_last_animtime >= 0 && animtime > _last_animtime)
  {
    dt = static_cast<float>(animtime - _last_animtime) * 0.001f;
  }
  _last_animtime = animtime;
  dt = std::clamp(dt, 0.0f, edge_lifetime);

  auto col = color.getValue(anim, time, animtime);
  tcolor = glm::vec4(col.x, col.y, col.z, opacity.getValue(anim, time, animtime));
  tabove = above.getValue(anim, time, animtime);
  tbelow = below.getValue(anim, time, animtime);
  // texture slot + visibility tracks, sampled every frame like the client (SetTexSlot only
  // recomputes on change; ours resolves the cell in draw() so a plain store is equivalent)
  _cur_slot = tex_slot_track.uses(anim) ? tex_slot_track.getValue(anim, time, animtime) : 0;
  _visible = visibility_track.uses(anim) ? (visibility_track.getValue(anim, time, animtime) != 0) : true;

  glm::vec3 const ntpos = parent->mat * glm::vec4(pos, 1);
  glm::vec3 ntup = parent->mat * (glm::vec4(pos, 1) + glm::vec4(0, 0, 1, 0));
  ntup = safe_normalize_vec3(ntup - ntpos, glm::vec3(0.0f, 1.0f, 0.0f));

  _prev_above = _cur_above;
  _prev_below = _cur_below;
  _cur_above = ntpos + ntup * tabove;
  _cur_below = ntpos - ntup * tbelow;
  if (!_have_prev)
  {
    _prev_above = _cur_above;
    _prev_below = _cur_below;
    _have_prev = true;
  }
  tpos = ntpos;

  // 1. expire (oldest at the back)
  while (!edges.empty() && edges.back().age + dt > edge_lifetime)
  {
    edges.pop_back();
  }

  // 2. emit with sub-frame interpolation -- ONLY while the visibility track is on (client Update
  // gates the emit block on it; expiry/aging below run regardless so the trail ages out).
  // Hiding also clears the primed-anchor state (client SetVisible(0) clears flag bit 0), so a
  // re-show does not stretch a quad from the last visible position.
  if (!_visible)
  {
    _have_prev = false;
  }
  float const f = _visible ? dt * edges_per_sec + _emit_accum : _emit_accum;
  if (_visible && f >= 1.0f)
  {
    float const inv = 1.0f / (f - _emit_accum);
    int const n = static_cast<int>(std::floor(f - 1.0f)) + 1;
    for (int k = 0; k < n; ++k)
    {
      float const t = ((1.0f + k) - _emit_accum) * inv; // fraction of the frame's motion
      RibbonEdge e;
      e.above = glm::mix(_prev_above, _cur_above, t);
      e.below = glm::mix(_prev_below, _cur_below, t);
      e.age = -t * dt; // backdated; the aging step below adds dt -> final (1-t)*dt
      edges.push_front(e);
    }
    _emit_accum = f - std::floor(f);
  }
  else if (_visible)
  {
    _emit_accum = f;
  }

  // cap (client: ceil(edgesPerSecond) * edgeLifetime edges)
  std::size_t const max_edges = static_cast<std::size_t>(std::ceil(edges_per_sec) * edge_lifetime) + 2;
  while (edges.size() > max_edges)
  {
    edges.pop_back();
  }

  // 3. age + gravity (exact integral of v = g*age over the step)
  for (auto& e : edges)
  {
    float const drop = (e.age + e.age + dt) * dt * gravity;
    e.above.y -= drop;
    e.below.y -= drop;
    e.age += dt;
  }
}

void RibbonEmitter::draw( OpenGL::Scoped::use_program& shader
                        , GLuint const& transform_vbo
                        , int instances_count
                        )
{
  // hidden (visibility track off): no head quad, so at least two aged edges are needed for a quad
  if (edges.empty() || (!_visible && edges.size() < 2))
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  std::vector<std::uint16_t> indices;
  std::vector<glm::vec3> vertices;
  std::vector<glm::vec2> texcoords;

  gl.enable(GL_BLEND);
  shader.uniform("color", tcolor);

  // Texture slot cell (client SetTexSlot @007b6f30, uvRect=(0,0,1,1)): column = slot % cols along
  // U (cell 1/s2), row = slot / cols along V (cell 1/s1). U runs base..base+cell with EDGE AGE /
  // lifetime; all edges use the CURRENT slot base (the client recomputes every edge's U each frame
  // from it, so a slot change re-textures the whole trail).
  float const cell_w = 1.0f / static_cast<float>(tex_cols);
  float const cell_h = 1.0f / static_cast<float>(tex_rows);
  int const slot = std::clamp(_cur_slot, 0, tex_rows * tex_cols - 1);
  float const base_u = static_cast<float>(slot % tex_cols) * cell_w;
  float const base_v = static_cast<float>(slot / tex_cols) * cell_h;

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

  // head: the CURRENT anchor points at age 0 (refreshed every frame like the client's head edge);
  // skipped while the visibility track is off (the client stops refreshing it -- the trail tail
  // just ages out).
  if (_visible)
  {
    texcoords.emplace_back(base_u, base_v);
    vertices.push_back(_cur_above);
    texcoords.emplace_back(base_u, base_v + cell_h);
    vertices.push_back(_cur_below);
  }

  bool first = !_visible; // hidden: the first aged edge takes the head's place (no quad before it)
  for (auto const& e : edges)
  {
    float const u = base_u + std::clamp(e.age / edge_lifetime, 0.0f, 1.0f) * cell_w;
    texcoords.emplace_back(u, base_v);
    vertices.push_back(e.above);
    texcoords.emplace_back(u, base_v + cell_h);
    vertices.push_back(e.below);

    if (first)
    {
      first = false;
      continue;
    }
    add_quad_indices(indices, indice);
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

  // MULTI-PASS like the client (render @007b80c0): the SAME strip is drawn once per
  // texture/material pair -- texture j with render_flags[material j]'s blend (pairs built in the
  // create loop pairing textures[j] with materials[j]). Nearly all ribbons have one pair.
  std::size_t const n_passes = std::max<std::size_t>(_texture_ids.size(), 1);
  for (std::size_t pass = 0; pass < n_passes; ++pass)
  {
    if (pass < _texture_ids.size() && _texture_ids[pass] < model->_textures.size())
    {
      model->_textures[_texture_ids[pass]]->bind();
      shader.uniform("tex_index", model->_textures[_texture_ids[pass]]->array_index());
    }
    else
    {
      shader.uniform("tex_index", 0);
    }

    // material for this pass (clamped to the last id if the arrays are uneven); M2 blend -> GL
    // exactly like ModelRenderPass::prepareDraw. Additive fallback = old behaviour.
    int blend_mode = 4;
    if (!_material_ids.empty())
    {
      std::uint16_t const mid = _material_ids[std::min(pass, _material_ids.size() - 1)];
      if (mid < model->_render_flags.size())
      {
        blend_mode = model->_render_flags[mid].blend;
      }
    }
    switch (blend_mode)
    {
      case 0:
      case 1:  gl.blendFunc(GL_ONE, GL_ZERO); break;                    // opaque / alpha-key
      case 2:  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break; // alpha
      case 3:  gl.blendFunc(GL_SRC_COLOR, GL_ONE); break;                // no-add-alpha
      case 4:  gl.blendFunc(GL_SRC_ALPHA, GL_ONE); break;                // additive
      case 5:  gl.blendFunc(GL_DST_COLOR, GL_ZERO); break;               // modulate
      case 6:  gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;          // mod2x
      default: gl.blendFunc(GL_SRC_ALPHA, GL_ONE); break;
    }
    shader.uniform("ribbon_blend", blend_mode);

    gl.drawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_SHORT, nullptr, instances_count);
  }

  // restore the pass-wide additive func the ribbon loop in WorldRender expects
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
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

void ParticleSystem::setColorOverride(std::array<glm::vec4, 3> const& rgb)
{
  for (std::size_t i = 0; i < colors.size(); ++i)
  {
    colors[i] = glm::vec4(rgb[i].r, rgb[i].g, rgb[i].b, _authored_colors[i].a);
  }
}

void ParticleSystem::clearColorOverride()
{
  colors = _authored_colors;
}

void ParticleSystem::read_geometry_model_filename(BlizzardArchive::ClientFile const& f,
                                                  std::uint32_t n, std::uint32_t ofs)
{
  _geometry_model_path.clear();
  if (n <= 1 || ofs == 0 || static_cast<std::size_t>(ofs) + n > f.getSize())
  {
    return;
  }
  std::string path(f.getBuffer() + ofs, n);
  path = path.c_str(); // trim at the embedded NUL
  if (path.empty())
  {
    return;
  }
  std::replace(path.begin(), path.end(), '\\', '/');
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (path.size() >= 4)
  {
    auto const ext = path.substr(path.size() - 4);
    if (ext == ".mdx" || ext == ".mdl")
    {
      path.replace(path.size() - 4, 4, ".m2");
    }
  }
  _geometry_model_path = std::move(path);
}

void ParticleSystem::appendGeometryParticleTransforms(glm::mat4x4 const& host_transform,
                                                      std::vector<glm::mat4x4>& out) const
{
  for (auto const& p : particles)
  {
    glm::mat4x4 t = glm::translate(host_transform, p.pos);
    // Sprite-spin reused as a yaw for the mesh (revolutions -> radians over the particle's age);
    // alternate the sign by slot parity like the billboard path when the emitter authors it.
    if (_spin != 0.0f)
    {
      float angle = _spin * p.life * glm::two_pi<float>();
      if (_spin_alternate && (p.slot & 1u))
      {
        angle = -angle;
      }
      t = glm::rotate(t, angle, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    t = glm::scale(t, glm::vec3(std::max(p.size, 0.001f)));
    out.push_back(t);
  }
}
