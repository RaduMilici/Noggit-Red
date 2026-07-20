// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <noggit/WMO.h>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

// A WMO indoor group's world-space AABB plus what's needed to light an object standing inside it the way
// the client does (RE_notes/15): transform the object into WMO-local space and raycast-sample the baked
// MOCV floor colour under it (WMOGroup::sample_ground_color). Holds its own wmo reference so a cached
// volume can never dangle if the instance is deleted between refreshes.
struct InteriorVolume
{
  glm::vec3 min;
  glm::vec3 max;
  glm::mat4 inv_transform;   // world -> WMO local
  scoped_wmo_reference wmo;  // keep-alive reference to the WMO model
  int group_index;
};

// A WMO MFOG entry resolved to world space, for the per-frame ENTITY fog (the camera's fog context):
// full inside r1, fading to the zone fog at r2. fog_start_abs is already the ABSOLUTE distance
// (WMOFog::init multiplies the authored scaler by the end distance).
class WMO;
class WMOGroup;

// One WMO group's world-space AABB for the CAMERA-fog resolve (client-exact, wow.exe @0069de20,
// docs/client_re/27): the camera's current group's MOGP fog indices drive the entity fog. Only
// groups of WMOs with MORE than the default MFOG entry are collected (default-only WMOs never
// override the zone fog -- the client evaluator bails on nFogs == 1).
struct WmoGroupFogVolume
{
  glm::vec3 min;
  glm::vec3 max;
  WMO* wmo;
  WMOGroup const* group;
  glm::mat4x4 transform;
};
