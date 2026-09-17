// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/ContextObject.hpp>
#include <noggit/Log.h>

#include <ClientData.hpp>
#include <Listfile.hpp>
#include <thread>

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <QOpenGLContext>

struct pair_hash
{
  std::size_t operator() (const std::pair<int, BlizzardArchive::Listfile::FileKey> &p) const noexcept
  {
    auto h1 = std::hash<int>{}(p.first);
    auto h2 = std::hash<std::string>{}(p.second.hasFilepath() ? p.second.filepath() : "");
    auto h3 = std::hash<int>{}(p.second.hasFileDataID() ? p.second.fileDataID() : 0);

    return h1 ^ h2 ^ h3;
  }
};

inline std::string async_object_filename(BlizzardArchive::Listfile::FileKey const& file_key)
{
  if (file_key.hasFilepath())
  {
    return file_key.filepath();
  }

  return file_key.hasFileDataID() ? std::to_string(file_key.fileDataID()) : std::string();
}

namespace Noggit
{

  template<typename T>
  struct AsyncObjectMultimap
  {
    AsyncObjectMultimap() = default;
    ~AsyncObjectMultimap()
    {
      /*
      apply ( [&] (std::string const& key, T const&)
              {
                auto pair = std::make_pair(context, key);
                LogDebug << key << ": " << _counts.at(pair) << std::endl;
              }
            );
      */
    }

    template<typename... Args>
      T* emplace (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context, Args&&... args)
    {
      std::scoped_lock const lock(_mutex);
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Emplacing " << normalized << " into context" << context << std::endl;

      {
        if ([&] { return _counts[pair]++; }())
        {
          return &_elements.at (pair);
        }
      }

      T* const obj ( [&]
                     {
                       // the object is built from the FULL key: a fileDataID out of MDDF / MODF / MODI /
                       // SFID (modern CASC clients) is authoritative and must not be re-derived from the
                       // path through the listfile
                       return &_elements.emplace ( std::piecewise_construct
                                                 , std::forward_as_tuple (pair)
                                                 , std::forward_as_tuple (file_key, context, args...)
                                                 ).first->second;
                     }()
                   );

      AsyncLoader::instance().queue_for_load(static_cast<AsyncObject*>(obj));

      return obj; 
    }
    void erase (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
    {
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Erasing " << normalized << " from context" << context << std::endl;

      AsyncObject* obj = nullptr;

      {
        std::scoped_lock lock(_mutex);

        // Tolerate an already-released / missing key instead of _counts.at() throwing
        // "invalid unordered_map<K,T> key" -> uncaught -> std::terminate. That fired on return-to-menu
        // while ~World tore down _creature_spawns: a ModelInstance's scoped texture reference could be
        // released after its count entry was already erased (a double-release), aborting the whole
        // teardown (and cascading into "deleteBuffers without active GL context" once the exception
        // unwound past ~MapView's context scope). If the entry is already gone there is nothing to do.
        auto const count_it = _counts.find(pair);
        if (count_it == _counts.end())
        {
          return;
        }

        if (--count_it->second == 0)
        {
          auto const elem_it = _elements.find(pair);
          if (elem_it != _elements.end())
          {
            obj = static_cast<AsyncObject*>(&(elem_it->second));
          }
        }
      }

      if (obj)
      {
        // The object may have been loaded manually while a queued async pointer
        // still exists, so it must always be removed from the loader before erase.
        AsyncLoader::instance().ensure_deletable(obj);

        {
          std::scoped_lock lock(_mutex);
          // [CRASH FIX 2026-08-05] RE-CHECK the count under the lock before deleting. Between dropping the
          // lock above (after --count hit 0) and here, a CONCURRENT emplace on the loader thread -- loading
          // another tile that references this same shared model/texture -- can revive the count (0 -> 1) and
          // hand a LIVE instance a pointer to this very object (unordered_map keeps element addresses stable
          // across rehash, so `obj` stays valid). Erasing it then would delete an object a live instance
          // still references -> UAF (the tile-unload crash that MapIndex::unloadTiles previously masked by
          // stalling the whole loader quiescent). Only erase if it's STILL unreferenced. If it was revived,
          // ensure_deletable() above merely cancelled a redundant reload of an already-loaded object -- harmless.
          auto const count_it = _counts.find(pair);
          if (count_it != _counts.end() && count_it->second == 0)
          {
            _elements.erase(pair);
            _counts.erase(pair);
          }
        }
      }
    }
    std::size_t size() const { std::scoped_lock lock(_mutex); return _elements.size(); } // [mem-diag]

    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }
    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }

    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun, Noggit::NoggitRenderContext context)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }
    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun, Noggit::NoggitRenderContext context) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }

  private:
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, T, pair_hash> _elements;
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, std::size_t, pair_hash> _counts;
    std::mutex mutable _mutex;
  };

}
