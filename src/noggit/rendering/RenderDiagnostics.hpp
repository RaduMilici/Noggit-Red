// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

namespace Noggit::Rendering
{
  extern unsigned g_gl_draw_instanced;
  extern unsigned g_gl_draw_single;
  extern unsigned g_gl_draw_persistent;
  extern unsigned g_gl_draw_particles;
  extern unsigned g_gl_draw_ribbons;

  extern unsigned g_gl_wmo_draw_calls;
  extern unsigned g_wmo_frame_stamp;
  extern unsigned g_gl_draw_wmo_group;
  extern bool g_vk_owns_wmo;

  extern thread_local int g_last_static_batch_reject;
  extern thread_local int g_last_tex_unit_reject;
  extern thread_local bool g_last_reject_permanent;
  extern thread_local int g_last_rej4_reason;

  extern bool g_noggit_harness_silent;
}
