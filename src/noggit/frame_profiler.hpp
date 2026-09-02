// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Lightweight always-available per-phase frame profiler (perf hunt 2026-07-19; spike detection 2026-08-03).
// Gated at runtime on NOGGIT_FRAME_PROFILE=1: when off, Scoped is a ~2-instruction no-op. When on it:
//   1. logs per-phase AVERAGE ms + the WORST single frame per phase + fps, once a second ([FRAME-PROFILE]);
//   2. detects LAG SPIKES per frame -- any frame slower than max(NOGGIT_SPIKE_MS, NOGGIT_SPIKE_FACTOR x
//      rolling-baseline) is logged immediately ([FRAME-SPIKE]) with its per-phase breakdown AND an
//      "unaccounted" bucket (Frame minus the profiled phases), which is where a hitch OUTSIDE the 3D draw
//      -- tile streaming, an ADT GPU upload, a stall on the loader/GL driver -- shows up. Averages hide
//      spikes (one 100ms hitch in a 60-frame second adds ~1.6ms to the mean); this catches the frame.
// CPU wall-clock deliberately -- it captures BOTH cpu work AND stalls (a phase blocking on the GL driver
// shows up as a fat number). Reuses the same phase split as the Tracy zones.
#pragma once

#include <noggit/Log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace noggit::perf
{
  enum class Phase
  {
    LightCollect, Terrain, WMO, M2, Water, Overlays, Post, // sub-phases inside WorldRender::draw
    M2Submit, M2Creatures, M2Particles,                    // finer M2 sub-phases (perf hunt 2026-07-19)
    SubmitInst, SubmitIndiv,                               // M2Submit split: instanced buckets vs one-by-one per-instance draws
    GatherCull, GatherMerge,                               // WMO-doodad gather serial sub-phases: Phase A cull / Phase C merge
    AnimateCPU,                                            // CPU bone/billboard compute + bone upload inside single-instance draw (animate())
    TileStream,                                            // main-thread tile streaming: MapTile ctor/queue + finished-tile GPU upload
    ModelUpload, TexUpload, SpawnLoad,                     // M2 spike hunt (2026-08-04): model VBO upload / BLP texture upload / creature+GO spawn load
    M2Gather, CreatureInject, Clutter, DoodadDraw, IndivDraw, BucketInterior, // M2 spike hunt: WMO gather / creature inject / clutter inject / instanced doodad buckets / individual draws / per-instance bucket_interior (interior_light_at)
    WorldDraw, GpuWait, Selection,                         // coarse (MapView): 3D CPU render / glFinish GPU-wait probe / selection
    FrameSetup, CamVolume,                                 // localize the ~12ms unprofiled-in-WorldDraw: whole pre-Terrain block / just camera_is_inside_wmo volume walk
    PibPrep, PibDrawGL,                                    // IndivDraw split (2026-08-07): billboard-doodad dedupe/group/interior gather vs the serial per-group instanced GL draws
    PaintBody, Frame,                                      // PaintBody = makeCurrent..doneCurrent (in-paint wall); Frame = start..start (whole period). Frame-PaintBody = between-frame (Qt swap/composite/loader).
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
      case Phase::TileStream:   return "TileStream";
      case Phase::ModelUpload:  return "ModelUpload";
      case Phase::TexUpload:    return "TexUpload";
      case Phase::SpawnLoad:    return "SpawnLoad";
      case Phase::M2Gather:     return "M2Gather";
      case Phase::CreatureInject: return "CreatureInject";
      case Phase::Clutter:      return "Clutter";
      case Phase::DoodadDraw:   return "DoodadDraw";
      case Phase::IndivDraw:    return "IndivDraw";
      case Phase::BucketInterior: return "BucketInterior";
      case Phase::WorldDraw:    return "WorldDraw";
      case Phase::GpuWait:      return "GpuWait";
      case Phase::Selection:    return "Selection";
      case Phase::FrameSetup:   return "FrameSetup";
      case Phase::CamVolume:    return "CamVolume";
      case Phase::PibPrep:      return "PibPrep";
      case Phase::PibDrawGL:    return "PibDrawGL";
      case Phase::PaintBody:    return "PaintBody";
      case Phase::Frame:        return "Frame";
      default:                  return "?";
    }
  }

  struct FrameProfiler
  {
    bool on = false;
    double spike_floor_ms = 20.0; // NOGGIT_SPIKE_MS: a frame under this is never a "spike" (avoids noise at high fps)
    double spike_factor = 2.0;    // NOGGIT_SPIKE_FACTOR: ...and it must exceed factor x the rolling baseline

    // per-second report window
    std::array<double, static_cast<std::size_t>(Phase::COUNT)> sum{};
    std::array<double, static_cast<std::size_t>(Phase::COUNT)> mx{}; // worst single frame per phase in the window
    int frames = 0;
    int spikes = 0;
    double worst_frame_ms = 0.0;
    std::chrono::steady_clock::time_point last_report{};

    // current frame (add() writes here; end_frame() folds it into the window)
    std::array<double, static_cast<std::size_t>(Phase::COUNT)> cur{};
    // [finding 96] time spent inside NESTED Scopes, per phase, so exclusive = sum - child_sum
    std::array<double, static_cast<std::size_t>(Phase::COUNT)> child_sum{};
    void add_child(Phase p, double ms)
    {
      if (!on) { return; }
      child_sum[static_cast<std::size_t>(p)] += ms;
    }
    double baseline_ms = 0.0; // EWMA of the frame period; the spike threshold rides on this

    // [SubmitInst breakdown 2026-08-05] accumulate the instanced-doodad draw SHAPE over the report window
    // so we know whether SubmitInst's cost is grouping (groups >> models -> collapse wins) or raw draw-call
    // count (drawcalls ~= models*passes, models huge -> needs persistent buffers/batching). Reset per window.
    long long inst_models = 0, inst_groups = 0, inst_drawcalls = 0, inst_instances = 0;
    void inst_add(int groups, int drawcalls, std::size_t instances)
    {
      if (!on) { return; }
      ++inst_models;
      inst_groups += groups;
      inst_drawcalls += drawcalls;
      inst_instances += static_cast<long long>(instances);
    }

    static FrameProfiler& get()
    {
      static FrameProfiler p = []
      {
        FrameProfiler fp;
        fp.on = std::getenv("NOGGIT_FRAME_PROFILE") != nullptr;
        if (char const* s = std::getenv("NOGGIT_SPIKE_MS"))     { fp.spike_floor_ms = std::atof(s); }
        if (char const* s = std::getenv("NOGGIT_SPIKE_FACTOR")) { fp.spike_factor  = std::atof(s); }
        fp.last_report = std::chrono::steady_clock::now();
        return fp;
      }();
      return p;
    }

    void add(Phase p, double ms) { cur[static_cast<std::size_t>(p)] += ms; }

    // Sum of the non-overlapping top-level phases, for the "unaccounted" bucket. Excludes the M2 sub-
    // splits (subsets of M2) AND TileStream (nested inside Terrain -- shown separately in the spike line).
    double accounted() const
    {
      auto v = [&](Phase p) { return cur[static_cast<std::size_t>(p)]; };
      return v(Phase::LightCollect) + v(Phase::Terrain) + v(Phase::WMO) + v(Phase::M2) + v(Phase::Water)
           + v(Phase::Overlays) + v(Phase::Post) + v(Phase::GpuWait) + v(Phase::Selection);
    }

    // Call once per rendered frame (from MapView after add(Frame, period)).
    void end_frame()
    {
      if (!on)
      {
        return;
      }
      double const total = cur[static_cast<std::size_t>(Phase::Frame)];
      if (total <= 0.0)
      {
        cur.fill(0.0); // no valid Frame period yet (very first frame) -- don't pollute the baseline
        return;
      }

      // SPIKE: a frame notably slower than the recent baseline. Log its breakdown immediately.
      if (baseline_ms > 1.0)
      {
        double const thresh = std::max(spike_floor_ms, spike_factor * baseline_ms);
        if (total > thresh)
        {
          ++spikes;
          double const acc = accounted();
          double const other = total - acc; // hitch OUTSIDE the profiled 3D draw (streaming, upload, driver stall)
          // [unaccounted bisect 2026-08-07] split the hitch into (a) inside paintGL but unprofiled and (b)
          // between paintGL calls (Qt swap/compositor blit/event loop/loader finalize). PaintBody =
          // makeCurrent..doneCurrent measured in MapView; Frame = start..start. Isolates which half stalls.
          double const paint_body = cur[static_cast<std::size_t>(Phase::PaintBody)];
          double const between_frame = (paint_body > 0.0) ? (total - paint_body) : 0.0; // Qt/swap/composite/loader
          double const in_paint_unprof = (paint_body > 0.0) ? (paint_body - acc) : other; // unprofiled work in the body
          // ALL phases (except the coarse Frame/WorldDraw) sorted by cost, so we never miss the culprit.
          std::array<std::pair<double, Phase>, static_cast<std::size_t>(Phase::COUNT)> top;
          for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
          {
            top[i] = {cur[i], static_cast<Phase>(i)};
          }
          std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
          std::ostringstream os;
          os << "[FRAME-SPIKE] frame=" << total << "ms (baseline " << baseline_ms << "ms, x"
             << (total / baseline_ms) << ")  unaccounted=" << other
             << "ms [betweenFrame=" << between_frame << " inPaintUnprof=" << in_paint_unprof << "]  |";
          for (auto const& [ms, ph] : top)
          {
            if (ms >= 0.3 && ph != Phase::Frame && ph != Phase::WorldDraw && ph != Phase::PaintBody)
            {
              os << "  " << phase_name(ph) << "=" << ms;
            }
          }
          LogError << os.str() << std::endl;
        }
      }
      baseline_ms = (baseline_ms <= 0.0) ? total : (0.90 * baseline_ms + 0.10 * total);

      for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
      {
        sum[i] += cur[i];
        if (cur[i] > mx[i]) { mx[i] = cur[i]; }
      }
      if (total > worst_frame_ms) { worst_frame_ms = total; }
      ++frames;
      cur.fill(0.0);

      auto const now = std::chrono::steady_clock::now();
      double const since_ms = std::chrono::duration<double, std::milli>(now - last_report).count();
      if (since_ms < 1000.0 || frames <= 0)
      {
        return;
      }
      double const fps = frames * 1000.0 / since_ms;
      std::ostringstream os;
      os << "[FRAME-PROFILE] " << frames << " frames  " << fps << " fps  spikes=" << spikes
         << "  worst-frame=" << worst_frame_ms << "ms\n  avg-ms:";
      for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
      {
        os << "  " << phase_name(static_cast<Phase>(i)) << "=" << (sum[i] / frames);
      }
      os << "\n  EXCLUSIVE-ms (self, children subtracted):";
      for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
      {
        double const excl = (sum[i] - child_sum[i]) / frames;
        if (excl > 0.005)
        {
          os << "  " << phase_name(static_cast<Phase>(i)) << "=" << excl;
        }
      }
      os << "\n  worst-frame-ms (per phase):";
      for (std::size_t i = 0; i < static_cast<std::size_t>(Phase::COUNT); ++i)
      {
        os << "  " << phase_name(static_cast<Phase>(i)) << "=" << mx[i];
      }
      os << "\n  instanced-doodads: " << inst_models << " model-draws  " << inst_groups
         << " groups  " << inst_drawcalls << " drawElemInstanced  " << inst_instances << " instances"
         << "  (per-frame: " << (inst_models / frames) << " models / " << (inst_groups / frames)
         << " groups / " << (inst_drawcalls / frames) << " draws;  groups/model="
         << (inst_models ? double(inst_groups) / inst_models : 0.0) << ")";
      LogError << os.str() << std::endl;
      inst_models = inst_groups = inst_drawcalls = inst_instances = 0;
      sum.fill(0.0);
      child_sum.fill(0.0);
      mx.fill(0.0);
      frames = 0;
      spikes = 0;
      worst_frame_ms = 0.0;
      last_report = now;
    }
  };

  // RAII: times its scope into the given phase bucket. No-op (bool check only) when profiling is off.
  struct Scoped
  {
    Phase _p;
    bool _on;
    std::chrono::steady_clock::time_point _t0;
    Scoped* _parent = nullptr;

    // per-thread innermost open scope; worker threads keep their own stack, so a phase entered on
    // a pool thread is attributed within that thread rather than to whatever the render thread
    // happened to have open
    static Scoped*& current()
    {
      static thread_local Scoped* c = nullptr;
      return c;
    }

    explicit Scoped(Phase p)
      : _p(p)
      , _on(FrameProfiler::get().on)
    {
      if (_on)
      {
        _parent = current();
        current() = this;
        _t0 = std::chrono::steady_clock::now();
      }
    }
    ~Scoped()
    {
      if (_on)
      {
        double const ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - _t0).count();
        FrameProfiler::get().add(_p, ms);
        current() = _parent;
        if (_parent) { FrameProfiler::get().add_child(_parent->_p, ms); }
      }
    }
    Scoped(Scoped const&) = delete;
    Scoped& operator=(Scoped const&) = delete;
  };
}
