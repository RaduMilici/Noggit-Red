// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/ray.hpp>
#include <noggit/map_enums.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/Selection.h>
#include <noggit/TileWater.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <opengl/shader.fwd.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/Misc.h>
#include <external/tsl/robin_map.h>
#include <noggit/rendering/TileRender.hpp>
#include <noggit/rendering/FlightBoundsRender.hpp>

#include <map>
#include <string>
#include <vector>
#include <array>

namespace math
{
  class frustum;
  struct vector_3d;
}

namespace Noggit::Rendering
{
  class TileRender;
  class FlightBoundsRender;
}

class World;


class MapTile : public AsyncObject
{
  friend class Noggit::Rendering::TileRender;
  friend class Noggit::Rendering::FlightBoundsRender;
  friend class MapChunk;
  friend class TextureSet;

public:
	MapTile( int x0
         , int z0
         , std::string const& pFilename
         , bool pBigAlpha
         , bool pLoadModels
         , bool use_mclq_green_lava
         , bool reloading_tile
         , World*
         , Noggit::NoggitRenderContext context
         , tile_mode mode = tile_mode::edit
         , bool pLoadTextures = true
         );
  ~MapTile();

  void finishLoading() override;
  void waitForChildrenLoaded() override;

  //! \todo on destruction, unload ModelInstances and WMOInstances on this tile:
  // a) either keep up the information what tiles the instances are on at all times
  //    (even while moving), to then check if all tiles it was on were unloaded, or
  // b) do the reference count lazily by iterating over all instances and checking
  //    what MapTiles they span. if any of those tiles is still loaded, keep it,
  //    otherwise remove it.
  //
  // I think b) is easier. It only requires
  // `std::set<C2iVector> XInstance::spanning_tiles() const` followed by
  // `if_none (isTileLoaded (x, y)): unload instance`, which is way easier than
  // constantly updating the reference counters.
  // Note that both approaches do not cover the issue that the instance might not
  // be saved to any tile, thus the movement might have been lost.

	//! \brief Get the maximum height of terrain on this map tile.
	float getMaxHeight();
	float getMinHeight();
  void forceRecalcExtents();

  void convert_alphamap(bool to_big_alpha);

  //! \brief Get chunk for sub offset x,z.

  [[nodiscard]]
  MapChunk* getChunk(unsigned int x, unsigned int z);
  //! \todo map_index style iterators

  [[nodiscard]]
  std::vector<MapChunk*> chunks_in_range (glm::vec3 const& pos, float radius) const;

  [[nodiscard]]
  std::vector<MapChunk*> chunks_in_rect (glm::vec3 const& pos, float radius) const;

  const TileIndex index;
  float xbase, zbase;

  std::atomic<bool> changed;

  // [perf 2026-08-06] Set by the renderer for every IN-FRUSTUM tile each frame; read by MapIndex::unloadTiles so
  // a tile still on screen is NEVER unloaded even when it sits beyond unload_dist. Without this, visible far
  // tiles (e.g. ocean at render distance) unload and render as empty flat quads -- the bug the pinned-changed
  // memory leak used to mask. unloadTiles clears it each pass; the next frame's render re-sets it while the tile
  // stays visible, so residency is bounded to ~frustum coverage (no unbounded growth).
  std::atomic<bool> rendered_recently{false};


  bool intersect (math::ray const&, selection_result*) const;


  bool GetVertex(float x, float z, glm::vec3 *V);
  void getVertexInternal(float x, float z, glm::vec3* v);

  void saveTile(World*);
	void CropWater();

  bool isTile(int pX, int pZ);

  async_priority loading_priority() const override
  {
    return async_priority::high;
  }

  bool has_model(uint32_t uid) const
  {
    return std::find(uids.begin(), uids.end(), uid) != uids.end();
  }

  void remove_model(uint32_t uid);
  void remove_model(SceneObject* instance);
  void add_model(uint32_t uid);
  void add_model(SceneObject* instance);

  TileWater Water;

  bool tile_is_being_reloaded() const { return _tile_is_being_reloaded; }

  std::vector<uint32_t>* get_uids() { return &uids; }

  void initEmptyChunks();

  void setFilename(const std::string& new_filename) {_file_key.setFilepath(new_filename);};

