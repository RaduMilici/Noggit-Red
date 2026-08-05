// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/World.h>
#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/project/CurrentProject.hpp>
#ifdef USE_MYSQL_UID_STORAGE
  #include <mysql/mysql.h>
#endif
#include <noggit/map_index.hpp>
#include <noggit/MySqlSettings.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/frame_profiler.hpp>
#include <ClientFile.hpp>

#include <QtCore/QSettings>
#include <QByteArray>
#include <QTextStream>
#include <QRegExp>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <vector>
#include <forward_list>
#include <cstdlib>

MapIndex::MapIndex (const std::string &pBasename, int map_id, World* world,
                    Noggit::NoggitRenderContext context, bool create_empty)
  : basename(pBasename)
  , _map_id (map_id)
  , _last_unload_time((clock() / CLOCKS_PER_SEC)) // to not try to unload right away
  , mBigAlpha(false)
  , mHasAGlobalWMO(false)
  , noadt(false)
  , changed(false)
  , _sort_models_by_size_class(false)
  , highestGUID(0)
  , _world (world)
  , _context(context)
{

  QSettings settings;
  _unload_interval = settings.value("unload_interval", 3).toInt();
  _unload_dist = settings.value("unload_dist", 5).toInt();

  if (create_empty)
  {

    mHasAGlobalWMO = false;
    mBigAlpha = true;
    _sort_models_by_size_class = true;
    changed = false;

    for (int j = 0; j < 64; ++j)
    {
      for (int i = 0; i < 64; ++i)
      {
        mTiles[j][i].tile = nullptr;
        mTiles[j][i].onDisc = false;
        mTiles[j][i].flags = 0;
      }
    }

    return;
  }

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdt";

  BlizzardArchive::ClientFile theFile(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  uint32_t fourcc;
  uint32_t size;

  // - MVER ----------------------------------------------

  uint32_t version;

  theFile.read(&fourcc, 4);
  theFile.read(&size, 4);
  theFile.read(&version, 4);

  //! \todo find the correct version of WDT files.
  assert(fourcc == 'MVER' && version == 18);

  // - MHDR ----------------------------------------------

  theFile.read(&fourcc, 4);
  theFile.read(&size, 4);

  assert(fourcc == 'MPHD');

  theFile.read(&mphd, sizeof(MPHD));

  mHasAGlobalWMO = mphd.flags & FLAG_GLOBAL_OBJECT;
  // The 1.12 client never reads MPHD flags -- "big alpha" (uncompressed 8-bit alphamaps)
  // only exists from WotLK on, where the client checks flag 0x4. Some Turtle custom maps
  // (e.g. Karazahn40, flags=0xE) carry garbage MPHD flags while their MCAL data is regular
  // 2048-byte 4-bit; trusting the bit makes the alpha reader consume 4096 bytes per layer,
  // which misreads texture blends on every chunk and runs past the file buffer on the last
  // chunk of a tile (access violation -> whole tile fails to load).
  mBigAlpha = (mphd.flags & FLAG_BIG_ALPHA)
           && Noggit::Project::CurrentProject::get()->projectVersion != Noggit::Project::ProjectVersion::CLASSIC;
  if ((mphd.flags & FLAG_BIG_ALPHA) && !mBigAlpha)
  {
    LogDebug << "WDT \"" << basename << "\" sets MPHD big-alpha flag on a classic project; ignoring it (1.12 client behavior)." << std::endl;
  }
  _sort_models_by_size_class = mphd.flags & FLAG_DOODADS_SORT;

  if (!(mphd.flags & FLAG_SHADING))
  {
    mphd.flags |= FLAG_SHADING;
    changed = true;
  }

  // - MAIN ----------------------------------------------

  theFile.read(&fourcc, 4);
  theFile.seekRelative(4);

  assert(fourcc == 'MAIN');

  /// this is the theory. Sadly, we are also compiling on 64 bit machines with size_t being 8 byte, not 4. Therefore, we can't do the same thing, Blizzard does in its 32bit executable.
  //theFile.read( &(mTiles[0][0]), sizeof( 8 * 64 * 64 ) );

  for (int j = 0; j < 64; ++j)
  {
    for (int i = 0; i < 64; ++i)
    {
      theFile.read(&mTiles[j][i].flags, 4);
      theFile.seekRelative(4);

      std::stringstream adt_filename;
      adt_filename << "World\\Maps\\" << basename << "\\" << basename << "_" << i << "_" << j << ".adt";

      mTiles[j][i].tile = nullptr;
      mTiles[j][i].onDisc = Noggit::Application::NoggitApplication::instance()->clientData()->existsOnDisk(adt_filename.str());

			if (mTiles[j][i].onDisc && !(mTiles[j][i].flags & 1))
			{
				mTiles[j][i].flags |= 1;
				changed = true;
			}
		}
	}

  if (!theFile.isEof() && mHasAGlobalWMO)
  {
    //! \note We actually don't load WMO only worlds, so we just stop reading here, k?
    //! \bug MODF reads wrong. The assertion fails every time. Somehow, it keeps being MWMO. Or are there two blocks?
    //! \nofuckingbug  on eof read returns just without doing sth to the var and some wdts have a MWMO without having a MODF so only checking for eof above is not enough

    // mHasAGlobalWMO = false;

    // - MWMO ----------------------------------------------

    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MWMO');

    globalWMOName = std::string(theFile.getPointer(), size);
    theFile.seekRelative(size);

    // - MODF ----------------------------------------------

    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MODF');

    theFile.read(&wmoEntry, sizeof(ENTRY_MODF));
  }

  // -----------------------------------------------------

  theFile.close();

  loadMinimapMD5translate();
}

void MapIndex::saveall (World* world)
{
  world->wait_for_all_tile_updates();

  saveMaxUID();

  for (MapTile* tile : loaded_tiles())
  {
    world->horizon.update_horizon_tile(tile);
    tile->saveTile(world);
    tile->changed = false;
  }
}

void MapIndex::save()
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdt";

  //Log << "Saving WDT \"" << filename << "\"." << std::endl;

  sExtendableArray wdtFile = sExtendableArray();
  int curPos = 0;

  // MVER
  //  {
  wdtFile.Extend(8 + 0x4);
  SetChunkHeader(wdtFile, curPos, 'MVER', 4);

  // MVER data
  *(wdtFile.GetPointer<int>(8)) = 18;

  curPos += 8 + 0x4;
  //  }

  // MPHD
  //  {
  wdtFile.Extend(8);
  SetChunkHeader(wdtFile, curPos, 'MPHD', sizeof(MPHD));
  curPos += 8;

  mphd.flags = 0;
  mphd.something = 0;
  if (mHasAGlobalWMO)
      mphd.flags |= FLAG_GLOBAL_OBJECT;
  if (mBigAlpha)
      mphd.flags |= FLAG_BIG_ALPHA;
  if (_sort_models_by_size_class)
      mphd.flags |= FLAG_DOODADS_SORT;

  mphd.flags |= FLAG_SHADING;

  wdtFile.Insert(curPos, sizeof(MPHD), (char*)&mphd);
  curPos += sizeof(MPHD);

  // MAIN
  //  {
  wdtFile.Extend(8);
  SetChunkHeader(wdtFile, curPos, 'MAIN', 64 * 64 * 8);
  curPos += 8;

  for (int j = 0; j < 64; ++j)
  {
    for (int i = 0; i < 64; ++i)
    {
      wdtFile.Insert(curPos, 4, (char*)&mTiles[j][i].flags);
      wdtFile.Extend(4);
      curPos += 8;
    }
  }
  //  }

  if (mHasAGlobalWMO)
  {
    // MWMO
    //  {
    wdtFile.Extend(8);
    SetChunkHeader(wdtFile, curPos, 'MWMO', static_cast<int>(globalWMOName.size()));
    curPos += 8;

    wdtFile.Insert(curPos, static_cast<unsigned long>(globalWMOName.size()), globalWMOName.data());
    curPos += static_cast<int>(globalWMOName.size());
    //  }

    // MODF
    //  {
    wdtFile.Extend(8);
    SetChunkHeader(wdtFile, curPos, 'MODF', sizeof(ENTRY_MODF));
    curPos += 8;

    wdtFile.Insert(curPos, sizeof(ENTRY_MODF), (char*)&wmoEntry);
    curPos += sizeof(ENTRY_MODF);
    //  }
  }

  BlizzardArchive::ClientFile f(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData(),
                                BlizzardArchive::ClientFile::NEW_FILE);
  f.setBuffer(wdtFile.data);
  f.save();
  f.close();

  changed = false;
}

