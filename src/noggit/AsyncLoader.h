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
};