  QImage getHeightmapImage(float min_height, float max_height);
  QImage getAlphamapImage(unsigned layer);
  QImage getAlphamapImage(std::string const& filename);
  QImage getVertexColorsImage();
  QImage getNormalmapImage();
  void setHeightmapImage(QImage const& baseimage, float multiplier, int mode, bool tiledEdges);
  void setAlphaImage(QImage const& image, unsigned layer);
  void setVertexColorImage(QImage const& image, int mode, bool tiledEdges);
  void registerChunkUpdate(unsigned flags) { _chunk_update_flags |= flags; };
  void endChunkUpdates() { _chunk_update_flags = 0; };
  std::array<float, 145 * 256 * 4>& getChunkHeightmapBuffer() { return _chunk_heightmap_buffer; };
  unsigned getChunkUpdateFlags() { return _chunk_update_flags; }
  void recalcExtents();
  void recalcObjectInstanceExtents();
  void recalcCombinedExtents();
  std::array<glm::vec3, 2>& getExtents() { return _extents; };
  std::array<glm::vec3, 2>& getCombinedExtents() { return _combined_extents; };

  World* getWorld() { return _world; };

  [[nodiscard]]
  tsl::robin_map<AsyncObject*, std::vector<SceneObject*>> const& getObjectInstances() const { return object_instances; };

  // Per-chunk object buckets (client MCRF semantics, checklist D1): render-only acceleration.
  // Each of the 16x16 chunk cells lists the instances whose AABB overlaps it, plus the union AABB
  // of those instances (can exceed the cell: objects overhang). On a partially-visible tile the
  // renderer frustum-tests the ~33yd buckets and only per-instance-tests survivors, instead of
  // testing every instance on the tile. Rebuilt lazily on the render thread whenever the tile's
  // object set changes (add/remove/move all re-register through add_model/remove_model).
  struct ObjectBucket
  {
    std::vector<std::pair<AsyncObject*, SceneObject*>> instances;
    glm::vec3 aabb_min = glm::vec3(0.0f);
    glm::vec3 aabb_max = glm::vec3(0.0f);
  };

  std::array<ObjectBucket, 256> const& getObjectBuckets();

  // [perf 2026-08-05] Persistent per-model doodad instance buffers (TileRender). This dirty flag rides the
  // SAME add/remove/move signal as _object_buckets_dirty (moves = remove+add through updateTilesEntry), so
  // an edit that invalidates the cull buckets also invalidates the persistent GPU buffers -- TileRender
  // rebuilds them lazily on next draw. No new invalidation surface.
  bool doodadBuffersDirty() const { return _doodad_instance_buffers_dirty; }
  void clearDoodadBuffersDirty() { _doodad_instance_buffers_dirty = false; }

  float camDist() { return _cam_dist; }
  void calcCamDist(glm::vec3 const& camera);
  void markExtentsDirty() { _extents_dirty = true; }
  void tagCombinedExtents(bool state) { _combined_extents_dirty = state; };

  Noggit::Rendering::TileRender* renderer() { return &_renderer; };
  Noggit::Rendering::FlightBoundsRender* flightBoundsRenderer() { return &_fl_bounds_render; };

private:

  tile_mode _mode;
  bool _tile_is_being_reloaded;

  bool _extents_dirty = true;
  bool _combined_extents_dirty = true;
  bool _requires_object_extents_recalc = true;


  std::array<glm::vec3, 2> _extents;
  std::array<glm::vec3, 2> _object_instance_extents;
  std::array<glm::vec3, 2> _combined_extents;
  glm::vec3 _center;
  float _cam_dist;

  // MFBO:
  glm::vec3 mMinimumValues[3 * 3];
  glm::vec3 mMaximumValues[3 * 3];

  unsigned _chunk_update_flags;

  // MHDR:
  int mFlags = 0;
  bool mBigAlpha;

  // Data to be loaded and later unloaded.
  std::vector<std::string> mTextureFilenames;
  std::vector<std::string> mModelFilenames;
  std::vector<std::string> mWMOFilenames;
  
  std::vector<uint32_t> uids;
  tsl::robin_map<AsyncObject*, std::vector<SceneObject*>> object_instances; // only includes M2 and WMO. perhaps a medium common ancestor then?

  std::array<ObjectBucket, 256> _object_buckets;
  bool _object_buckets_dirty = true;
  bool _doodad_instance_buffers_dirty = true; // persistent per-model doodad buffers (TileRender); see doodadBuffersDirty()
  void rebuildObjectBuckets();

  std::unique_ptr<MapChunk> mChunks[16][16];
  std::array<float, 145 * 256 * 4> _chunk_heightmap_buffer;

  bool _load_models;
  bool _load_textures;
  World* _world;


  Noggit::Rendering::TileRender _renderer;
  Noggit::Rendering::FlightBoundsRender _fl_bounds_render;

  Noggit::NoggitRenderContext _context;

};