void MapIndex::enterTile(const TileIndex& tile)
{
  noadt = !hasTile(tile);
  int cx = static_cast<int>(tile.x);
  int cz = static_cast<int>(tile.z);

  // Prefetch the (2r+1)x(2r+1) grid of tiles around the camera. User-configurable (Settings -> "ADT
  // loading radius"), default 1 = 3x3 to match the reference build. A larger radius streams content in
  // earlier (smoother as you move) but keeps more tiles resident -> more terrain/objects/WMOs to draw
  // every frame -> lower fps. Read live so the slider takes effect on the next tile crossing.
  int const radius = std::max(0, QSettings().value("loading_radius", 1).toInt());

  // [perf 2026-08-05] BUDGET main-thread MapTile creation. Each new tile costs a MapTile ctor + an MPQ
  // exists() on the MAIN thread; with loading_radius=4 (81-tile grid) a fast fly crosses a ~9-tile edge per
  // boundary, so creating them all in one frame stalled `tick` ~110ms (measured [TICK-STALL]). Cap NEW
  // creations per call and take the NEAREST first (content appears where the camera is heading); the rest
  // are created over the next frames -- the async loader still streams their heavy data in the background,
  // and already-resident tiles are untouched. NOGGIT_TILE_LOAD_BUDGET overrides (0 = unlimited = old).
  static int const s_load_budget = []
  {
    char const* v = std::getenv("NOGGIT_TILE_LOAD_BUDGET");
    return v ? std::atoi(v) : 3;
  }();

  // Collect only the tiles that still need CREATING (skip resident / awaiting / failed / absent -- those
  // are cheap no-ops in loadTile anyway), nearest-first.
  std::vector<std::pair<float, TileIndex>> pending;
  for (int pz = std::max(cz - radius, 0); pz <= std::min(cz + radius, 63); ++pz)
  {
    for (int px = std::max(cx - radius, 0); px <= std::min(cx + radius, 63); ++px)
    {
      TileIndex const t(static_cast<std::size_t>(px), static_cast<std::size_t>(pz));
      if (!hasTile(t) || tileLoaded(t) || tileAwaitingLoading(t) || tileLoadFailed(t))
      {
        continue;
      }
      pending.emplace_back(tile.dist(t), t);
    }
  }

  if (s_load_budget > 0 && static_cast<int>(pending.size()) > s_load_budget)
  {
    std::partial_sort(pending.begin(), pending.begin() + s_load_budget, pending.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });
  }
  else
  {
    std::sort(pending.begin(), pending.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
  }

  int created = 0;
  for (auto const& p : pending)
  {
    if (s_load_budget > 0 && created >= s_load_budget)
    {
      break;
    }
    loadTile(p.second);
    ++created;
  }
}

