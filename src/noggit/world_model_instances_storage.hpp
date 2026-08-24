// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/ModelInstance.h>
#include <noggit/Selection.h>
#include <noggit/TileIndex.hpp>
#include <noggit/WMOInstance.h>
#include <opengl/scoped.hpp>
#include <variant>
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class World;

using m2_instance_umap = std::unordered_map<std::uint32_t, ModelInstance>;
using wmo_instance_umap = std::unordered_map<std::uint32_t, WMOInstance>;

namespace Noggit
{
  class world_model_instances_storage
  {
  public:
    world_model_instances_storage(World* world);

    world_model_instances_storage() = delete;
    world_model_instances_storage(world_model_instances_storage const&) = delete;
    world_model_instances_storage(world_model_instances_storage&&) = delete;
    world_model_instances_storage& operator= (world_model_instances_storage const&) = delete;
    world_model_instances_storage& operator= (world_model_instances_storage&&) = delete;

    // perform uid duplicate check, return the uid of the stored instance
    std::uint32_t add_model_instance(ModelInstance instance, bool from_reloading);
    // perform uid duplicate check, return the uid of the stored instance
    std::uint32_t add_wmo_instance(WMOInstance instance, bool from_reloading);

    std::optional<ModelInstance*> get_model_instance(std::uint32_t uid);
    std::optional<WMOInstance*> get_wmo_instance(std::uint32_t uid);
    std::optional<selection_type> get_instance(std::uint32_t uid, bool lock=true);

    void delete_instances_from_tile(TileIndex const& tile);
    void delete_instances(std::vector<selection_type> const& instances);
    void delete_instance(std::uint32_t uid);
    void unload_instance_and_remove_from_selection_if_necessary(std::uint32_t uid);

    void clear();

    void clear_duplicates();

    bool uid_duplicates_found() const
    {
      return _uid_duplicates_found.load();
    }

    void upload();
    void unload();

    unsigned int getTotalModelsCount() const { return _m2s.size() + _wmos.size(); };

    // [20.4 light-collection cache] Monotonic counter bumped on any instance mutation reaching
    // World::updateTilesModel/updateTilesWMO (add/move/remove/doodad-set change) plus clear().
    // WorldRender's point-light registry re-derives its uid lists only when this changes; on
    // unchanged epochs the light collection walks ONLY registered light carriers instead of
    // every instance. Relaxed atomics: a late-observed bump just delays the re-derive one tick.
    void bump_light_epoch() { _light_epoch.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t light_epoch() const { return _light_epoch.load(std::memory_order_relaxed); }

    // Locked uid-list visitors for the registry walks. Missing uids are skipped (a delete always
    // bumps the epoch, so stale entries only survive until the next registry re-derive).
    template<typename Fun>
      void visit_m2_uids(std::vector<std::uint32_t> const& uids, Fun&& fn)
    {
      std::unique_lock<std::mutex> const lock (_mutex);
      for (auto const uid : uids)
      {
        auto it = _m2s.find(uid);
        if (it != _m2s.end())
        {
          fn(it->second);
        }
      }
    }

    template<typename Fun>
      void visit_wmo_uids(std::vector<std::uint32_t> const& uids, Fun&& fn)
    {
      std::unique_lock<std::mutex> const lock (_mutex);
      for (auto const uid : uids)
      {
        auto it = _wmos.find(uid);
        if (it != _wmos.end())
        {
          fn(it->second);
        }
      }
    }

  private: // private functions aren't thread safe
    inline bool unsafe_uid_is_used(std::uint32_t uid) const;

    std::uint32_t unsafe_add_model_instance_no_world_upd(ModelInstance instance);
    std::uint32_t unsafe_add_wmo_instance_no_world_upd(WMOInstance instance);
    std::optional<ModelInstance*> unsafe_get_model_instance(std::uint32_t uid);
    std::optional<WMOInstance*> unsafe_get_wmo_instance(std::uint32_t uid);

  public:
    template<typename Fun>
      void for_each_wmo_instance(Fun&& function)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _wmos)
      {
        function(it.second);
      }
    }

    template<typename Fun, typename Stop>
      void for_each_wmo_instance(Fun&& function, Stop&& stop_cond)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _wmos)
      {
        function(it.second);

        if (stop_cond())
        {
          break;
        }
      }
    }

    template<typename Fun>
      void for_each_m2_instance(Fun&& function)
    {
      std::unique_lock<std::mutex> const lock (_mutex);

      for (auto& it : _m2s)
      {
        function(it.second);
      }
    }

  private:
    World* _world;
    std::mutex _mutex;
    std::atomic<bool> _uid_duplicates_found = {false};
    std::atomic<std::uint64_t> _light_epoch = {0};

    m2_instance_umap _m2s;
    wmo_instance_umap _wmos;

    OpenGL::Scoped::deferred_upload_buffers<1> _buffers;
    GLuint const& _m2_instances_transform_buf = _buffers[0];
    GLuint _m2_instances_transform_buf_tex;
    std::uint32_t _n_allocated_m2_transforms = 4096;
    std::uint32_t _n_used_m2_transforms = 0;
    bool _transform_storage_uploaded = false;

    std::unordered_map<std::uint32_t, int> _instance_count_per_uid;
  };
}
