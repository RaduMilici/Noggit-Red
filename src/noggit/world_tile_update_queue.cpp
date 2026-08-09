// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/world_tile_update_queue.hpp>

#include <algorithm>

#include <noggit/Log.h>
#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>


namespace Noggit
{
  struct instance_update
  {
    instance_update() = delete;
    instance_update(instance_update const&) = delete;
    instance_update(instance_update&&) = default;
    instance_update& operator= (instance_update const&) = delete;
    instance_update& operator= (instance_update&&) = default;

    instance_update(SceneObject* obj, model_update type, bool mark_changed_)
      : instance(obj)
      , update_type(type)
      , mark_changed(mark_changed_)
    {

    }

    void apply(World* const world)
    {
      instance->instance_model()->wait_until_loaded();
      auto& extents(instance->getExtents());
      TileIndex start(extents[0]), end(extents[1]);

      // Defensive span clamp. A model with a bad/garbage bounding box (inf/-inf or min>max header boxes,
      // common in fuckported/custom models) yields extents spanning the whole 64x64 map, so this double
      // loop would call update_model_tile (-> loadTile) for THOUSANDS of tiles -> multi-GB blowup and a
      // wedged update thread (main thread then thrashes rendering the ballooning world = "freeze"). No
      // real instance spans more than a couple tiles; cap it at a generous 8x8 so a bad box can never
      // load the map. (Root cause also fixed in ModelInstance::recalcExtents.)
      size_t const z0 = std::min(start.z, end.z);
      size_t const x0 = std::min(start.x, end.x);
      size_t const z1 = std::min<size_t>(std::max(start.z, end.z), z0 + 8);
      size_t const x1 = std::min<size_t>(std::max(start.x, end.x), x0 + 8);

      for (size_t z = z0; z <= z1; ++z)
      {
        for (size_t x = x0; x <= x1; ++x)
        {
          world->mapIndex.update_model_tile(TileIndex(x, z), update_type, instance, mark_changed);
        }
      }
    }

    SceneObject* instance;
    model_update update_type;
    bool mark_changed;
  };

  world_tile_update_queue::world_tile_update_queue(World* world)
    : _world(world)
  {
    _thread = std::make_unique<std::thread>(&world_tile_update_queue::process_queue, this);
  }

  world_tile_update_queue::~world_tile_update_queue()
  {
    _stop = true;
    _state_changed.notify_all();

    _thread->join();

    if (!_update_queue.empty())
    {
      LogError << "Update queue deleted with some update pending !" << std::endl;
    }
  }

  void world_tile_update_queue::wait_for_all_update()
  {
    std::unique_lock<std::mutex> lock (_mutex);

    _state_changed.wait
    ( lock
    , [&]
      {
        return _update_queue.empty();
      }
    );
  }

  void world_tile_update_queue::queue_update(SceneObject* instance, model_update type, bool mark_changed)
  {
    {
      std::lock_guard<std::mutex> const lock(_mutex);

      _update_queue.emplace(new instance_update(instance, type, mark_changed));
      _state_changed.notify_one();
    }
    // make sure deletion are done here
    // otherwise the instance get deleted
    if (type == model_update::remove)
    {
      // wait for all update to make sure they are done in the right order
      wait_for_all_update();
    }
  }

  void world_tile_update_queue::process_queue()
  {
    instance_update* update;

    while(!_stop.load())
    {
      {
        std::unique_lock<std::mutex> lock(_mutex);

        _state_changed.wait
        ( lock
        , [&]
          {
            return _stop.load() || !_update_queue.empty();
          }
        );

        if (_stop.load())
        {
          return;
        }

        update = _update_queue.front().get();
      }

      update->apply(_world);

      {
        std::lock_guard<std::mutex> const lock (_mutex);
        _update_queue.pop();
        _state_changed.notify_all();
      }
    }
  }
}