void MapIndex::update_model_tile(const TileIndex& tile, model_update type, SceneObject* instance, bool mark_changed)
{
  MapTile* adt = loadTile(tile);

  if (!adt)
    return;

  adt->wait_until_loaded();
  // [perf 2026-08-05] Only flag the tile dirty for REAL edits. The load-time path (UID-collision reassignment
  // / tile reload) passes mark_changed=false so streamed-in tiles are NOT pinned resident (changed tiles never
  // unload -> unbounded memory growth on a long fly = the original "eats RAM & crashes"). See queue_update.
  if (mark_changed)
  {
    adt->changed = true;
  }

  if (type == model_update::add)
  {
    adt->add_model(instance);
  }
  else if(type == model_update::remove)
  {
    adt->remove_model(instance);
  }
}

void MapIndex::setChanged(const TileIndex& tile)
{
  MapTile* mTile = loadTile(tile);

  if (!!mTile)
  {
    mTile->changed = true;
  }
}

void MapIndex::setChanged(MapTile* tile)
{
  setChanged(tile->index);
}

void MapIndex::unsetChanged(const TileIndex& tile)
{
  // change the changed flag of the map tile
  if (hasTile(tile))
  {
    mTiles[tile.z][tile.x].tile->changed = false;
  }
}

bool MapIndex::has_unsaved_changes(const TileIndex& tile) const
{
  return (tileLoaded(tile) ? getTile(tile)->changed.load() : false);
}

void MapIndex::setFlag(bool to, glm::vec3 const& pos, uint32_t flag)
{
  TileIndex tile(pos);

  if (tileLoaded(tile))
  {
    setChanged(tile);

    int cx = (pos.x - tile.x * TILESIZE) / CHUNKSIZE;
    int cz = (pos.z - tile.z * TILESIZE) / CHUNKSIZE;

    MapChunk* chunk = getTile(tile)->getChunk(cx, cz);
    NOGGIT_CUR_ACTION->registerChunkFlagChange(chunk);
    chunk->setFlag(to, flag);
  }
}

MapTile* MapIndex::loadTile(const TileIndex& tile, bool reloading, bool load_models, bool load_textures)
{
  if (!hasTile(tile))
  {
    return nullptr;
  }

  if (tileLoadFailed(tile))
  {
    return nullptr;
  }

  if (tileLoaded(tile) || tileAwaitingLoading(tile))
  {
    return mTiles[tile.z][tile.x].tile.get();
  }

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";

  // [perf 2026-08-05] The MPQ clientData()->exists() check that used to be here was measured at 4-9ms per
  // tile ([LOADTILE] split) -- the ENTIRE enterTile main-thread stall (the ctor is ~0.03ms). It is
  // REDUNDANT: loadTile already returned above unless hasTile(tile) is true, and hasTile is the WDT
  // tile-present flag (mTiles[..].flags & 1) -- the map's own authoritative record of which ADTs exist. A
  // WDT-present-but-file-missing tile (corrupt custom data) is handled downstream: MapTile::finishLoading on
  // the loader thread throws FileReadFailedError -> AsyncLoader marks it loading_failed (tileLoadFailed),
  // exactly as a failed exists() would have rejected it, just off the main thread. So skip the scan.

  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
     mBigAlpha, load_models, use_mclq_green_lava(), reloading, _world, _context, tile_mode::edit, load_textures);

  MapTile* adt = mTiles[tile.z][tile.x].tile.get();

  AsyncLoader::instance().queue_for_load(adt);
  _n_loaded_tiles++;

  // [TILE] streaming trace (NOGGIT_FRAME_PROFILE): pair with the [FRAME-SPIKE]/TileStream numbers to see
  // whether a hitch lines up with a tile being queued (main-thread MapTile ctor above) or, a few frames
  // later, its first-draw GPU upload. loaded=count lets you watch the working set grow/shrink.
  if (noggit::perf::FrameProfiler::get().on)
  {
    LogError << "[TILE] queue " << tile.x << "_" << tile.z << "  loaded=" << _n_loaded_tiles << std::endl;
  }

  return adt;
}

void MapIndex::reloadTile(const TileIndex& tile)
{
  if (tileLoaded(tile))
  {
    mTiles[tile.z][tile.x].tile.reset();
    loadTile(tile, true);
  }
}

