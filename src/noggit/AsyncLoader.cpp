// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/errorHandling.h>
#include <Exception.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <cstdlib>
#include <list>

namespace
{
  bool async_loader_trace_enabled()
  {
    static bool const enabled = std::getenv("NOGGIT_ASYNC_LOADER_TRACE") != nullptr;
    return enabled;
  }

  std::string async_object_key(AsyncObject const* object)
  {
    if (!object)
    {
      return {};
    }

    auto const& key = object->file_key();
    if (key.hasFilepath())
    {
      return key.filepath();
    }

    return key.hasFileDataID() ? std::to_string(key.fileDataID()) : std::string();
  }
}

bool AsyncLoader::is_loading()
{
  std::lock_guard<std::mutex> const lock (_guard);
  return !_currently_loading.empty();
}

void AsyncLoader::wait_until_idle()
{
  std::unique_lock<std::mutex> lock(_guard);
  _state_changed.wait(lock, [&]
  {
    return std::all_of(_to_load.begin(), _to_load.end(),
                       [](auto const& to_load) { return to_load.empty(); })
        && _currently_loading.empty();
  });
}

void AsyncLoader::process()
{
  AsyncObject* object = nullptr;

  QSettings settings;
  bool additional_log = settings.value("additional_file_loading_log", false).toBool();

  while (!_stop)
  {
    {    
      std::unique_lock<std::mutex> lock (_guard);

      _state_changed.wait 
      ( lock
      , [&]
        {
          return !!_stop || std::any_of ( _to_load.begin(), _to_load.end()
                                        , [](auto const& to_load) { return !to_load.empty(); }
                                        );
        }
      );

      if (_stop)
      {
        return;
      }

      for (auto& to_load : _to_load)
      {
        if (to_load.empty())
        {
          continue;
        }

        object = to_load.front();
        _currently_loading.emplace_back (object);
        to_load.pop_front();

        break;
      }
    }

    try
    {
      if (async_loader_trace_enabled())
      {
        LogDebug << "Async finish begin type=" << object->async_object_type_name()
                 << " key='" << async_object_key(object)
                 << "' ptr=" << object << std::endl;
      }

      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loading file '" << (object->file_key().hasFilepath() ? object->file_key().filepath()
          : std::to_string(object->file_key().fileDataID()))<< "'" << std::endl;
      }

      object->finishLoading();

      if (async_loader_trace_enabled())
      {
        LogDebug << "Async finish done type=" << object->async_object_type_name()
                 << " key='" << async_object_key(object)
                 << "' ptr=" << object << std::endl;
      }

      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loaded  file '" << (object->file_key().hasFilepath() ? object->file_key().filepath()
        : std::to_string(object->file_key().fileDataID())) << "'" << std::endl;
      }

      {
        std::lock_guard<std::mutex> const lock (_guard);
        _currently_loading.remove (object);
        _state_changed.notify_all();
      }
    }
    catch (BlizzardArchive::Exceptions::FileReadFailedError const&)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      object->error_on_loading();

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      _state_changed.notify_all();
    }
    catch (std::exception const& e)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      LogError << "Async load exception type=" << object->async_object_type_name()
               << " key='" << async_object_key(object)
               << "' what='" << e.what() << "'" << std::endl;

      object->error_on_loading();

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      _state_changed.notify_all();
    }
    catch (...)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      LogError << "Async load unknown exception type=" << object->async_object_type_name()
               << " key='" << async_object_key(object) << "'" << std::endl;

      object->error_on_loading();

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      _state_changed.notify_all();
    }
  }
}

void AsyncLoader::queue_for_load (AsyncObject* object)
{
  std::lock_guard<std::mutex> const lock (_guard);
  _to_load[(size_t)object->loading_priority()].push_back (object);
  _state_changed.notify_one();
}

void AsyncLoader::ensure_deletable (AsyncObject* object)
{
  std::unique_lock<std::mutex> lock (_guard);
  _state_changed.wait
  ( lock
  , [&]
    {
      // Search ALL priority buckets by POINTER instead of indexing via object->loading_priority().
      // The caller (AsyncObjectMultimap::erase) drops its own lock between resolving this pointer and
      // calling here, so under heavy concurrent load the object can be freed in that window -- and
      // object->loading_priority() then dereferenced freed/garbage memory and crashed (null read at
      // offset 0x10, AsyncLoader.cpp:197). Pointer comparison is safe even on a stale pointer, and the
      // queues are small, so scanning all buckets is cheap.
      for (auto& to_load : _to_load)
      {
        auto const it = std::find (to_load.begin(), to_load.end(), object);
        // don't load it if it's just to delete it afterward
        if (it != to_load.end())
        {
          to_load.erase(it);
          return true;
        }
      }
      return std::find (_currently_loading.begin(), _currently_loading.end(), object) == _currently_loading.end();
    }
  );
}

AsyncLoader::AsyncLoader(int numThreads)
  : _stop (false)
{
  for (int i = 0; i < numThreads; ++i)
  {
    _threads.emplace_back (&AsyncLoader::process, this);
  }
}

AsyncLoader::~AsyncLoader()
{
  _stop = true;
  _state_changed.notify_all();

  for (auto& thread : _threads)
  {
    thread.join();
  }
}
