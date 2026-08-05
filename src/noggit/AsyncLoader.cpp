// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/errorHandling.h>
#include <Exception.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <cstdlib>
#include <list>

#if defined(_WIN32)
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <eh.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

void AsyncLoader::pause()
{
  std::unique_lock<std::mutex> lock(_guard);
  _paused = true;
  // Wait ONLY for the in-flight loads to finish (<= worker count), NOT the whole _to_load backlog. Workers
  // that finish park in the wake-condition above (paused) instead of grabbing more, so _currently_loading
  // drains and stays empty until resume(). The loader is then quiescent -> freeing tiles is race-free.
  _state_changed.wait(lock, [&] { return _currently_loading.empty(); });
}

void AsyncLoader::resume()
{
  {
    std::unique_lock<std::mutex> lock(_guard);
    _paused = false;
  }
  _state_changed.notify_all(); // wake the parked workers to resume streaming
}

void AsyncLoader::process()
{
#if defined(_WIN32)
  // Translate SEH exceptions (access violations etc.) into std::runtime_error on this
  // worker thread so the catch blocks below can log the exception code, faulting
  // module+offset and accessed address instead of an anonymous "unknown exception".
  // Requires /EHa (set globally in CMakeLists). The module+offset is resolvable to a
  // source line with llvm-symbolizer against the .pdb.
  _set_se_translator([](unsigned int code, EXCEPTION_POINTERS* ep)
  {
    char buf[192];
    void* code_addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    unsigned long long target = 0;
    int op = -1;
    if (ep && ep->ExceptionRecord && code == EXCEPTION_ACCESS_VIOLATION
        && ep->ExceptionRecord->NumberParameters >= 2)
    {
      op = static_cast<int>(ep->ExceptionRecord->ExceptionInformation[0]);
      target = static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    char mod_name[96] = "?";
    unsigned long long mod_ofs = 0;
    HMODULE mod = nullptr;
    if (code_addr
        && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCSTR>(code_addr), &mod)
        && mod)
    {
      char full[MAX_PATH];
      if (GetModuleFileNameA(mod, full, sizeof(full)))
      {
        char const* slash = std::strrchr(full, '\\');
        std::snprintf(mod_name, sizeof(mod_name), "%s", slash ? slash + 1 : full);
      }
      mod_ofs = reinterpret_cast<unsigned long long>(code_addr) - reinterpret_cast<unsigned long long>(mod);
    }
    std::snprintf(buf, sizeof(buf), "SEH exception 0x%08X at %s+0x%llX (%s of address 0x%llX)",
                  code, mod_name, mod_ofs, op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "exec" : "op?", target);
    throw std::runtime_error(buf);
  });
#endif

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
          // While paused, don't pick up new work (a tile unload is freeing objects race-free). A worker
          // already mid-load isn't affected -- it finishes, removes itself from _currently_loading, then
          // parks here until resume(). That bounds pause()'s wait to the in-flight loads only.
          return !!_stop || (!_paused && std::any_of ( _to_load.begin(), _to_load.end()
                                        , [](auto const& to_load) { return !to_load.empty(); }
                                        ));
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
        // LogError (not LogDebug) so it survives to log.txt -- for a HANG, the last "begin" with no
        // matching "done" names the object whose finishLoading() is stuck (e.g. a malformed WMO/M2 chunk
        // that allocs/loops forever). cerr is unit-buffered so it reaches disk even before a force-close.
        LogError << "ASYNCLOAD begin type=" << object->async_object_type_name()
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
        LogError << "ASYNCLOAD done  type=" << object->async_object_type_name()
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
