// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

// [VULKAN phase G] Per-frame mirror of the M2 PARTICLE quads.
//
// GL builds every emitter's quads on the CPU each frame (ParticleSystem::draw) and streams them into
// its own VBOs. Vulkan needs the same stream, so the emitter appends a WORLD-SPACE copy here: the
// instance transform and the billboard offset are already folded in, which keeps the VK vertex to
// position + uv + colour and removes the per-draw transform attribute GL uses.
//
// One Draw per emitter draw, in the order GL issues them -- particles are order-dependent.

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Rendering::VK
{
  struct ParticleFeed
  {
    struct Draw
    {
      std::uint32_t first_index = 0;
      std::uint32_t index_count = 0;
      std::int32_t  base_vertex = 0;
      int           blend = 0;          // M2 particle blend mode (0..7)
      float         alpha_test = 0.f;
      float         alpha_mod = 1.f;    // CreatureModelAlpha
      bool          ribbon = false;     // ribbons skip the particle black-fringe divide
      std::string   blp;                // emitter texture, resolved to a bindless id by MapView
    };

    std::vector<float>         vertices;   // pos xyz | uv | rgba  (9 floats)
    std::vector<std::uint32_t> indices;
    std::vector<Draw>          draws;
    std::size_t                skipped = 0;   // emitter draws VK could not mirror (tracked TODO)

    void clear() { vertices.clear(); indices.clear(); draws.clear(); skipped = 0; }
  };

  // One global feed: the particle pass is single-threaded and runs once per frame.
  ParticleFeed& particleFeed();

  // Set by ModelRender::drawParticlesForInstance around the emitter draw, because the transform GL
  // uses lives in a VBO the emitter itself cannot read back.
  struct ParticleDrawContext
  {
    float transform[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float alpha_mod = 1.f;
    bool  valid = false;
  };
  ParticleDrawContext& particleDrawContext();

  // Ribbons are drawn INSTANCED from a transform list the emitter cannot read back either.
  struct RibbonDrawContext
  {
    std::vector<float> transforms;   // 16 floats per instance
    bool valid = false;
  };
  RibbonDrawContext& ribbonDrawContext();
  bool& vkOwnsRibbons();

  // True while Vulkan draws the particles, so GL skips its own emitter draws.
  bool& vkOwnsParticles();
}
