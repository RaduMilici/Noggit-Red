// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Lightweight always-available per-phase frame profiler (perf hunt 2026-07-19). Gated at runtime on
// NOGGIT_FRAME_PROFILE=1: when off, Scoped is a ~2-instruction no-op. When on, it accumulates wall-clock
// ms per render phase and logs the averages + fps to log.txt once a second. CPU wall-clock deliberately --
// it captures BOTH cpu work AND stalls (a phase that blocks on the GL driver shows up as a fat number),
// which is what we need for the "CPU+GPU both idle" case. Reuses the same phase split as the Tracy zones.
#pragma once

#include <noggit/Log.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace noggit::perf
{
  enum class Phase
  {
    LightCollect, Terrain, WMO, M2, Water, Overlays, Post, // sub-phases inside WorldRender::draw
    M2Submit, M2Creatures, M2Particles,                    // finer M2 sub-phases (perf hunt 2026-07-19)
    SubmitInst, SubmitIndiv,                               // M2Submit split: instanced buckets vs one-by-one per-instance draws
    GatherCull, GatherMerge,                               // WMO-doodad gather serial sub-phases: Phase A cull / Phase C merge
    AnimateCPU,                                            // CPU bone/billboard compute + bone upload inside single-instance draw (animate())
    WorldDraw, Selection, Frame,                           // coarse (MapView): 3D total / selection pass / whole frame
    COUNT
  };

  inline char const* phase_name(Phase p)
  {
    switch (p)
    {
      case Phase::LightCollect: return "LightCollect";
      case Phase::Terrain:      return "Terrain";
      case Phase::WMO:          return "WMO";
      case Phase::M2:           return "M2";
      case Phase::Water:        return "Water";
      case Phase::Overlays:     return "Overlays";
      case Phase::Post:         return "Post";
      case Phase::M2Submit:     return "M2Submit";
      case Phase::M2Creatures:  return "M2Creatures";
      case Phase::M2Particles:  return "M2Particles";
      case Phase::SubmitInst:   return "SubmitInst";
      case Phase::SubmitIndiv:  return "SubmitIndiv";
      case Phase::GatherCull:   return "GatherCull";
      case Phase::GatherMerge:  return "GatherMerge";
      case Phase::AnimateCPU:   return "AnimateCPU";
      case Phase::WorldDraw:    return "WorldDraw";
      case Phase::Selection:    return "Selection";
      case Phase::Frame:        return "Frame";
      default:                  return "?";
    }
  }

  struct FrameProfiler
  {
    bool on = false;
    std::array<double, static_cast<std::size_t>(Phase::COUNT)> accum{};
    int frames = 0;
    std::chrono::steady_clock::time_point last_report{};

    static FrameProfiler& get()
    {
      static FrameProfiler p = []
      {
        FrameProfiler fp;
        fp.on = std::getenv("NOGGIT_FRAME_PROFILE") != nullptr;
        fp.last_report = std::chrono::steady_clock::now();
        return fp;
      }();
      return p;
    }

    void add(Phase p, double ms) { accum[static_cast<std::size_t>(p)] += ms; }

    // Call once per rendered frame (from MapView after the swap).
    void end_frame()
    {
      if (!on)
      {
        return;
      }
      ++frames;
      auto const now = std::chrono::steady_clock::now();
      double const since_ms = std::chrono::duration<double, std::milli>(now - last_report).count();
      if (since_ms < 1000.0 || frames <= 0)
      {
        return;
      }
      double const fps = frames * 1000.0 / since_ms;
      std::ostringstream os;
      os << "[FRAME-PROFILE] " << frames << " frames  " << fps << " fps  avg-ms/frame:";
      for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
      {
        os << "  " << phase_name(static_cast<Phase>(i)) << "=" << (accum[i] / frames);
      }
      LogError << os.str() << std::endl;
      accum.fill(0.0);
      frames = 0;
      last_report = now;
    }
  };

  // RAII: times its scope into the given phase bucket. No-op (bool check only) when profiling is off.
  struct Scoped
  {
    Phase _p;
    bool _on;
    std::chrono::steady_clock::time_point _t0;

    explicit Scoped(Phase p)
      : _p(p)
      , _on(FrameProfiler::get().on)
    {
      if (_on)
      {
        _t0 = std::chrono::steady_clock::now();
      }
    }
    ~Scoped()
    {
      if (_on)
      {
        FrameProfiler::get().add(_p,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _t0).count());
      }
    }
    Scoped(Scoped const&) = delete;
    Scoped& operator=(Scoped const&) = delete;
  };
}
