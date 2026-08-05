// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/map_enums.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

class SceneObject;
class World;

namespace Noggit
{
  struct instance_update;

  class world_tile_update_queue
  {
  public:
    world_tile_update_queue(World* world);
    ~world_tile_update_queue();

    world_tile_update_queue() = delete;
    world_tile_update_queue(world_tile_update_queue const&) = delete;
    world_tile_update_queue(world_tile_update_queue&&) = delete;
    world_tile_update_queue& operator= (world_tile_update_queue const&) = delete;
    world_tile_update_queue& operator= (world_tile_update_queue&&) = delete;

    void wait_for_all_update();

    // mark_changed=false: register the instance into its tiles WITHOUT flagging them dirty/unsaved. Used by
    // the load-time path (UID-collision reassignment / tile reload) so streamed-in tiles stay UNLOADABLE --
    // flagging them changed pinned them resident forever = unbounded memory growth on a long fly. Real user
    // edits keep the default true so their changes still persist/flag for save. [perf 2026-08-05]
    void queue_update(SceneObject* instance, model_update type, bool mark_changed = true);

  private:
    void process_queue();
  private:
    World* _world;

    std::atomic<bool> _stop = {false};
    std::mutex _mutex;
    std::condition_variable _state_changed;

    // only use one thread
    std::unique_ptr<std::thread> _thread;

    std::queue<std::unique_ptr<instance_update>> _update_queue;
  };
}