void MapIndex::unloadTiles(const TileIndex& tile)
{
  if (((clock() / CLOCKS_PER_SEC) - _last_unload_time) > _unload_interval)
  {
    // [CRASH FIX 2026-08-04] Collect the tiles to unload FIRST, then unload them AFTER the loop.
    // unloadTile() sets mTiles[..].tile = nullptr, which mutates the very container loaded_tiles()
    // iterates -> iterator invalidation / use-after-free (0xC0000005 in unloadTile). Latent for years
    // because with unload_dist large nothing ever crossed the threshold; it fires the moment tiles
    // actually unload (sane unload_dist + fast movement) -> "it crashes when it goes too fast".
    unsigned resident = 0, far_count = 0, far_blocked_changed = 0;
    std::vector<TileIndex> to_unload;
    for (MapTile* adt : loaded_tiles())
    {
      ++resident;
      if (tile.dist(adt->index) > _unload_dist)
      {
        ++far_count;
        //Only unload adts not marked to save
        if (!adt->changed.load())
        {
          to_unload.push_back(adt->index);
        }
        else
        {
          ++far_blocked_changed;
        }
      }
    }
    // [perf 2026-08-05] Free at most N tiles per pass. Freeing needs the loader quiescent for crash-safety (a
    // loader finishing a model/WMO writes tile-owned state -> feeds SceneObject instances into MapTile::
    // object_instances -> freeing that tile mid-load = UAF in AsyncLoader::process -> finishLoading, confirmed
    // in the VEH callstack; the AsyncObjectMultimap::erase re-check closes the refcount-revive race but NOT
    // this one). ScopedPause gives that quiescence BOUNDED (waits only for the in-flight loads). But the FREE
    // ITSELF is O(tiles): each tile tears down hundreds of instances, so freeing a big backlog at once = a ~1s
    // stall (a prior "defer then batch-free 46-52 tiles" attempt hit 1152ms). So cap the free per pass and let
    // a backlog drain over the next passes. NOGGIT_MAX_UNLOAD_PER_PASS overrides (0 = unlimited).
    static unsigned const s_max_unload_per_pass = []
    {
      char const* v = std::getenv("NOGGIT_MAX_UNLOAD_PER_PASS");
      return v ? static_cast<unsigned>(std::max(0, std::atoi(v))) : 6u;
    }();
    // DEFER while the loader streams AND the reclaimable backlog (non-changed far tiles) is still modest: the
    // ScopedPause returns instantly only when the loader is idle, so freeing mid-stream pays the in-flight
    // wait. Skip then -- free on the loader's next lull, or once the backlog grows past the threshold so
    // memory can't run away (then still capped per pass so we drain gradually, never batch-free a big pile).
    static unsigned const s_defer_backlog = []
    {
      char const* v = std::getenv("NOGGIT_UNLOAD_DEFER_BACKLOG");
      return v ? static_cast<unsigned>(std::max(0, std::atoi(v))) : 40u;
    }();
    bool const defer = !to_unload.empty() && s_defer_backlog > 0
                    && to_unload.size() <= s_defer_backlog
                    && AsyncLoader::instance().is_loading();
    unsigned unloaded = 0;
    if (!to_unload.empty() && !defer)
    {
      // Collect-first still required (unloadTile nulls mTiles[..] mid-iteration of loaded_tiles() -> iterator
      // invalidation). ScopedPause = the bounded loader quiescence for a race-free free (see above).
      AsyncLoader::ScopedPause const loader_pause;
      for (TileIndex const& idx : to_unload)
      {
        if (s_max_unload_per_pass > 0 && unloaded >= s_max_unload_per_pass)
        {
          break;
        }
        unloadTile(idx);
        ++unloaded;
      }
    }
    if (noggit::perf::FrameProfiler::get().on)
    {
      LogError << "[UNLOAD] resident=" << resident << " far=" << far_count
               << " far_blocked_changed=" << far_blocked_changed
               << " unloaded=" << unloaded << "/" << to_unload.size()
               << (defer ? " DEFERRED(loader busy)" : "")
               << " maxPerPass=" << s_max_unload_per_pass << " deferBacklog=" << s_defer_backlog
               << " unload_dist=" << _unload_dist
               << " loading_radius=" << QSettings().value("loading_radius", 1).toInt() << std::endl;
    }

    _last_unload_time = clock() / CLOCKS_PER_SEC;
  }
}

void MapIndex::unloadTile(const TileIndex& tile)
{
  // unloads a tile with givn cords
  if (tileLoaded(tile))
  {
    Log << "Unload Tile " << tile.x << "-" << tile.z << std::endl;
    mTiles[tile.z][tile.x].tile = nullptr;
    _n_loaded_tiles--;
    if (noggit::perf::FrameProfiler::get().on)
    {
      LogError << "[TILE] unload " << tile.x << "_" << tile.z << "  loaded=" << _n_loaded_tiles << std::endl;
    }
  }
}

void MapIndex::markOnDisc(const TileIndex& tile, bool mto)
{
  if(tile.is_valid())
  {
    mTiles[tile.z][tile.x].onDisc = mto;
  }
}

bool MapIndex::isTileExternal(const TileIndex& tile) const
{
  // is onDisc
  return tile.is_valid() && mTiles[tile.z][tile.x].onDisc;
}

void MapIndex::saveTile(const TileIndex& tile, World* world, bool save_unloaded)
{
  world->wait_for_all_tile_updates();

	// save given tile
	if (save_unloaded)
  {
    auto filepath = std::filesystem::path (Noggit::Project::CurrentProject::get()->ProjectPath)
                    / BlizzardArchive::ClientData::normalizeFilenameInternal (mTiles[tile.z][tile.x].tile->file_key().filepath());

    QFile file(filepath.string().c_str());
    file.open(QIODevice::WriteOnly);

    mTiles[tile.z][tile.x].tile->initEmptyChunks();
    mTiles[tile.z][tile.x].tile->saveTile(world);
    return;
  }

	if (tileLoaded(tile))
	{
    saveMaxUID();
    world->horizon.update_horizon_tile(mTiles[tile.z][tile.x].tile.get());
		mTiles[tile.z][tile.x].tile->saveTile(world);
	}
}

