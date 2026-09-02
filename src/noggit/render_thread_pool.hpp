// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

// [threading 2026-08-18] Persistent worker pool for per-frame data-parallel RENDER-PREP work (creature
// skeleton animate, doodad gather, particle simulation). It replaces the old pattern of spawning a fresh
// std::vector<std::thread> and join()ing it EVERY frame -- each std::thread costs ~50-100us to spawn on
// Windows, so with ~20 workers across ~4 sites that was milliseconds/frame of pure thread-creation churn,
// and it never kept the cores warm. Here the workers are created ONCE and sleep on a condition variable
// between dispatches; parallel_for wakes them, runs fn(i) for i in [0,count) across the workers AND the
// calling thread, and blocks until every item is done. CPU-ONLY work (no GL on workers), matching the
// sites it replaces. Opt out with NOGGIT_NO_THREAD_POOL=1 (falls back to serial).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace noggit
{
  class RenderThreadPool
  {
  public:
    explicit RenderThreadPool(unsigned threads)
    {
      // Total parallelism = 1 (the calling thread) + N workers, so spawn threads-1 persistent workers.
      unsigned const n = threads < 2u ? 0u : threads - 1u;
      _workers.reserve(n);
      for (unsigned i = 0; i < n; ++i)
      {
        _workers.emplace_back([this] { workerLoop(); });
      }
    }

    ~RenderThreadPool()
    {
      {
        std::lock_guard<std::mutex> lk(_m);
        _stop = true;
        ++_gen;
      }
      _cv.notify_all();
      for (auto& w : _workers)
      {
        if (w.joinable()) { w.join(); }
      }
    }

    RenderThreadPool(RenderThreadPool const&) = delete;
    RenderThreadPool& operator=(RenderThreadPool const&) = delete;

    std::size_t parallelism() const { return _workers.size() + 1; }

    // Run fn(i) for every i in [0, count), distributed across the workers + the calling thread. Blocks
    // until all items complete. fn must be CPU-only (no GL calls) and thread-safe across distinct i.
    //
    // min_parallel is the smallest batch worth dispatching. The default of 3 suits many small items;
    // a caller whose items are individually expensive should pass 2, because then the dispatch cost
    // (tens of microseconds) is nothing against the item. The water derive is exactly that case: at
    // 3-13 ms per tile, a 2-tile crossing was running fully serially for want of one comparison.
    void parallel_for(std::size_t count, std::function<void(std::size_t)> const& fn,
                      std::size_t min_parallel = 3)
    {
      if (count == 0) { return; }
      // Serial for a single worker, an empty pool, or a batch below the caller's threshold.
      if (_workers.empty() || count < min_parallel)
      {
        for (std::size_t i = 0; i < count; ++i) { fn(i); }
        return;
      }

      {
        std::lock_guard<std::mutex> lk(_m);
        _fn = &fn;                                  // points at the caller's local fn; valid until we return
        _count = count;
        // [finding 132] Claim work in CHUNKS. One item per fetch_add meant two contended atomic
        // RMWs per iteration, which swamps any loop whose body is cheap. Aim for ~8 chunks per
        // worker: enough to stay load-balanced, few enough that the atomics disappear.
        _chunk = std::max<std::size_t>(1, count / (parallelism() * 8));
        _next.store(0, std::memory_order_relaxed);
        _remaining.store(count, std::memory_order_relaxed);
        ++_gen;
      }
      _cv.notify_all();

      runItems();                                   // the calling thread participates

      {
        std::unique_lock<std::mutex> lk(_done_m);
        _done_cv.wait(lk, [this] { return _remaining.load(std::memory_order_acquire) == 0; });
      }
      {
        std::lock_guard<std::mutex> lk(_m);
        _fn = nullptr;                              // drop the dangling pointer to the caller's local fn
      }
    }

  private:
    void runItems()
    {
      // _fn is published under _m before the workers wake / before the caller calls this, and every item is
      // done (remaining==0) before parallel_for clears it, so this read is safe without the lock.
      std::function<void(std::size_t)> const* const fn = _fn;
      std::size_t const count = _count;
      std::size_t const chunk = _chunk ? _chunk : 1;
      std::size_t i;
      while ((i = _next.fetch_add(chunk, std::memory_order_relaxed)) < count)
      {
        std::size_t const stop = std::min(count, i + chunk);
        for (std::size_t k = i; k < stop; ++k)
        {
          (*fn)(k);
        }
        std::size_t const did = stop - i;
        if (_remaining.fetch_sub(did, std::memory_order_acq_rel) == did)
        {
          std::lock_guard<std::mutex> lk(_done_m);
          _done_cv.notify_all();
        }
      }
    }

    void workerLoop()
    {
      std::uint64_t last = 0;
      while (true)
      {
        {
          std::unique_lock<std::mutex> lk(_m);
          _cv.wait(lk, [this, &last] { return _stop || _gen != last; });
          if (_stop) { return; }
          last = _gen;
        }
        runItems();
      }
    }

    std::vector<std::thread> _workers;
    std::mutex _m;
    std::condition_variable _cv;
    std::mutex _done_m;
    std::condition_variable _done_cv;
    std::function<void(std::size_t)> const* _fn = nullptr;
    std::size_t _count = 0;
    std::size_t _chunk = 1;   // finding 132: items claimed per fetch_add
    std::atomic<std::size_t> _next{0};
    std::atomic<std::size_t> _remaining{0};
    std::uint64_t _gen = 0;
    bool _stop = false;
  };

  // Process-wide render worker pool, created once (sized to the hardware) on first use. Returns nullptr
  // when NOGGIT_NO_THREAD_POOL=1 so callers fall back to their serial/legacy path.
  inline RenderThreadPool* render_pool()
  {
    static bool const disabled = []
    {
      char const* v = std::getenv("NOGGIT_NO_THREAD_POOL");
      return v && *v && *v != '0';
    }();
    if (disabled) { return nullptr; }
    static RenderThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    return &pool;
  }
}
