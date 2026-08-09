// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <math/frustum.hpp>
#include <noggit/liquid_layer.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/tool_enums.hpp>

#include <vector>
#include <set>
#include <optional>

class sExtendableArray;
class MapChunk;
class TileWater;

namespace BlizzardArchive
{
  class ClientFile;
}

class ChunkWater
{
public:
  ChunkWater() = delete;
  explicit ChunkWater(MapChunk* chunk, TileWater* water_tile, float x, float z, bool use_mclq_green_lava);

  ChunkWater (ChunkWater const&) = delete;
  ChunkWater (ChunkWater&&) = delete;
  ChunkWater& operator= (ChunkWater const&) = delete;
  ChunkWater& operator= (ChunkWater&&) = delete;

  // mcnk_liquid_id: authoritative liquid id from the MCNK header flags (lq_magma/slime/ocean/
  // river), or 0 if the header specifies none -> then the per-tile MCLQ nibble is used.
  void from_mclq(std::vector<mclq>& layers, int mcnk_liquid_id = 0);
  void fromFile(BlizzardArchive::ClientFile& f, size_t basePos);
  void save(sExtendableArray& adt, int base_pos, int& header_pos, int& current_pos);

  // Drop redundant duplicate layers (same liquid id, coverage and height) that the water tool can
  // accumulate, so a chunk doesn't serialize e.g. three identical water blocks. Called before save.
  void removeDuplicateLayers();

  // Vanilla MCLQ serialization: one mclq block per liquid layer.
  void to_mclq(std::vector<mclq>& out) const;
  // MCNK header liquid flag bits (lq_river/ocean/magma/slime) required by the layers.
  mcnk_flags mclq_header_flags() const;

  bool is_visible ( const float& cull_distance
                  , const math::frustum& frustum
                  , const glm::vec3& camera
                  , display_mode display
                  ) const;

  void autoGen(MapChunk* chunk, float factor);
  void CropWater(MapChunk* chunkTerrain);

  void setType(int type, size_t layer);
  int getType(size_t layer) const;
  bool hasData(size_t layer) const;
  void tagUpdate();

  std::vector<liquid_layer>* getLayers() { return &_layers; };

  // update every layer's render
  void update_layers();
  float getMinHeight() { return vmin.y; };
  float getMaxHeight() { return vmax.y; }

  void paintLiquid( glm::vec3 const& pos
                  , float radius
                  , int liquid_id
                  , bool add
                  , math::radians const& angle
                  , math::radians const& orientation
                  , bool lock
                  , glm::vec3 const& origin
                  , bool override_height
                  , bool override_liquid_id
                  , MapChunk* chunk
                  , float opacity_factor
                  );

  MapChunk* getChunk() { return _chunk; };
  TileWater* getWaterTile() { return _water_tile; };
  std::optional<MH2O_Render> Render;
  float xbase, zbase;

private:

  glm::vec3 vmin, vmax, vcenter;
  bool _use_mclq_green_lava;

  // remove empty layers
  void cleanup();

  void copy_height_to_layer(liquid_layer& target, glm::vec3 const& pos, float radius);




  std::vector<liquid_layer> _layers;
  MapChunk* _chunk;
  TileWater* _water_tile;

  friend class MapView;
};