void MapIndex::saveChanged (World* world, bool save_unloaded)
{
  world->wait_for_all_tile_updates();

  if (changed)
  {
    save();
  }

  if (!save_unloaded)
  {
    saveMaxUID();
  }
  else
  {
    for (int i = 0; i < 64; ++i)
    {
      for (int j = 0; j < 64; ++j)
      {
        if (!(mTiles[i][j].tile && mTiles[i][j].tile->changed.load()))
        {
          continue;
        }

        auto filepath = std::filesystem::path (Noggit::Project::CurrentProject::get()->ProjectPath)
                        / BlizzardArchive::ClientData::normalizeFilenameInternal (mTiles[i][j].tile->file_key().filepath());

        if (mTiles[i][j].flags & 0x1)
        {
          QFile file(filepath.string().c_str());
          file.open(QIODevice::WriteOnly);

          mTiles[i][j].tile->initEmptyChunks();
          mTiles[i][j].tile->saveTile(world);
          mTiles[i][j].tile->changed = false;
        }
        else
        {
          QFile file(filepath.string().c_str());
          file.remove();
        }
      }
    }
    return;
  }

  for (MapTile* tile : loaded_tiles())
  {
    if (tile->changed.load())
    {
      world->horizon.update_horizon_tile(tile);
      tile->saveTile(world);
      tile->changed = false;
    }
  }
}

bool MapIndex::hasAGlobalWMO()
{
  return mHasAGlobalWMO;
}


bool MapIndex::hasTile(const TileIndex& tile) const
{
  return tile.is_valid() && (mTiles[tile.z][tile.x].flags & 1);
}

bool MapIndex::tileLoadFailed(const TileIndex& tile) const
{
  return hasTile(tile) && mTiles[tile.z][tile.x].tile && mTiles[tile.z][tile.x].tile->loading_failed();
}

bool MapIndex::tileAwaitingLoading(const TileIndex& tile) const
{
  return hasTile(tile) && mTiles[tile.z][tile.x].tile && !mTiles[tile.z][tile.x].tile->finishedLoading();
}

bool MapIndex::tileLoaded(const TileIndex& tile) const
{
  return hasTile(tile) && mTiles[tile.z][tile.x].tile && mTiles[tile.z][tile.x].tile->finishedLoading() && !mTiles[tile.z][tile.x].tile->loading_failed();
}

bool MapIndex::hasAdt()
{
  return noadt;
}

void MapIndex::setAdt(bool value)
{
  noadt = value;
}

MapTile* MapIndex::getTile(const TileIndex& tile) const
{
  return (tile.is_valid() ? mTiles[tile.z][tile.x].tile.get() : nullptr);
}

MapTile* MapIndex::getTileAbove(MapTile* tile) const
{
  TileIndex above(tile->index.x, tile->index.z - 1);
  if (tile->index.z == 0 || (!tileLoaded(above) && !tileAwaitingLoading(above)))
  {
    return nullptr;
  }

  MapTile* tile_above = mTiles[tile->index.z - 1][tile->index.x].tile.get();
  tile_above->wait_until_loaded();

  if (tile_above->loading_failed())
  {
    return nullptr;
  }

  return tile_above;
}

MapTile* MapIndex::getTileLeft(MapTile* tile) const
{
  TileIndex left(tile->index.x - 1, tile->index.z);
  if (tile->index.x == 0 || (!tileLoaded(left) && !tileAwaitingLoading(left)))
  {
    return nullptr;
  }

  MapTile* tile_left = mTiles[tile->index.z][tile->index.x - 1].tile.get();
  tile_left->wait_until_loaded();

  if (tile_left->loading_failed())
  {
    return nullptr;
  }

  return tile_left;
}

uint32_t MapIndex::getFlag(const TileIndex& tile) const
{
  return (tile.is_valid() ? mTiles[tile.z][tile.x].flags : 0);
}

void MapIndex::convert_alphamap(bool to_big_alpha)
{
  mBigAlpha = to_big_alpha;
  if (to_big_alpha)
  {
    mphd.flags |= 4;
  }
  else
  {
    mphd.flags &= 0xFFFFFFFB;
  }
}


