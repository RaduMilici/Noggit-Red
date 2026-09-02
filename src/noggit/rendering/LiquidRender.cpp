// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include "LiquidRender.hpp"
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/MapChunk.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/rendering/TileRender.hpp>

using namespace Noggit::Rendering;

LiquidRender::LiquidRender(MapTile* map_tile)
: _map_tile(map_tile)
{
}

void LiquidRender::draw(math::frustum const& frustum
    , const glm::vec3& camera
    , bool camera_moved
    , OpenGL::Scoped::use_program& water_shader
    , int animtime
    , int layer
    , display_mode display
    , LiquidTextureManager* tex_manager
)
{
  // [finding 162] PERMANENT WATER HOLES. _has_data is recomputed in exactly ONE place -- inside the
  // `if (_need_buffer_update)` block below -- and this early return sits above it. So the moment a
  // tile evaluates false it can never recover: the only code that could set it true again is
  // unreachable. A tile drawn once before its water finished parsing, or one whose buffers were
  // re-tagged by unload(), is then skipped for the rest of the session even though its water is
  // present and loaded. The result is per-tile water holes whose pattern depends on load timing,
  // which is what "water is patchy / has chunk holes" looks like from the editor.
  //
  // Letting a PENDING BUFFER UPDATE through is what makes it recoverable: the block below re-derives
  // _has_data from the chunks. If it is still false afterwards, _render_layers stays empty and the
  // draw below iterates nothing, so this costs a walk of the tile's chunks and draws nothing.
  if (!_map_tile->Water.hasData() && !_need_buffer_update)
  {
    static int logged_empty_water_tiles = 0;
    if (logged_empty_water_tiles < 20)
    {
      LogError << "Turtle water: draw skipped, tile has no water data "
               << _map_tile->index.x << "," << _map_tile->index.z << std::endl;
      logged_empty_water_tiles++;
    }
    return;
  }

  constexpr int N_SAMPLERS = 14;
  static std::vector<int> samplers_upload_buf {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

  updateLayerData(tex_manager);

  gl.activeTexture(GL_TEXTURE1);
  gl.bindTexture(GL_TEXTURE_2D_ARRAY, _map_tile->renderer()->shadowmapTexture());

  if (_map_tile->Water._extents_changed)
  {
    _map_tile->Water.recalcExtents();
  }

  std::size_t n_render_blocks = _render_layers.size();

  for (auto& render_layer : _render_layers)
  {
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::CHUNK_LIQUID_INSTANCE_INDEX, render_layer.chunk_data_buf, 0, sizeof(OpenGL::LiquidChunkInstanceDataUniformBlock) * 256);

    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, render_layer.vertex_data_tex);

    if (render_layer.texture_samplers.size() > N_SAMPLERS)
        [[unlikely]] // multi draw-call mode
    {
      // TODO:
    }
    else
    {
      std::fill(samplers_upload_buf.begin(), samplers_upload_buf.end(), -1);

      for (std::size_t j = 0; j < render_layer.texture_samplers.size(); ++j)
      {
        samplers_upload_buf[j] = render_layer.texture_samplers[j];
      }

      for (std::size_t j = 0; j < N_SAMPLERS; ++j)
      {
        if (samplers_upload_buf[j] < 0)
          break;

        gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + 2 + j));
        gl.bindTexture(GL_TEXTURE_2D_ARRAY, samplers_upload_buf[j]);
      }

      gl.drawArraysInstanced(GL_TRIANGLES, 0, 8 * 8 * 6, render_layer.n_used_chunks);
    }

  }


}
void LiquidRender::updateLayerData(LiquidTextureManager* tex_manager)
{
  tsl::robin_map<unsigned, std::tuple<GLuint, glm::vec2, int, unsigned>> const& tex_frames = tex_manager->getTextureFrames();

  if (tex_frames.empty())
  {
    static bool logged_empty_texture_profiles = false;
    if (!logged_empty_texture_profiles)
    {
      LogError << "Turtle water: no liquid texture profiles uploaded" << std::endl;
      logged_empty_texture_profiles = true;
    }
    return;
  }

  // create opengl resources if needed
  if (_need_buffer_update)
  {
    _map_tile->Water._has_data = false;

    std::size_t layer_counter = 0;
    for(;;)
    {
      std::size_t n_chunks = 0;
      for (std::size_t z = 0; z < 16; ++z)
      {
        for (std::size_t x = 0; x < 16; ++x)
        {
          ChunkWater* chunk = _map_tile->Water.chunks[z][x].get();

          if (layer_counter >= chunk->getLayers()->size())
            continue;

          if (!_map_tile->Water._has_data)
          {
            _map_tile->Water._has_data = chunk->hasData(layer_counter);
          }

          liquid_layer& layer = (*chunk->getLayers())[layer_counter];

          // create layer
          if (layer_counter >= _render_layers.size())
          {
            auto& render_layer = _render_layers.emplace_back();

            gl.genTextures(1, &render_layer.vertex_data_tex);
            gl.bindTexture(GL_TEXTURE_2D_ARRAY, render_layer.vertex_data_tex);
            gl.texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, 9, 9, 256, 0, GL_RGBA, GL_FLOAT, nullptr);
            gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.texParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

            gl.genBuffers(1, &render_layer.chunk_data_buf);
            gl.bindBuffer(GL_UNIFORM_BUFFER, render_layer.chunk_data_buf);
            gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::LiquidChunkInstanceDataUniformBlock) * 256, NULL, GL_DYNAMIC_DRAW);}

          auto& layer_params = _render_layers[layer_counter];

          // fill per-chunk data
          auto tex_profile_it = tex_frames.find(layer.liquidID());
          if (tex_profile_it == tex_frames.end())
          {
            // [finding 163] THE CHUNK HOLES. This used to `continue`, dropping the chunk entirely --
            // one chunk-sized hole in the water surface for every liquid whose id has no texture
            // profile. Turtle's custom maps use liquid ids that are not in the stock liquid tables,
            // so whole runs of chunks vanished; stock Azeroth never hits it, which is why the parity
            // cameras never caught it. The id only selects which TEXTURE to animate -- the surface
            // geometry, height and depth are all already built -- so an unknown id is no reason not
            // to draw the water. Fall back to plain water (LiquidType 1), then to whatever profile
            // exists, and only give up if the manager has nothing at all.
            tex_profile_it = tex_frames.find(1u);
            if (tex_profile_it == tex_frames.end() && !tex_frames.empty())
              tex_profile_it = tex_frames.begin();
            static int logged_missing_liquid_profiles = 0;
            if (logged_missing_liquid_profiles < 40)
            {
              LogError << "Turtle water: missing liquid profile " << layer.liquidID()
                       << (tex_profile_it == tex_frames.end()
                             ? ", and no fallback profile exists -- chunk not drawn"
                             : ", drawing with the fallback profile") << std::endl;
              logged_missing_liquid_profiles++;
            }
            if (tex_profile_it == tex_frames.end())
              continue;
          }

          std::tuple<GLuint, glm::vec2, int, unsigned> const& tex_profile = tex_profile_it->second;
          OpenGL::LiquidChunkInstanceDataUniformBlock& params_data = layer_params.chunk_data[n_chunks];

          params_data.xbase = layer.getChunk()->xbase;
          params_data.zbase = layer.getChunk()->zbase;

          GLuint tex_array = std::get<0>(tex_profile);
          auto it = std::find(layer_params.texture_samplers.begin(), layer_params.texture_samplers.end(), tex_array);

          unsigned sampler_index = 0;

          if (it != layer_params.texture_samplers.end())
          {
            sampler_index = std::distance(layer_params.texture_samplers.begin(), it);
          }
          else
          {
            sampler_index = static_cast<unsigned int>(layer_params.texture_samplers.size());
            layer_params.texture_samplers.emplace_back(std::get<0>(tex_profile));
          }

          params_data.texture_array = sampler_index;
          params_data.type = std::get<2>(tex_profile);
          params_data.n_texture_frames = std::get<3>(tex_profile);

          glm::vec2 anim = std::get<1>(tex_profile);
          params_data.anim_u = anim.x;
          params_data.anim_v = anim.y;

          std::uint64_t subchunks = layer.getSubchunks();

          // Overlapping water planes: where two liquid layers in this chunk cover the same
          // 8x8 sub-tile, render ONLY the topmost one. Stacking translucent planes via alpha
          // blending darkened the water (Timbermaw etc.). Mask out sub-tiles of this layer that
          // a HIGHER layer also covers (ties broken by layer index so duplicates collapse to one).
          {
            auto& all_layers = *chunk->getLayers();
            for (std::size_t li = 0; li < all_layers.size(); ++li)
            {
              if (li == layer_counter)
                continue;

              bool const other_is_above =
                  all_layers[li].max() > layer.max()
                  || (all_layers[li].max() == layer.max() && li < layer_counter);

              if (other_is_above)
                subchunks &= ~all_layers[li].getSubchunks();
            }
          }

          params_data.subchunks_1 = subchunks & 0xFF'FF'FF'FF;
          params_data.subchunks_2 = subchunks >> 32;
          params_data._pad1 = static_cast<unsigned>(x * 16 + z);

          // fill vertex data
          auto& vertices = layer.getVertices();
          auto& tex_coords = layer.getTexCoords();
          auto& depth = layer.getDepth();

          // Derive a SEAMLESS render depth from the continuous terrain height under the water
          // rather than the per-chunk MCLQ/MH2O depth bytes. Those file bytes are inconsistent
          // at tile boundaries (adjacent chunks disagree at shared vertices -> hard color/alpha
          // steps, and the semi-transparent water revealed the bumpy seafloor unevenly = the
          // "tile" seams). Terrain is shared across chunks, so terrain-derived depth is
          // continuous and tiles connect cleanly. Render-only: layer._depth (saved to the ADT)
          // is untouched. We pack the RAW depth (world units of water above the bottom); the
          // shader maps it to a BROAD color gradient (light shore -> dark deep "fatigue" water)
          // and a STEEPER opacity (hides the per-tile seafloor) independently.
          // Use the CURRENT terrain chunk from the tile (the stored MapChunk* in ChunkWater can
          // be stale -> crash); this is the same fresh pointer the autoGen/opacity tools pass in.
          MapChunk* terrain = _map_tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));
          glm::vec3 const* heightmap = terrain ? terrain->getHeightmap() : nullptr;

          // [2026-08-21 WPL "too bright" fix] The client's water color factor is the AUTHORED
          // Water depth for the colour/alpha ramps = the MAX of the physical terrain thinness and the
          // AUTHORED MCLQ/MH2O depth byte. [2026-08-23: RESTORED the authored term after a wrong detour.]
          // The client bakes the water colour/alpha from the AUTHORED depth (the artist-painted 0..255
          // shallow->deep gradient, verified: Azeroth_36_29 MCLQ carries a real 0..244 spread), NOT the
          // physical terrain. Using terrain-only broke Caer Darrow (a STEEP crater lake): the terrain
          // drops away everywhere so terrain-diff read uniformly DEEP -> the whole lake snapped to one
          // dark colour with no coast gradient (user report). The authored byte preserves the real
          // shallow-edge -> deep-centre gradient and is streaming-STABLE. authored_deep_units scales the
          // normalized byte so 255 saturates this liquid type's colour ramp (river 0.05/u, ocean 0.012/u).
          int const liquid_cat = layer.mclq_liquid_type(); // 0 water, 1 ocean, 2 magma, 3 slime
          float const authored_deep_units = layer.hasAuthoredDepth()
            ? ((liquid_cat == 1) ? (1.0f / 0.012f) : (1.0f / 0.05f))
            : 0.0f;

          for (int z_v = 0; z_v < 9; ++z_v)
          {
            for (int x_v = 0; x_v < 9; ++x_v)
            {
              const unsigned v_index = z_v * 9 + x_v;
              glm::vec2& tex_coord = tex_coords[v_index];
              // Depth = MAX(physical terrain thinness, authored byte x ramp scale). [2026-08-23 final,
              // after two wrong detours the same day:] the authored MCLQ byte is a LINEAR depth
              // measure, not a full-ramp saturator — ordinary rivers author TINY bytes (Elwynn
              // 31_49/32_49: max 6-55, mean 0-9) so authored-ONLY collapsed every river to the flat
              // shallow colour (user: "one flat color, no coast gradient"); their gradient comes from
              // TERRAIN. WPL's murk pools author high bytes (~255 -> saturate the ramp) so max() still
              // gives them the deep murk colour over physically-shallow beds (the 08-21 intent). The
              // earlier Caer Darrow "whole lake snaps dark" was the STREAMING FALLBACK (byte*100 while
              // the terrain heightmap loads), not max() — keep that fallback on the same authored
              // scale instead so un-streamed tiles look like their streamed selves.
              float render_depth = depth[v_index] * (layer.hasAuthoredDepth() ? authored_deep_units : 8.f);
              if (heightmap)
              {
                float const diff = vertices[v_index].y - heightmap[17 * z_v + x_v].y;
                render_depth = std::max(std::max(0.f, diff), depth[v_index] * authored_deep_units);
              }
              layer_params.vertex_data[n_chunks][v_index] = glm::vec4(vertices[v_index].y, render_depth, tex_coord.x, tex_coord.y);
            }
          }

          n_chunks++;
        }
      }


      if (!n_chunks) // break and clean-up
      {
        if (long diff = static_cast<long>(_render_layers.size() - layer_counter); diff > 0)
        {
          for (int i = 0; i < diff; ++i)
          {
            auto& layer_params = _render_layers.back();

            gl.deleteBuffers(1, &layer_params.chunk_data_buf);
            gl.deleteTextures(1, &layer_params.vertex_data_tex);

            _render_layers.pop_back();
          }
        }

        break;
      }
      else
      {
        auto& layer_params = _render_layers[layer_counter];
        layer_params.n_used_chunks = static_cast<unsigned int>(n_chunks);

        static int logged_render_layers = 0;
        if (logged_render_layers < 40)
        {
          LogError << "Turtle water: render layer " << layer_counter << " tile "
                   << _map_tile->index.x << "," << _map_tile->index.z
                   << " chunks " << n_chunks
                   << " samplers " << layer_params.texture_samplers.size() << std::endl;
          logged_render_layers++;
        }

        gl.bindTexture(GL_TEXTURE_2D_ARRAY, layer_params.vertex_data_tex);
        gl.bindBuffer(GL_UNIFORM_BUFFER, layer_params.chunk_data_buf);

        gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LiquidChunkInstanceDataUniformBlock) * 256, layer_params.chunk_data.data());
        gl.texSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 9, 9, 256, GL_RGBA, GL_FLOAT, layer_params.vertex_data.data());
      }

      layer_counter++;
    }

    _need_buffer_update = false;
  }
}


void LiquidRender::unload()
{
  _need_buffer_update = true;

  for (auto& render_layer : _render_layers)
  {
    gl.deleteBuffers(1, &render_layer.chunk_data_buf);
    gl.deleteTextures(1, &render_layer.vertex_data_tex);
  }

  _render_layers.clear();

}

void LiquidRender::upload()
{

}
