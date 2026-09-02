// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/rendering/vulkan/VkParticleFeed.hpp>

namespace Noggit::Rendering::VK
{
  ParticleFeed& particleFeed()
  {
    static ParticleFeed s_feed;
    return s_feed;
  }

  ParticleDrawContext& particleDrawContext()
  {
    static ParticleDrawContext s_ctx;
    return s_ctx;
  }

  bool& vkOwnsParticles()
  {
    static bool s_owns = false;
    return s_owns;
  }

  RibbonDrawContext& ribbonDrawContext()
  {
    static RibbonDrawContext s_ctx;
    return s_ctx;
  }

  bool& vkOwnsRibbons()
  {
    static bool s_owns = false;
    return s_owns;
  }
}