uint32_t MapIndex::getHighestGUIDFromFile(const std::string& pFilename) const
{
	uint32_t highGUID = 0;

    BlizzardArchive::ClientFile theFile(pFilename, Noggit::Application::NoggitApplication::instance()->clientData());
    if (theFile.isEof())
    {
      return highGUID;
    }

    uint32_t fourcc;
    uint32_t size;

    MHDR Header;

    // - MVER ----------------------------------------------

    uint32_t version;

    theFile.read(&fourcc, 4);
    theFile.seekRelative(4);
    theFile.read(&version, 4);

    assert(fourcc == 'MVER' && version == 18);

    // - MHDR ----------------------------------------------

    theFile.read(&fourcc, 4);
    theFile.seekRelative(4);

    assert(fourcc == 'MHDR');

    theFile.read(&Header, sizeof(MHDR));

    // - MDDF ----------------------------------------------

    theFile.seek(Header.mddf + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MDDF');

    ENTRY_MDDF const* mddf_ptr = reinterpret_cast<ENTRY_MDDF const*>(theFile.getPointer());
    for (unsigned int i = 0; i < size / sizeof(ENTRY_MDDF); ++i)
    {
        highGUID = std::max(highGUID, mddf_ptr[i].uniqueID);
    }

    // - MODF ----------------------------------------------

    theFile.seek(Header.modf + 0x14);
    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MODF');

    ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(theFile.getPointer());
    for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
    {
        highGUID = std::max(highGUID, modf_ptr[i].uniqueID);
    }
    theFile.close();

    return highGUID;
}

uint32_t MapIndex::newGUID()
{
  std::unique_lock<std::mutex> lock (_mutex);

#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (Noggit::mysqlSetting("enabled", false).toBool())
  {
    mysql::updateUIDinDB(_map_id, highestGUID + 1); // update the highest uid in db, note that if the user don't save these uid won't be used (not really a problem tho) 
  }
#endif
  return ++highestGUID;
}

uid_fix_status MapIndex::fixUIDs (World* world, bool cancel_on_model_loading_error)
{
  // clear all selection groups since UIDs will change.
  // TODO : update them instead.
    _world->clear_selection_groups();

  // pre-cond: mTiles[z][x].flags are set

  // unload any previously loaded tile, although there shouldn't be as
  // the fix is executed before loading the map
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (mTiles[z][x].tile)
      {
        MapTile* tile = mTiles[z][x].tile.get();

        // don't unload half loaded tiles
        tile->wait_until_loaded();

        unloadTile(tile->index);
      }
    }
  }

  _uid_fix_all_in_progress = true;

  std::forward_list<ModelInstance> models;
  std::forward_list<WMOInstance> wmos;

  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";
      BlizzardArchive::ClientFile file(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

      if (file.isEof())
      {
        continue;
      }

      std::array<glm::vec3, 2> tileExtents;
      tileExtents[0] = { x*TILESIZE, 0, z*TILESIZE };
      tileExtents[1] = { (x+1)*TILESIZE, 0, (z+1)*TILESIZE };
      misc::minmax(&tileExtents[0], &tileExtents[1]);

      std::forward_list<ENTRY_MDDF> modelEntries;
      std::forward_list<ENTRY_MODF> wmoEntries;
      std::vector<std::string> modelFilenames;
      std::vector<std::string> wmoFilenames;

      uint32_t fourcc;
      uint32_t size;

      MHDR Header;

      // - MVER ----------------------------------------------
      uint32_t version;
      file.read(&fourcc, 4);
      file.seekRelative(4);
      file.read(&version, 4);
      assert(fourcc == 'MVER' && version == 18);

      // - MHDR ----------------------------------------------
      file.read(&fourcc, 4);
      file.seekRelative(4);
      assert(fourcc == 'MHDR');
      file.read(&Header, sizeof(MHDR));

      // - MDDF ----------------------------------------------
      file.seek(Header.mddf + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MDDF');

      ENTRY_MDDF const* mddf_ptr = reinterpret_cast<ENTRY_MDDF const*>(file.getPointer());

      for (unsigned int i = 0; i < size / sizeof(ENTRY_MDDF); ++i)
      {
        bool add = true;
        ENTRY_MDDF const& mddf = mddf_ptr[i];

        if (!misc::pointInside({ mddf.pos[0], 0, mddf.pos[2] }, tileExtents))
        {
          continue;
        }

        // check for duplicates
        for (ENTRY_MDDF& entry : modelEntries)
        {
          if ( mddf.nameID == entry.nameID
            && misc::float_equals(mddf.pos[0], entry.pos[0])
            && misc::float_equals(mddf.pos[1], entry.pos[1])
            && misc::float_equals(mddf.pos[2], entry.pos[2])
            && misc::float_equals(mddf.rot[0], entry.rot[0])
            && misc::float_equals(mddf.rot[1], entry.rot[1])
            && misc::float_equals(mddf.rot[2], entry.rot[2])
            && mddf.scale == entry.scale
            )
          {
            add = false;
            break;
          }
        }

        if (add)
        {
          modelEntries.emplace_front(mddf);
        }
      }

      // - MODF ----------------------------------------------
      file.seek(Header.modf + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MODF');

      ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(file.getPointer());

      for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
      {
        bool add = true;
        ENTRY_MODF const& modf = modf_ptr[i];

        if (!misc::pointInside({ modf.pos[0], 0, modf.pos[2] }, tileExtents))
        {
          continue;
        }

        // check for duplicates
        for (ENTRY_MODF& entry : wmoEntries)
        {
          if (modf.nameID == entry.nameID
            && misc::float_equals(modf.pos[0], entry.pos[0])
            && misc::float_equals(modf.pos[1], entry.pos[1])
            && misc::float_equals(modf.pos[2], entry.pos[2])
            && misc::float_equals(modf.rot[0], entry.rot[0])
            && misc::float_equals(modf.rot[1], entry.rot[1])
            && misc::float_equals(modf.rot[2], entry.rot[2])
            )
          {
            add = false;
            break;
          }
        }

        if (add)
        {
          wmoEntries.emplace_front(modf);
        }
      }

      // - MMDX ----------------------------------------------
      file.seek(Header.mmdx + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MMDX');

      {
        char const* lCurPos = reinterpret_cast<char const*>(file.getPointer());
        char const* lEnd = lCurPos + size;

        while (lCurPos < lEnd)
        {
          modelFilenames.push_back(std::string(lCurPos));
          lCurPos += strlen(lCurPos) + 1;
        }
      }

      // - MWMO ----------------------------------------------
      file.seek(Header.mwmo + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MWMO');

      {
        char const* lCurPos = reinterpret_cast<char const*>(file.getPointer());
        char const* lEnd = lCurPos + size;

        while (lCurPos < lEnd)
        {
          wmoFilenames.push_back(std::string(lCurPos));
          lCurPos += strlen(lCurPos) + 1;
        }
      }

      file.close();

      for (ENTRY_MDDF& entry : modelEntries)
      {
        models.emplace_front(modelFilenames[entry.nameID], &entry, _context);
      }
      for (ENTRY_MODF& entry : wmoEntries)
      {
        wmos.emplace_front(wmoFilenames[entry.nameID], &entry, _context);
      }
    }
  }

  // set all uids
  // for each tile save the m2/wmo present inside
  highestGUID = 0;

  std::map<std::size_t, std::map<std::size_t, std::forward_list<std::uint32_t>>> uids_per_tile;

  bool loading_error = false;

  for (ModelInstance& instance : models)
  {
    instance.uid = highestGUID++;
    instance.model->wait_until_loaded();
    instance.recalcExtents();

    loading_error |= instance.model->loading_failed();

    // to avoid going outside of bound
    std::size_t sx = std::max((std::size_t)(instance.extents[0].x / TILESIZE), (std::size_t)0);
    std::size_t sz = std::max((std::size_t)(instance.extents[0].z / TILESIZE), (std::size_t)0);
    std::size_t ex = std::min((std::size_t)(instance.extents[1].x / TILESIZE), (std::size_t)63);
    std::size_t ez = std::min((std::size_t)(instance.extents[1].z / TILESIZE), (std::size_t)63);

    auto const real_uid (world->add_model_instance (std::move(instance), false));

    for (std::size_t z = sz; z <= ez; ++z)
    {
      for (std::size_t x = sx; x <= ex; ++x)
      {
        uids_per_tile[z][x].push_front (real_uid);
      }
    }
  }

  models.clear();

  for (WMOInstance& instance : wmos)
  {
    instance.uid = highestGUID++;
    instance.wmo->wait_until_loaded();
    instance.recalcExtents();
    // no need to check if the loading is finished since the extents are stored inside the adt
    // to avoid going outside of bound
    std::size_t sx = std::max((std::size_t)(instance.extents[0].x / TILESIZE), (std::size_t)0);
    std::size_t sz = std::max((std::size_t)(instance.extents[0].z / TILESIZE), (std::size_t)0);
    std::size_t ex = std::min((std::size_t)(instance.extents[1].x / TILESIZE), (std::size_t)63);
    std::size_t ez = std::min((std::size_t)(instance.extents[1].z / TILESIZE), (std::size_t)63);

    auto const real_uid (world->add_wmo_instance (std::move(instance), false));

    for (std::size_t z = sz; z <= ez; ++z)
    {
      for (std::size_t x = sx; x <= ex; ++x)
      {
        uids_per_tile[z][x].push_front (real_uid);
      }
    }
  }

  wmos.clear();

  if (cancel_on_model_loading_error && loading_error)
  {
    return uid_fix_status::failed;
  }

  // load each tile without the models and
  // save them with the models with the new uids
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      // load even the tiles without models in case there are old ones
      // that shouldn't be there to avoid creating new duplicates

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      // load the tile without the models
      MapTile tile(x, z, filename.str(), mBigAlpha, false, use_mclq_green_lava(), false, world, _context, tile_mode::uid_fix_all);
      tile.finishLoading();

      // add the uids to the tile to be able to save the models
      // which have been loaded in world earlier
      for (std::uint32_t uid : uids_per_tile[z][x])
      {
        tile.add_model(uid);
      }

      tile.saveTile(world);
    }
  }

  // override the db highest uid if used
  saveMaxUID();

  _uid_fix_all_in_progress = false;

  // force instances unloading
  world->unload_every_model_and_wmo_instance();

  return loading_error ? uid_fix_status::done_with_errors : uid_fix_status::done;
}

