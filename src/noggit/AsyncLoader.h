// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/AsyncObject.h>

#include <QtCore/QSettings>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <thread>

class AsyncLoader
{
public:
  static AsyncLoader& instance()
  {
    // Worker thread count is user-configurable (Settings -> "Loader threads"), default 3 to match the
    // reference build. More threads stream faster but contend with the main/render thread's CPU while
    // loading; fewer keeps frame time smoother. Created once; a change needs a restart.
    int const threads = std::max(1, QSettings().value("async_thread_count", 8).toInt());
    static AsyncLoader async_loader(threads);
    return async_loader;
  }

  //! Ownership is _not_ transferred. Call ensure_deletable to ensure 
  //! that a previously enqueued object can be destroyed.
  void queue_for_load (AsyncObject*);
  
  void ensure_deletable (AsyncObject*);

  bool is_loading();
  void wait_until_idle();

  // [perf 2026-08-05] Bounded alternative to wait_until_idle for the tile-unload race. pause() stops workers
  // picking up NEW loads and blocks only until the IN-FLIGHT loads finish (<= worker count, ~ms) -- vs
  // wait_until_idle which drains the whole _to_load backlog (hundreds of items during a fast fly = a
  // multi-hundred-ms main-thread stall = the streaming lag spike). While paused the loader is quiescent, so
  // freeing tiles is race-free exactly as under wait_until_idle. resume() MUST be called after (use the RAII
  // LoaderPause guard). Re-entrant pause is not supported -- one pause/resume pair at a time.
  void pause();
  void resume();

  // RAII: pause the loader for the scope, resume on exit (even on exception).
  struct ScopedPause
  {
    ScopedPause() { AsyncLoader::instance().pause(); }
    ~ScopedPause() { AsyncLoader::instance().resume(); }
    ScopedPause(ScopedPause const&) = delete;
    ScopedPause& operator=(ScopedPause const&) = delete;
  };

  AsyncLoader(int numThreads);
  ~AsyncLoader();

  bool important_object_failed_loading() const { return _important_object_failed_loading; }
  void reset_object_fail() { _important_object_failed_loading = false; }

private:
  void process();

  std::mutex _guard;
  std::condition_variable _state_changed;
  std::atomic<bool> _stop;
  std::array<std::list<AsyncObject*>, (size_t)async_priority::count> _to_load;
  std::list<AsyncObject*> _currently_loading;
  std::list<std::thread> _threads;
  bool _important_object_failed_loading = false;
  bool _paused = false; // when set, workers finish their current object then stop picking up new loads
};