void MapIndex::searchMaxUID()
{
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";
      highestGUID = std::max(highestGUID, getHighestGUIDFromFile(filename.str()));
    }
  }

  saveMaxUID();
}

void MapIndex::saveMaxUID()
{
#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (Noggit::mysqlSetting("enabled", false).toBool())
  {
    if (mysql::hasMaxUIDStoredDB(_map_id))
    {
	    mysql::updateUIDinDB(_map_id, highestGUID);
    }
    else
    {
	    mysql::insertUIDinDB(_map_id, highestGUID);
    }
  }
#endif
  // save the max UID on the disk (always save to sync with the db if used
  uid_storage::saveMaxUID (_map_id, highestGUID);
}

void MapIndex::loadMaxUID()
{
  highestGUID = uid_storage::getMaxUID (_map_id);
#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (Noggit::mysqlSetting("enabled", false).toBool())
  {
    highestGUID = std::max(mysql::getGUIDFromDB(_map_id), highestGUID);
    // save to make sure the db and disk uid are synced
    saveMaxUID();
  }
#endif
}

void MapIndex::loadMinimapMD5translate()
{
  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists("textures/minimap/md5translate.trs"))
  {
    LogError << "md5translate.trs was not found. "
                "Noggit will generate a new one in the project directory on minimap save." << std::endl;
    return;
  }

  BlizzardArchive::ClientFile md5trs_file("textures/minimap/md5translate.trs", Noggit::Application::NoggitApplication::instance()->clientData());

  size_t size = md5trs_file.getSize();
  void* buffer_raw = std::malloc(size);
  md5trs_file.read(buffer_raw, size);

  QByteArray md5trs_bytes(static_cast<char*>(buffer_raw), static_cast<int>(size));

  QTextStream md5trs_stream(md5trs_bytes, QIODevice::ReadOnly);

  QString cur_dir = "";
  while (!md5trs_stream.atEnd())
  {
    QString line = md5trs_stream.readLine();

    if (!line.length())
    {
      continue;
    }

    if (line.startsWith("dir: ", Qt::CaseInsensitive))
    {
      QStringList dir_line_split = line.split(" ");
      cur_dir = dir_line_split[1];
      continue;
    }

    QStringList line_split = line.split(QRegExp("[\t]"));

    if (line_split.length() < 2)
    {
        std::string text = "Failed to read md5translate.trs.\nLine \"" + line.toStdString() + "\n has no tab spacing. Spacing must be only a tab character and not spaces.";
        LogError << text << std::endl;
        throw  std::logic_error(text);
    }

    if (cur_dir.length())
    {
      _minimap_md5translate[cur_dir.toStdString()][line_split[0].toStdString()] = line_split[1].toStdString();
    }

  }

}

void MapIndex::saveMinimapMD5translate()
{
  QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(str.endsWith('\\') || str.endsWith('/')))
  {
    str += "/";
  }

  QString filepath = str + "/textures/minimap/md5translate.trs";

  QFile file = QFile(filepath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    QTextStream out(&file);

    for (auto it = _minimap_md5translate.begin(); it != _minimap_md5translate.end(); ++it)
    {
      out << "dir: " << it->first.c_str() << "\n"; // save dir

      for (auto it_ = it->second.begin(); it_ != it->second.end(); ++it_)
      {
        out << it_->first.c_str() << "\t" << it_->second.c_str() << "\n";
      }
    }

    file.close();
  }
  else
  {
    LogError << "Failed saving md5translate.trs. File can't be opened." << std::endl;
  }




}

void MapIndex::addTile(const TileIndex& tile)
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";

  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
      mBigAlpha, true, use_mclq_green_lava(), false, _world, _context);

  mTiles[tile.z][tile.x].flags |= 0x1;
  mTiles[tile.z][tile.x].tile->changed = true;

  _world->horizon.update_horizon_tile(mTiles[tile.z][tile.x].tile.get());

  changed = true;
}

void MapIndex::removeTile(const TileIndex &tile)
{
  mTiles[tile.z][tile.x].flags &= ~0x1;

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";
  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
     mBigAlpha, true, use_mclq_green_lava(), false, _world, _context);

  mTiles[tile.z][tile.x].tile->changed = true;
  mTiles[tile.z][tile.x].onDisc = false;

  _world->horizon.remove_horizon_tile(tile.z, tile.x);

  changed = true;
}

unsigned MapIndex::getNumExistingTiles()
{
  if (_n_existing_tiles >= 0)
    return _n_existing_tiles;

  _n_existing_tiles = 0;
  for (int i = 0; i < 4096; ++i)
  {
    TileIndex index(i / 64, i % 64);

    if (hasTile(index))
    {
      _n_existing_tiles++;
    }
  }

  return _n_existing_tiles;
}

void MapIndex::set_basename(const std::string &pBasename)
{
  basename = pBasename;

  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!mTiles[z][x].tile)
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      mTiles[z][x].tile->setFilename(filename.str());
    }
  }
}

void MapIndex::create_empty_wdl()
{
    // for new map creation, creates a new WDL with all heights as 0
    std::stringstream filename;
    filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl"; // mapIndex.basename ? 
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    sExtendableArray wdlFile = sExtendableArray();
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // MWMO
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWMO', 0);

    curPos += 8;
    //  }

    // MWID
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWID', 0);

    curPos += 8;
    //  }

    // MODF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MODF', 0);

    curPos += 8;
    //  }

    uint32_t mare_offsets[4096] = { 0 }; // [64][64];
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;

    uint32_t mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            bool has_tile = hasTile(index);

            // if (tile_exists)
            if (has_tile) // TODO check if tile exists
            {
                // write offset in MAOF entry
                wdlFile.Insert(curPos, 4, (char*)&mareoffset);
                mare_offsets[y * 64 + x] = mareoffset;
                mareoffset += 1138; // mare + maho
            }
            else
                wdlFile.Extend(4);
            curPos += 4;

        }
    }

    for (auto offset : mare_offsets)
    {
        if (!offset)
            continue;

        // MARE
        //  {
        wdlFile.Extend(8);
        SetChunkHeader(wdlFile, curPos, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
        curPos += 8;

        // write inner and outer heights
        wdlFile.Extend((2 * (17 * 17)) + (2 * (16 * 16)));
        curPos += (2 * (17 * 17)) + (2 * (16 * 16));
        //  }

        // MAHO (maparea holes)
        //  {
        wdlFile.Extend(8);
        SetChunkHeader(wdlFile, curPos, 'MAHO', 2 * 16); // 1 hole mask for each chunk
        curPos += 8;

        wdlFile.Extend(32);
        curPos += 32;
    }
    BlizzardArchive::ClientFile f(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData(),
    BlizzardArchive::ClientFile::NEW_FILE);
    f.setBuffer(wdlFile.data);
    f.save();
    f.close();
}
