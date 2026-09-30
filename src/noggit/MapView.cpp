// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/rendering/vulkan/VkParticleFeed.hpp>
#include <noggit/rendering/RenderDiagnostics.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapView.h>
namespace Noggit { void printStacktrace(); }   // error_handling.cpp (StackWalker on WIN32)
#include <noggit/Misc.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h> // WMOManager (mem-diag)
#include <noggit/WMOInstance.h> // WMOInstance
#ifdef _WIN32
#include <windows.h>
#include <psapi.h> // GetProcessMemoryInfo (mem-diag)
#endif
#include <noggit/World.h>
#include <noggit/rendering/vulkan/VulkanBackend.hpp> // [VULKAN PHASE 0] interop proof of life (win32-gated inside)
#include <noggit/map_index.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/DetailInfos.h> // detailInfos
#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/Help.h>
#include <noggit/ui/HelperModels.h>
#include <noggit/ui/ModelImport.h>
#include <noggit/ui/ObjectEditor.h>
#include <noggit/ui/RotationEditor.h>
#include <noggit/ui/TexturePicker.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/ZoneMusicPlayer.hpp>
#include <noggit/ui/SfxPlayer.hpp>
#include <noggit/ui/WaterSoundPlayer.hpp>

// [VULKAN harness] silences every audio path during off-screen parity runs (set in muteAudioForHarness)
bool Noggit::Rendering::g_noggit_harness_silent = false;

#ifdef _WIN32
// [VULKAN] BLP path -> bindless texture index, shared by the terrain tilesets and the M2 batches.
static std::unordered_map<std::string, std::int32_t> s_vk_tex_ids;

// [finding 83] BACKGROUND TILESET DECODER.
//
// Decoding a tileset BLP is pure CPU work, but batching the decodes of one crossing onto the
// render pool bought almost nothing (7 textures: 4.17 ms serial -> 3.42 ms parallel), because the
// archive read underneath serialises. Parallelism is therefore the wrong lever -- the work has to
// happen on a DIFFERENT frame, not on more threads of the same one.
//
// This thread decodes ahead: the neighbourhood knows which tiles it has not packed yet, so their
// tilesets are queued as soon as the tile appears and are usually decoded by the time the pack
// budget reaches that tile. The render thread then only creates the VK texture (~0.1 ms each).
// Anything not ready in time is decoded inline exactly as before, so this can only be a win.
namespace
{
  // [2026-09-05] Last M2 frame counts, so the 60-frame NATIVE HOLES line can carry them. The
  // M2-frame log fires every 300 frames, which a short session samples at most once -- not enough
  // to tell a still-streaming scene from a broken one.
  std::size_t& vkLastM2Draws()     { static std::size_t v = 0; return v; }
  std::size_t& vkLastM2Instances() { static std::size_t v = 0; return v; }
  std::size_t& vkLastM2TexPairs()  { static std::size_t v = 0; return v; }
  std::size_t& vkLastM2Bones()     { static std::size_t v = 0; return v; }
  // texture pairs that resolved to a NEGATIVE bindless id (BLP not resident): those models draw
  // untextured/discarded -- invisible -- while the few grass BLPs resolve early and show.
  std::size_t& vkLastM2UnresolvedPairs() { static std::size_t v = 0; return v; }

  struct VkPfDecoded
  {
    bool ok = false, compressed = false;
    std::uint32_t w = 0, h = 0;
    VkFormat vf = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    std::vector<std::vector<std::uint8_t>> mips;
  };
  std::mutex s_pf_mtx;
  std::condition_variable s_pf_cv;
  std::deque<std::string> s_pf_queue;
  std::map<std::string, VkPfDecoded> s_pf_ready;
  std::set<std::string> s_pf_seen;          // queued or ready; never queue the same name twice
  std::thread s_pf_thread;
  bool s_pf_stop = false;

  // Decodes one tileset BLP. No GL and no VK: NoggitRenderContext is an enum tag, and the archive
  // read is the same one AsyncLoader already performs on its own workers.
  VkPfDecoded vkDecodeTileset(std::string const& name, Noggit::NoggitRenderContext ctx)
  {
    VkPfDecoded d;
    try
    {
      blp_texture tex(BlizzardArchive::Listfile::FileKey(name), ctx);
      tex.finishLoading();
      d.w = static_cast<std::uint32_t>(tex.width());
      d.h = static_cast<std::uint32_t>(tex.height());
      if (!d.w || !d.h) return VkPfDecoded{};
      if (tex.compression_format())
      {
        GLint const cf = *tex.compression_format();
        d.compressed = true;
        d.vf = (cf == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) ? VK_FORMAT_BC2_UNORM_BLOCK
             : (cf == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) ? VK_FORMAT_BC3_UNORM_BLOCK
             : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        for (auto const& [lvl, bytes] : tex.compressed_data()) { (void)lvl; d.mips.push_back(bytes); }
      }
      else if (!tex.data().empty())
      {
        for (auto const& [lvl, px] : tex.data())
        {
          (void)lvl;
          auto const* p8 = reinterpret_cast<std::uint8_t const*>(px.data());
          d.mips.emplace_back(p8, p8 + px.size() * 4u);
        }
      }
      d.ok = !d.mips.empty();
    }
    catch (std::exception const& e)
    {
      LogError << "[VK] tileset decode failed for " << name << ": " << e.what() << std::endl;
      return VkPfDecoded{};
    }
    return d;
  }

  void vkTilesetDecodeLoop(Noggit::NoggitRenderContext ctx)
  {
    for (;;)
    {
      std::string name;
      {
        std::unique_lock<std::mutex> lk(s_pf_mtx);
        s_pf_cv.wait(lk, [] { return s_pf_stop || !s_pf_queue.empty(); });
        if (s_pf_stop) return;
        name = std::move(s_pf_queue.front());
        s_pf_queue.pop_front();
      }
      VkPfDecoded d = vkDecodeTileset(name, ctx);   // decoded OUTSIDE the lock
      std::lock_guard<std::mutex> lk(s_pf_mtx);
      // publish the result either way. A failure resolves to a permanent id of -1 in the caller,
      // so a name that cannot be decoded stops being pending instead of being re-queued forever.
      s_pf_ready.emplace(std::move(name), std::move(d));
    }
  }
}
#endif

#include <noggit/ui/CreatureInfoPanel.hpp>
#include <noggit/ui/Toolbar.h> // Noggit::Ui::toolbar
#include <noggit/ui/Water.h>
#include <noggit/ui/ZoneIDBrowser.h>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/ShaderTool.hpp>
#include <noggit/ui/TerrainTool.hpp>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/hole_tool.hpp>
#include <noggit/ui/texture_palette_small.hpp>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/frame_profiler.hpp>
#include <opengl/scoped.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/ui/tools/TimeGlobe/TimeGlobeWidget.hpp>
#include <QtWidgets/QAction>
#include <QtWidgets/QToolBar>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/AssetBrowser/ModelView.hpp>
#include <noggit/ui/tools/PresetEditor/Ui/PresetEditor.hpp>
#include <noggit/ui/tools/NodeEditor/Ui/NodeEditor.hpp>
#include <noggit/ui/tools/UiCommon/ImageBrowser.hpp>
#include <noggit/ui/tools/BrushStack/BrushStack.hpp>
#include <noggit/ui/tools/LightEditor/LightEditor.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkManipulatorPanel.hpp>
#include <external/imguipiemenu/PieMenu.hpp>
#include <external/tracy/Tracy.hpp>
#include <noggit/ui/object_palette.hpp>
#include <external/glm/gtc/type_ptr.hpp>

#ifdef _WIN32
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QWindow>

// [VULKAN NATIVE PRESENT, 2026-09-03] WM_NCHITTEST -> HTTRANSPARENT on the present child window:
// the OS skips it during hit-testing, so every mouse event lands on the main window and Qt routes
// it to the widget under the cursor exactly as in GL mode. Keyboard focus never moves (the child
// is never activated by a click).
namespace
{
  struct VkPresentHitFilter : QAbstractNativeEventFilter
  {
    std::unordered_set<void*> hwnds;
    bool nativeEventFilter(QByteArray const& type, void* message, long* result) override
    {
      if (type != "windows_generic_MSG")
        return false;
      MSG* const msg = static_cast<MSG*>(message);
      if (msg->message == WM_NCHITTEST && hwnds.count(reinterpret_cast<void*>(msg->hwnd)))
      {
        *result = HTTRANSPARENT;
        return true;
      }
      return false;
    }
  };
  VkPresentHitFilter& vkPresentHitFilter()
  {
    static VkPresentHitFilter f;
    return f;
  }
}
// [VULKAN NATIVE PRESENT] reachable from resizeGL / captureFrameNow, which live outside draw_map's
// static interop block. Pointer only -- the object is the function-local static backend inside
// draw_map (it lives to process exit).
namespace Noggit::Rendering::VK { class VulkanBackend; }
static Noggit::Rendering::VK::VulkanBackend* g_vk_capture_backend = nullptr;
#endif
#include <opengl/types.hpp>
#include <limits>
#include <variant>
#include <noggit/Selection.h>
#include <math/ray.hpp>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>
#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/ui/npc/NpcWorkflow.hpp>
#include <noggit/ui/quest/QuestBrowserDialog.hpp>
#include <noggit/ui/content/SqlApply.hpp>

using Noggit::Ui::confirmSqlApply;
using Noggit::Ui::reportSqlResult;

#include <QtCore/QSettings>
#include <noggit/MySqlSettings.hpp>
#include <noggit/ssh/SshTunnelManager.hpp>
#endif

#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_settings.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>

#include <noggit/ui/FontNoggit.hpp>

#include "revision.h"

#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>

namespace
{
  // Game Mode panel: display-id spinner that steps through REAL CreatureDisplayInfo rows instead
  // of every integer (most of which are empty ids that resolve to nothing).
  class DisplayIdSpinBox : public QSpinBox
  {
  public:
    using QSpinBox::QSpinBox;
    std::function<std::vector<std::uint32_t> const*()> row_ids;
    void stepBy(int steps) override
    {
      std::vector<std::uint32_t> const* rows = row_ids ? row_ids() : nullptr;
      if (!rows || rows->empty())
      {
        QSpinBox::stepBy(steps);
        return;
      }
      auto const cur = static_cast<std::uint32_t>(std::max(0, value()));
      auto const it = std::lower_bound(rows->begin(), rows->end(), cur);
      long long idx = it - rows->begin();
      if (steps > 0 && !(it != rows->end() && *it == cur))
      {
        idx += steps - 1; // stepping up from a non-row lands on the next real row
      }
      else
      {
        idx += steps;
      }
      idx = std::clamp<long long>(idx, 0, static_cast<long long>(rows->size()) - 1);
      setValue(static_cast<int>((*rows)[static_cast<std::size_t>(idx)]));
    }
  };

  // Bind-pose attachment lookup for the picker preview (copy of WorldRender's find_attachment_def:
  // direct lookup, classic-layout sanity fallback scan).
  ModelAttachmentDef const* preview_find_attachment_def(Model const* model, int attachment_id)
  {
    if (!model || attachment_id < 0
        || static_cast<std::size_t>(attachment_id) >= model->_attachment_lookup.size())
    {
      return nullptr;
    }
    auto const lookup = model->_attachment_lookup[attachment_id];
    auto const attachment_is_sane = [model](ModelAttachmentDef const& attachment)
    {
      return attachment.bone < model->header.nBones
          && std::isfinite(attachment.pos.x)
          && std::isfinite(attachment.pos.y)
          && std::isfinite(attachment.pos.z);
    };
    if (lookup >= 0 && static_cast<std::size_t>(lookup) < model->_attachments.size())
    {
      auto const& attachment = model->_attachments[lookup];
      if (!model->usesClassicLayout() || attachment_is_sane(attachment))
      {
        return &attachment;
      }
    }
    if (model->usesClassicLayout())
    {
      for (auto const& attachment : model->_attachments)
      {
        if (static_cast<int>(attachment.id) == attachment_id && attachment_is_sane(attachment))
        {
          return &attachment;
        }
      }
    }
    return nullptr;
  }

  // Creature editor: wander_distance spinner that reports keyboard focus, so the map view can show
  // the wander-radius ground ring exactly while the field is highlighted/being edited.
  class FocusReportingDoubleSpinBox : public QDoubleSpinBox
  {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;
    std::function<void(bool)> on_focus;
  protected:
    void focusInEvent(QFocusEvent* e) override
    {
      QDoubleSpinBox::focusInEvent(e);
      if (on_focus) on_focus(true);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
      QDoubleSpinBox::focusOutEvent(e);
      if (on_focus) on_focus(false);
    }
  };
}
#include <QtWidgets/QRadioButton>
#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QIcon>
#include <QWidgetAction>
#include <QActionGroup>
#include <QSlider>
#include <QHBoxLayout>
#include <QLabel>
#include <QSurfaceFormat>
#include <QMessageBox>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QDateTime>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QProgressDialog>
#include <QProcess>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QOpenGLExtraFunctions> // [VULKAN phase A] glFenceSync/glClientWaitSync for the GL->VK hard sync
#include <noggit/MapHeaders.h>    // [VULKAN phase B] MCLYFlags for the textured-terrain feed
#include <noggit/texture_set.hpp>

// [VULKAN 2026-08-29] settings-driven backend selection; definitions live in `namespace vk_diff` above
// MapView::draw_map (forward-declared here so initializeGL can seed them from QSettings).
// [phase I] where the GL<->VK interop actually costs: the CPU-side fence wait before VK may reuse
// the shared images, and the semaphore wait before GL may sample them. Subtraction said ~73 ms/frame
// was going somewhere that was neither VK recording nor VK GPU work; these name it.
inline double& vk_stat_gl_sync_ms() { static double v = 0.0; return v; }
// Per-phase CPU cost of BUILDING the VK feeds. Subtraction said ~20 ms/frame is still unaccounted
// after recording, GPU and interop; these say which feed it is instead of guessing again.
inline double& vk_stat_tile_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_glscene_ms() { static double v = 0.0; return v; }
inline double& vk_stat_vkblock_ms() { static double v = 0.0; return v; }
// Everything paintGL does BEFORE the scene traversal / VK block. Measured in BOTH apis: the same
// region costs ~3 ms more in VK mode than in GL mode and nothing accounted for it.
inline double& vk_stat_prevk_ms() { static double v = 0.0; return v; }
// set each frame from the VK block; lets the scene-spike log report whether VK still owned terrain
inline bool& s_vk_backend_tt_ready() { static bool v = false; return v; }
// paintGL split into [before draw_map] / [draw_map] / [after draw_map]. VK's paintGL costs ~3.9 ms
// more than GL's and only ~1 ms of it is inside the instrumented VK block, so the rest is in one of
// these three and no timer covered them.
inline double& vk_stat_pg_pre_ms() { static double v = 0.0; return v; }
inline double& vk_stat_pg_map_ms() { static double v = 0.0; return v; }
// Second accumulator for the SAME span as vk_stat_vkblock_ms, reported on the harness's 300-frame
// window instead of draw_map's own. The two reporters count different things (draw_map executions vs
// harness frames) so their windows cover different stretches of a flight, and subtracting one from
// the other produced a 2.9 ms "gap" in code that does not exist.
inline double& vk_stat_block2_ms() { static double v = 0.0; return v; }
// The VK block minus its named phases still had ~1.7 ms nobody owned. Split it either side of the
// traversal call: MID = end of PREP -> traversal (record + submit + semaphore wait), TAIL = traversal
// -> end of block (readback/blit/signal/fence).
inline double& vk_stat_mid_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_tail_ms() { static double v = 0.0; return v; }
// PREP/mid/tail are all accumulated inside `if (s_vk.ok && backend.ready())` while the block timer
// runs unconditionally, so a frame that fails that check is counted in the block and in NO phase.
// Count the frames each side actually saw: if they differ, the "unnamed" time is just that.
inline double& vk_stat_blk_frames()  { static double v = 0.0; return v; }
inline double& vk_stat_prep_frames() { static double v = 0.0; return v; }
inline double& vk_stat_cull_ms()    { static double v = 0.0; return v; }
// [finding 114] bumped whenever the chunk/water bounds arrays are rebuilt, so the frustum cull
// can tell "same camera, same geometry" from "something moved" without comparing the arrays.
inline std::uint64_t& vk_bounds_generation() { static std::uint64_t g = 0; return g; }
inline double& vk_stat_m2up_ms()    { static double v = 0.0; return v; }
inline double& vk_stat_compose_ms() { static double v = 0.0; return v; }
// The compose PASS itself runs inside WorldRender::draw (pre_scene_compose), so it needs its
// own timer -- vk_stat_compose_ms covers only the wait+semaphore+setup that precedes the scene.
inline double& vk_stat_composepass_ms() { static double v = 0.0; return v; }
// vk block minus the GL scene pass is ~1.65 ms and only ~0.67 of it is accounted for by the
// named feeds. Bracket the block: PREP is everything before renderFrame (feeds, uploads, sky,
// cull), POST is everything after the scene draw (probe, blit, signal).
inline double& vk_stat_prep_ms() { static double v = 0.0; return v; }
inline double& vk_stat_post_ms() { static double v = 0.0; return v; }
// The GL scene pass has only ever been REPORTED from inside the VK block, so its cost in pure
// GL mode -- the thing the gates are supposed to be reducing -- was never actually measured.
// This accumulator is reported from the scene lambda itself, so it prints under either API.
inline double& vk_stat_glscene_any_ms() { static double v = 0.0; return v; }
inline double& vk_stat_walk_glcalls() { static double v = 0.0; return v; }
// PREP is 1.39 ms and its named feeds only account for 0.60. Split it in three to find the rest:
// S1 = tiles/terrain/water/M2/particles, S2 = lighting+sky+cloud+celestials, S3 = WMO frame.
inline double& vk_stat_prep1_ms() { static double v = 0.0; return v; }
inline double& vk_stat_prep2_ms() { static double v = 0.0; return v; }
inline double& vk_stat_prep3_ms() { static double v = 0.0; return v; }
// S2 came back the largest at 0.60 ms, which is absurd for a 768-vertex dome -- split it too.
inline double& vk_stat_s2sky_ms() { static double v = 0.0; return v; }
inline double& vk_stat_s2cel_ms() { static double v = 0.0; return v; }
inline double& vk_stat_wpairs_ms() { static double v = 0.0; return v; }
// finding 86: the "pairs" cost survived two plausible explanations (per-name mutex+notify, then
// unbudgeted texture creation). Split it rather than guess a third time.
inline double& vk_stat_wp_sweep_ms() { static double v = 0.0; return v; }
inline double& vk_stat_wp_lock_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_wp_make_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_wp_idof_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_wp_need()     { static double v = 0.0; return v; }
inline double& vk_stat_warena_ms() { static double v = 0.0; return v; }
inline double& vk_stat_wtable_ms() { static double v = 0.0; return v; }
inline double& vk_stat_s2part_ms() { static double v = 0.0; return v; }
inline double& vk_stat_s2wmo_ms() { static double v = 0.0; return v; }
// sky came back at 1.39 ms -- the WHOLE VK-only overhead sits in this one block. Split it.
inline double& vk_stat_skdome_ms() { static double v = 0.0; return v; }
inline double& vk_stat_skcmesh_ms() { static double v = 0.0; return v; }
inline double& vk_stat_skctex_ms() { static double v = 0.0; return v; }
inline double& vk_stat_wliq_ms()  { static double v = 0.0; return v; }
inline double& vk_stat_m2_ms()    { static double v = 0.0; return v; }
inline double& vk_stat_part_ms()  { static double v = 0.0; return v; }
struct VkPhaseTimer
{
  double& sink;
  std::chrono::steady_clock::time_point t0;
  explicit VkPhaseTimer(double& s) : sink(s), t0(std::chrono::steady_clock::now()) {}
  ~VkPhaseTimer() { sink += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count(); }
};
inline double& vk_stat_sem_ms() { static double v = 0.0; return v; }

namespace vk_diff
{
  inline int& apiMode();
  inline bool& parityCheck();
  inline std::string& camsPath();
  inline bool& forced();
  inline bool& finished();
  inline bool& vkReady();
  inline int& meshTileX();
  inline int& meshTileZ();
}
#include <QElapsedTimer>
#include <QTimer>

#include <QClipboard>
#include <QTextStream>

#include <algorithm>
#include <sstream>
#include <cctype>
#include <cmath>
#include <cstring>
#include <noggit/render_thread_pool.hpp>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include <vector>
#include <random>

namespace
{
  bool capture_debug_enabled()
  {
    if (char const* value = std::getenv("NOGGIT_CAPTURE_DEBUG"))
    {
      return std::string(value) != "0";
    }

    return false;
  }

  bool creature_capture_overlay_enabled()
  {
    if (char const* value = std::getenv("NOGGIT_CAPTURE_CREATURES"))
    {
      return std::string(value) != "0";
    }

    return true;
  }

  glm::vec3 server_to_client_creature_position(float server_x, float server_y, float server_z, bool global_wmo_map)
  {
    if (global_wmo_map)
    {
      return {-server_y, server_z, -server_x};
    }

    return {ZEROPOINT - server_y, server_z, ZEROPOINT - server_x};
  }

  glm::vec3 client_to_server_creature_position(glm::vec3 const& client_pos, bool global_wmo_map)
  {
    if (global_wmo_map)
    {
      return {-client_pos.z, -client_pos.x, client_pos.y};
    }

    return {ZEROPOINT - client_pos.z, ZEROPOINT - client_pos.x, client_pos.y};
  }

  float client_to_server_creature_orientation(float client_orientation)
  {
    auto orientation = glm::radians(client_orientation + 180.0f);
    auto full_rotation = glm::two_pi<float>();
    orientation = std::fmod(orientation, full_rotation);
    if (orientation < 0.0f)
    {
      orientation += full_rotation;
    }

    return orientation;
  }

  QString creature_type_label(std::uint32_t creature_type)
  {
    switch (creature_type)
    {
      case 1:  return "Beast";
      case 2:  return "Dragonkin";
      case 3:  return "Demon";
      case 4:  return "Elemental";
      case 5:  return "Giant";
      case 6:  return "Undead";
      case 7:  return "Humanoid";
      case 8:  return "Critter";
      case 9:  return "Mechanical";
      case 10: return "Not specified";
      case 11: return "Totem";
      case 12: return "Non-combat Pet";
      case 13: return "Gas Cloud";
      default: return QString("Type %1").arg(creature_type);
    }
  }

  // NPC/GO picker 3D preview with an ORBIT camera: the camera hangs on a fixed "pole" aimed at the
  // model's center -- dragging (either button) spins around the model (yaw/pitch), the wheel zooms
  // along the pole, and every new model auto-frames as close as the near plane and the frustum
  // allow. The base class's free-fly camera (WASD + look) is fully disabled here; the asset
  // browser keeps it.
  class CreaturePreviewModelViewer final : public Noggit::Ui::Tools::AssetBrowser::ModelViewer
  {
  public:
    explicit CreaturePreviewModelViewer(QWidget* parent = nullptr)
      : Noggit::Ui::Tools::AssetBrowser::ModelViewer(parent, Noggit::NoggitRenderContext::ASSET_BROWSER)
    {
      // The WORLD view gets its antialiasing from its own multisampled offscreen FBO
      // (render/msaa, WorldRender::setupBloom) -- not from the widget framebuffer -- so preview
      // widgets were rendering with ZERO samples and every model edge aliased. Request the same
      // MSAA level on this widget's backing framebuffer (must happen before the widget is realized).
      {
        int msaa = QSettings().value("render/msaa", 4).toInt();
        // Dev-only override for the VK parity harness: GL renders 4x MSAA by default while the VK
        // backend is still single-sampled, so every silhouette differs. NOGGIT_MSAA isolates that.
        if (char const* env_msaa = std::getenv("NOGGIT_MSAA"))
          msaa = std::atoi(env_msaa);
        if (msaa != 0 && msaa != 2 && msaa != 4 && msaa != 8)
        {
          msaa = 4;
        }
        if (msaa > 0)
        {
          QSurfaceFormat fmt = format();
          fmt.setSamples(msaa);
          setFormat(fmt);
        }
      }

      // Re-frame on widget resizes (the fit depends on the aspect ratio); keep the user's zoom
      // unless they were sitting at the default.
      connect(this, &Noggit::Ui::Tools::AssetBrowser::ModelViewer::resized, [this]
      {
        bool const at_default_zoom = std::abs(_orbit_distance - _fit_distance) < 0.01f;
        refitCamera(at_default_zoom);
      });
    }

    std::function<void()> on_double_click;
    // The NPC editor's look (display ID, scale) waiting for the GL context.
    std::optional<std::pair<std::uint32_t, float>> pending_look;

    void setModel(std::string const& filename) override
    {
      _preview_attachment_ids.clear(); // the base setModel replaces the instance list
      Noggit::Ui::Tools::AssetBrowser::ModelViewer::setModel(filename);
      refitCamera(true);
    }

    void setCreatureSpawnPreview(World& world, World::CreatureSpawnOverlay const& spawn)
    {
      setModel(spawn.model_path);
      if (_model_instances.empty())
      {
        return;
      }

      auto& instance = _model_instances.front();
      instance.scale = std::clamp(spawn.template_scale * spawn.model_scale,
                                  ModelInstance::min_scale(),
                                  ModelInstance::max_scale());
      instance.dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      instance.updateTransformMatrix();
      world.applyCreatureSpawnModelAppearance(spawn, instance, _context);
      instance.recalcExtents();

      // Attachments (helm / shoulders / weapons): the world draws these as separate models on the
      // body's attachment points -- without them a helmeted NPC previews BALD (the helmet rule hid
      // the hair, and nothing drew the helmet: Lakeshire Guard 10037). Bind pose: bone matrices are
      // identity, so the point is just parent_transform x translate(fixCoordSystem(attachment.pos)).
      for (auto const& spec : world.resolveCreaturePreviewAttachments(spawn))
      {
        try
        {
          auto& att = _model_instances.emplace_back(BlizzardArchive::Listfile::FileKey(spec.model_path), _context);
          att.model->wait_until_loaded();
          if (att.model->loading_failed())
          {
            _model_instances.pop_back();
            continue;
          }
          for (auto const& tex : spec.texture_overrides)
          {
            att.setReplaceTexture(tex.first, tex.second);
          }
          // Express the placement through pos/dir/scale (NOT setTransformMatrix): the draw path's
          // lazy recalcExtents rebuilds the matrix from these, which wiped a directly-set matrix
          // and dropped every attachment to the body's origin (helmets at the feet). For this
          // rigid composition the two forms are identical: body x T(att) == T(body x att_point) x
          // R(body yaw) x S(body scale).
          auto& body = _model_instances.front();
          glm::vec3 att_point = body.pos;
          if (auto const* def = preview_find_attachment_def(body.model.get(), spec.attachment_id))
          {
            att_point = glm::vec3(body.transformMatrix() * glm::vec4(fixCoordSystem(def->pos), 1.0f));
          }
          att.pos = att_point;
          att.dir = body.dir;
          att.scale = body.scale;
          att.updateTransformMatrix();
          att.recalcExtents();
          _preview_attachment_ids.push_back(spec.attachment_id);
        }
        catch (...)
        {
          // missing/broken attachment model -> preview the body alone
        }
      }

      // Face the creature from a three-quarter front view, slightly above: the camera's look yaw
      // is opposite the model's facing.
      _orbit_yaw = spawn.orientation + 205.0f;
      _orbit_pitch = 12.0f;
      refitCamera(true);
    }

  protected:
    // attachment_id per extra instance (_model_instances[1..]), parallel order
    std::vector<int> _preview_attachment_ids;

    // Animate the attachments with the body's skeleton: re-anchor each on its parent bone's CURRENT
    // matrix every frame, exactly like the world's attachment draw -- the bind-pose-only placement
    // left helmets/weapons frozen mid-air while the body idled. Uses the bone matrices the body's
    // previous draw computed (one frame of lag, invisible). setRenderAnchor feeds the shader
    // directly, so the lazy extent/transform rebuilds can't clobber it.
    void tick(float dt) override
    {
      Noggit::Ui::Tools::AssetBrowser::ModelViewer::tick(dt);
      if (_model_instances.size() < 2 || _preview_attachment_ids.empty())
      {
        return;
      }
      auto& body = _model_instances.front();
      if (!body.model->finishedLoading() || body.model->bone_matrices.empty())
      {
        return;
      }
      for (std::size_t i = 1; i < _model_instances.size() && i - 1 < _preview_attachment_ids.size(); ++i)
      {
        auto& att = _model_instances[i];
        auto const* def = preview_find_attachment_def(body.model.get(), _preview_attachment_ids[i - 1]);
        if (!def)
        {
          continue;
        }
        glm::mat4x4 att_local = glm::translate(glm::mat4x4(1.0f), fixCoordSystem(def->pos));
        if (def->bone >= 0 && static_cast<std::size_t>(def->bone) < body.model->bone_matrices.size())
        {
          att_local = body.model->bone_matrices[def->bone] * att_local;
        }
        glm::mat4x4 const world = body.transformMatrix() * att_local;
        glm::mat4x4 rel = world;
        rel[3] = glm::vec4(glm::vec3(world[3]) - body.pos, 1.0f);
        att.setRenderAnchor(body.pos, rel);
      }
    }

    // --- orbit ("pole") camera state ---
    float _orbit_yaw = 25.0f;       // camera look yaw, degrees
    float _orbit_pitch = 12.0f;     // degrees above horizontal
    float _orbit_distance = 12.0f;  // pole length
    float _fit_distance = 12.0f;    // the auto-framed default zoom
    float _scene_radius = 1.0f;     // fit-box worst-case horizontal half-extent
    float _scene_half_height = 1.0f;
    float _orbit_height_offset = 0.0f; // vertical pivot pan (middle-drag); pole stays centered in XZ
    glm::vec3 _orbit_center = glm::vec3(0.0f);
    bool _orbiting = false;
    bool _panning = false;

    void applyOrbitCamera()
    {
      _orbit_pitch = std::clamp(_orbit_pitch, -80.0f, 80.0f);
      _camera.yaw(math::degrees(_orbit_yaw));
      _camera.pitch(math::degrees(_orbit_pitch));
      // The camera always LOOKS at the (vertically pannable) pivot; backing up along its own view
      // direction is the pole (the same trick PreviewRenderer::resetCamera uses to frame a scene).
      _camera.position = _orbit_center + glm::vec3(0.0f, _orbit_height_offset, 0.0f);
      _camera.move_forward_factor(-1.0f, _orbit_distance);
    }

    // The box the auto-framing fits. The instance extents union the header's anim-padded bounding
    // box with the collision box (weapon-swing/effect space -- often 2-3x the visible mesh), which
    // framed models tiny in lots of empty space; and header boxes themselves are unreliable
    // per-model (dragonkin/whelps author huge or degenerate ones -- The Beast, Lost Whelp,
    // Rivendare all framed far). Ground truth = the MESH: the bind-pose vertex AABB transformed by
    // the instance (Model::_vertices are already in the noggit frame from load-time
    // fixCoordSystem). Fallbacks: collision box -> bounding box -> scene extents (WMO previews).
    std::vector<glm::vec3> fitExtents()
    {
      if (!_model_instances.empty())
      {
        // front() = the previewed BODY (attachment instances are appended after it)
        auto const& inst = _model_instances.front();
        if (inst.model->finishedLoading() && !inst.model->loading_failed())
        {
          glm::mat4x4 const mat = inst.transformMatrix();

          if (!inst.model->_vertices.empty())
          {
            glm::vec3 wmin(std::numeric_limits<float>::max());
            glm::vec3 wmax(std::numeric_limits<float>::lowest());
            std::size_t used = 0;
            for (auto const& v : inst.model->_vertices)
            {
              if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z))
              {
                continue;
              }
              glm::vec3 const world = glm::vec3(mat * glm::vec4(v.position, 1.0f));
              wmin = glm::min(wmin, world);
              wmax = glm::max(wmax, world);
              ++used;
            }
            if (used > 2 && wmin.x < wmax.x && wmin.y < wmax.y && wmin.z < wmax.z)
            {
              return {wmin, wmax};
            }
          }

          auto const& hdr = inst.model->header;
          auto const finite_box = [](glm::vec3 const& a, glm::vec3 const& b)
          {
            return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)
                && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z)
                && a.x < b.x && a.y < b.y && a.z < b.z;
          };
          glm::vec3 bmin(0.0f), bmax(0.0f);
          bool have_box = false;
          if (finite_box(hdr.collision_box_min, hdr.collision_box_max))
          {
            bmin = hdr.collision_box_min;
            bmax = hdr.collision_box_max;
            have_box = true;
          }
          if (!have_box && finite_box(hdr.bounding_box_min, hdr.bounding_box_max))
          {
            bmin = hdr.bounding_box_min;
            bmax = hdr.bounding_box_max;
            have_box = true;
          }
          if (have_box)
          {
            glm::vec3 wmin(std::numeric_limits<float>::max());
            glm::vec3 wmax(std::numeric_limits<float>::lowest());
            for (int i = 0; i < 8; ++i)
            {
              glm::vec3 const corner((i & 1) ? bmax.x : bmin.x,
                                     (i & 2) ? bmax.y : bmin.y,
                                     (i & 4) ? bmax.z : bmin.z);
              glm::vec3 const world = glm::vec3(mat * glm::vec4(misc::transform_model_box_coords(corner), 1.0f));
              wmin = glm::min(wmin, world);
              wmax = glm::max(wmax, world);
            }
            return {wmin, wmax};
          }
        }
      }
      return calcSceneExtents();
    }

    // Recompute the model's fit box and the auto-framed distance. reset_zoom snaps the pole back
    // to the default fit (and clears the vertical pan); otherwise the current zoom is kept.
    void refitCamera(bool reset_zoom)
    {
      auto const extents = fitExtents();
      auto const valid = [](glm::vec3 const& v)
      {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
      };
      glm::vec3 half(1.0f);
      if (!valid(extents[0]) || !valid(extents[1])
          || extents[0].x > extents[1].x
          || extents[0].y > extents[1].y
          || extents[0].z > extents[1].z)
      {
        _orbit_center = glm::vec3(0.0f);
      }
      else
      {
        _orbit_center = (extents[0] + extents[1]) * 0.5f;
        half = glm::max((extents[1] - extents[0]) * 0.5f, glm::vec3(0.05f));
      }
      // Worst-case horizontal half-extent while orbiting = the XZ half-diagonal; vertical = half.y.
      _scene_half_height = half.y;
      _scene_radius = std::max(std::sqrt(half.x * half.x + half.z * half.z), 0.25f);

      // Default zoom = "as close as possible without clipping or cropping": per-axis box fit (a
      // tall thin creature closes in until its HEIGHT fills the frame, instead of a conservative
      // bounding-sphere fit), plus the box's front depth, and never inside the near plane (1.0).
      float const fov_y = _camera.fov()._;
      float const aspect = std::max(aspect_ratio(), 0.2f);
      float const fov_x = 2.0f * std::atan(std::tan(fov_y * 0.5f) * aspect);
      float const dist_v = _scene_half_height / std::max(std::tan(fov_y * 0.5f), 0.05f);
      float const dist_h = _scene_radius / std::max(std::tan(fov_x * 0.5f), 0.05f);
      float fit = (std::max(dist_v, dist_h) + _scene_radius) * 1.03f;
      fit = std::max(fit, _scene_radius + 0.15f);
      if (!std::isfinite(fit))
      {
        fit = _scene_radius * 2.0f + 2.0f;
      }
      _fit_distance = fit;
      if (reset_zoom)
      {
        _orbit_distance = fit;
        _orbit_height_offset = 0.0f;
      }
      else
      {
        _orbit_distance = std::clamp(_orbit_distance, minZoom(), maxZoom());
        _orbit_height_offset = std::clamp(_orbit_height_offset,
                                          -2.0f * _scene_half_height, 2.0f * _scene_half_height);
      }
      applyOrbitCamera();
    }

    // Near plane is 0.05 in the preview projection -- allow zooming nearly to the surface.
    float minZoom() const { return std::max(0.12f, _scene_radius * 0.05f); }
    float maxZoom() const { return std::max(_fit_distance * 5.0f, minZoom() + 1.0f); }

    void mousePressEvent(QMouseEvent* event) override
    {
      if (event->button() == Qt::LeftButton)
      {
        _orbiting = true;
      }
      else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)
      {
        _panning = true; // right (or middle) drag = vertical pivot pan
      }
      _last_mouse_pos = event->pos();
      event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
      if (event->button() == Qt::LeftButton)
      {
        _orbiting = false;
      }
      else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)
      {
        _panning = false;
      }
      event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
      QLineF const relative_movement(_last_mouse_pos, event->pos());
      if (_orbiting)
      {
        // Grab-the-model sense: dragging right spins the model rightward (camera orbits left).
        _orbit_yaw -= static_cast<float>(relative_movement.dx()) * 0.4f;
        _orbit_pitch += static_cast<float>(relative_movement.dy()) * 0.4f;
        applyOrbitCamera();
      }
      else if (_panning)
      {
        // Vertical pivot pan ONLY (middle-drag): slide the whole pole up/down so a zoomed-in shot
        // can re-center on the head/torso -- the model tracks the cursor 1:1 at the pivot plane.
        float const world_per_pixel =
          2.0f * std::tan(_camera.fov()._ * 0.5f) * _orbit_distance / std::max(height(), 1);
        _orbit_height_offset += static_cast<float>(relative_movement.dy()) * world_per_pixel;
        _orbit_height_offset = std::clamp(_orbit_height_offset,
                                          -2.0f * _scene_half_height, 2.0f * _scene_half_height);
        applyOrbitCamera();
      }
      _last_mouse_pos = event->pos();
      event->accept();
    }

    void wheelEvent(QWheelEvent* event) override
    {
      float const step = event->angleDelta().y() > 0 ? 1.0f / 1.12f : 1.12f;
      _orbit_distance = std::clamp(_orbit_distance * step, minZoom(), maxZoom());
      applyOrbitCamera();
      event->accept();
    }

    // The pole replaces free-fly entirely: no WASD/arrow-key flying in the picker preview.
    void keyPressEvent(QKeyEvent* event) override { event->accept(); }
    void keyReleaseEvent(QKeyEvent* event) override { event->accept(); }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
      if (on_double_click)
      {
        on_double_click();
      }
      event->accept();
    }
  };

  // A QMenu that stays open when a CHECKABLE item is clicked, so the Seasonal Events dropdown can toggle
  // several events without reopening each time. Non-checkable items (All / None) and outside clicks close
  // it as usual.
  class MultiToggleMenu final : public QMenu
  {
  public:
    using QMenu::QMenu;

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
      QAction* const action = activeAction();
      if (action && action->isEnabled() && action->isCheckable())
      {
        action->trigger(); // toggle in place; keep the menu open
        return;
      }
      QMenu::mouseReleaseEvent(event);
    }
  };

  // A clean calendar QIcon rendered ONCE at high resolution (so Qt smooth-scales it down without the
  // aliasing/artifacts a per-size QIconEngine produced at 16px), tinted to the SAME palette color as the
  // noggit font-glyph toolbar icons (see FontNoggitIconEngine).
  QIcon make_calendar_icon()
  {
    Noggit::Ui::FontNoggitButtonStyle style;
    style.ensurePolished();
    QColor const color = style.palette().color(QPalette::WindowText);

    int const S = 128;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color);
    pen.setWidthF(S * 0.055);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Calendar body (rounded), leaving headroom at the top for the two binding tabs.
    QRectF const body(S * 0.15, S * 0.24, S * 0.70, S * 0.60);
    p.drawRoundedRect(body, S * 0.07, S * 0.07);

    // Two binding tabs crossing the top edge.
    qreal const tabX1 = body.left() + body.width() * 0.28;
    qreal const tabX2 = body.left() + body.width() * 0.72;
    qreal const tabTop = S * 0.15;
    qreal const tabBot = body.top() + body.height() * 0.10;
    p.drawLine(QPointF(tabX1, tabTop), QPointF(tabX1, tabBot));
    p.drawLine(QPointF(tabX2, tabTop), QPointF(tabX2, tabBot));

    // Header separator, then a filled header strip so it reads clearly as a calendar at small sizes.
    qreal const headerY = body.top() + body.height() * 0.30;
    p.drawLine(QPointF(body.left(), headerY), QPointF(body.right(), headerY));

    // A single centered "day" block in the lower area (one clean mark scales far better than a dot grid).
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QRectF const day(body.center().x() - body.width() * 0.16,
                     headerY + body.height() * 0.22,
                     body.width() * 0.32,
                     body.height() * 0.30);
    p.drawRoundedRect(day, S * 0.03, S * 0.03);

    p.end();
    return QIcon(pm);
  }
}


/* Some ugly macros we use */
// TODO: make those methods instead???

#define DESTRUCTIVE_ACTION(ACTION_CODE)                                                                                \
QMessageBox::StandardButton reply;                                                                                     \
reply = QMessageBox::question(this, "Destructive action", "This action cannot be undone. Current change history will be lost. Continue?", \
QMessageBox::Yes|QMessageBox::No);                                                                                     \
if (reply == QMessageBox::Yes)                                                                                         \
{                                                                                                                      \
NOGGIT_ACTION_MGR->purge();                                                                            \
ACTION_CODE                                                                                                            \
}                                                                                                                      \


#define ADD_ACTION_NS(menu, name, on_action)                      \
  {                                                               \
    auto action (menu->addAction (name));                         \
    connect (action, &QAction::triggered, on_action);             \
  }


#define ADD_TOGGLE(menu_, name_, shortcut_, property_)            \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_NS(menu_, name_, property_)                    \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_POST(menu_, name_, shortcut_, property_, post_)\
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
    connect ( action, &QAction::toggled, post_);                  \
    connect ( &property_, &Noggit::BoolToggleProperty::changed, \
    post_);                                                       \
  }                                                               \
  while (false)



#define ADD_TOGGLE_NS_POST(menu_, name_, property_, code_)        \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::bool_toggle_property::set      \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
      connect ( action, &QAction::toggled                         \
            ,  code_                                              \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , code_                                               \
            );                                                    \
  }                                                               \
  while (false)



#define ADD_ACTION(menu, name, shortcut, on_action)               \
  {                                                               \
    auto action (menu->addAction (name));                         \
    action->setShortcut (QKeySequence (shortcut));                \
    auto callback = on_action;                                    \
    connect (action, &QAction::triggered, [this, callback]()      \
    {                                                             \
       if (NOGGIT_CUR_ACTION) \
        return;                                                   \
       callback();                                                \
                                                                  \
    });                                                           \
  }


static const float XSENS = 15.0f;
static const float YSENS = 15.0f;

void MapView::set_editing_mode(editing_mode mode)
{
  // Selection markers (the disc under a spawn) only show while that spawn's tool is the active mode.
  _world->setDrawCreatureMarkers(mode == editing_mode::creature);
  _world->setDrawGameObjectMarkers(mode == editing_mode::gameobject);
  _world->setDrawGameObjectSpawns(mode == editing_mode::gameobject);

  {
    QSignalBlocker const asset_browser_blocker(_asset_browser_dock);
    QSignalBlocker const tex_browser_blocker(_texture_browser_dock);
    QSignalBlocker const texture_palette_blocker(_texture_palette_dock);
    QSignalBlocker const object_palette_blocker(_object_palette_dock);

    objectEditor->modelImport->hide();
    objectEditor->rotationEditor->hide();
    _texture_browser_dock->hide();
    _texture_picker_dock->hide();
    _texture_palette_dock->hide();
    _object_palette_dock->hide();
    _asset_browser_dock->hide();
    _viewport_overlay_ui->gizmoBar->hide();
  }

  auto previous_mode = _left_sec_toolbar->getCurrentMode();

  _left_sec_toolbar->setCurrentMode(this, mode);

  if (context() && context()->isValid())
  {
    if (mode == editing_mode::holes && previous_mode != editing_mode::holes)
    {
        _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = true;
        _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = true;
    }
    else if (previous_mode == editing_mode::holes && mode != editing_mode::holes)
    {
        _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = _draw_lines.get();
        _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = _draw_hole_lines.get();
    }

    _world->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_impass_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = false;
    _minimap->use_selection(nullptr);

    bool use_classic_ui = _settings->value("classicUI", true).toBool();

    switch (mode)
    {
      case editing_mode::ground:
        if (terrainTool->_edit_type != eTerrainType_Vertex || (terrainTool->_edit_type != eTerrainType_Script && terrainTool->getImageMaskSelector()->isEnabled()))
        {
          terrainTool->updateMaskImage();
        }
        break;
      case editing_mode::paint:
        if (texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && texturingTool->getImageMaskSelector()->isEnabled())
        {
          texturingTool->updateMaskImage();
        }

        if (use_classic_ui)
        {
            if (texturingTool->show_unpaintable_chunks())
            {
                _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }
        else
        {
            if (_left_sec_toolbar->showUnpaintableChunk())
            {
                _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = true;
            }
        }
        break;
      case editing_mode::mccv:
        if (shaderTool->getImageMaskSelector()->isEnabled())
        {
          shaderTool->updateMaskImage();
        }
        break;
      case editing_mode::stamp:
        if (stampTool->getActiveBrushItem() && stampTool->getActiveBrushItem()->isEnabled())
        {
          stampTool->getActiveBrushItem()->updateMask();
        }
        break;
      case editing_mode::areaid:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = true;
        break;
      case editing_mode::flags:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_impass_overlay = true;
        break;
      case editing_mode::minimap:
        _world->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = true;
        _minimap->use_selection(minimapTool->getSelectedTiles());
        break;
      case editing_mode::creature:
        _show_creature_browser.set(true);
        if (!_world->hasCreatureSpawnsLoaded())
        {
          _world->reloadCreatureSpawns();
        }
        _world->setDrawCreatureSpawns(true);
        rebuildCreatureBrowserList(true);
        updateDatabaseStatus();
        break;
      case editing_mode::gameobject:
        _show_gameobject_browser.set(true);
        _world->ensureGameObjectSpawnsLoaded();
        rebuildGameObjectBrowserList(true);
        updateGameObjectBrowserStatus();
        break;
      default:
        break;
    }
  }

  MoveObj = false;
  _world->reset_selection();
  _rotation_editor_need_update = true;

  if (!ui_hidden)
  {
    setToolPropertyWidgetVisibility(mode);
  }

  terrainMode = mode;
  _toolbar->check_tool (mode);
  this->activateWindow();

  _world->renderer()->markTerrainParamsUniformBlockDirty();
}

void MapView::setToolPropertyWidgetVisibility(editing_mode mode)
{
  bool const creature_mode = mode == editing_mode::creature;
  bool const gameobject_mode = mode == editing_mode::gameobject;
  bool const spawn_mode = creature_mode || gameobject_mode; // dock-based tools without a tool panel
  _main_window->setCorner(Qt::BottomRightCorner,
                          spawn_mode ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);

  if (_tool_panel_dock)
  {
    _tool_panel_dock->setVisible(!ui_hidden && !spawn_mode);
    if (!spawn_mode)
    {
      // gameobject (=15) has no tool-panel page; setCurrentIndex must not be called with it.
      _tool_panel_dock->setCurrentIndex(static_cast<int>(mode));
    }
  }

  auto set_creature_docks_visible = [](bool visible, std::initializer_list<QDockWidget*> docks)
  {
    for (auto* dock : docks)
    {
      if (dock)
      {
        dock->setVisible(visible);
      }
    }
  };

  if (mode != editing_mode::creature)
  {
    if (_creature_actions_overlay)
    {
      _creature_actions_overlay->setVisible(false);
    }
    set_creature_docks_visible(false, {_creature_browser_dock,
                                       _creature_editor_dock,
                                       _creature_model_picker_dock});
  }

  if (mode != editing_mode::gameobject)
  {
    if (_gameobject_actions_overlay)
    {
      _gameobject_actions_overlay->setVisible(false);
    }
    set_creature_docks_visible(false, {_gameobject_browser_dock,
                                       _gameobject_model_picker_dock});
  }

  switch (mode)
  {

  case editing_mode::object:
    _asset_browser_dock->setVisible(!ui_hidden && _settings->value("map_view/asset_browser", false).toBool());
    _object_palette_dock->setVisible(!ui_hidden && _settings->value("map_view/object_palette", false).toBool());
    _viewport_overlay_ui->gizmoBar->setVisible(!ui_hidden);
    break;
  case editing_mode::creature:
    if (_creature_actions_overlay)
    {
      _creature_actions_overlay->setVisible(!ui_hidden && _show_creature_browser.get());
    }
    set_creature_docks_visible(!ui_hidden && _show_creature_browser.get(),
                               {_creature_browser_dock,
                                _creature_editor_dock,
                                _creature_model_picker_dock});
    if (!ui_hidden && _show_creature_browser.get())
    {
      if (_creature_browser_dock && _creature_editor_dock)
      {
        _main_window->resizeDocks({_creature_browser_dock, _creature_editor_dock},
                                  {3, 1},
                                  Qt::Vertical);
      }
      if (_creature_browser_dock)
      {
        _main_window->resizeDocks({_creature_browser_dock}, {330}, Qt::Horizontal);
      }
      if (_creature_model_picker_dock)
      {
        _main_window->resizeDocks({_creature_model_picker_dock}, {260}, Qt::Vertical);
      }
    }
    break;
  case editing_mode::gameobject:
    if (_gameobject_actions_overlay)
    {
      _gameobject_actions_overlay->setVisible(!ui_hidden && _show_gameobject_browser.get());
    }
    set_creature_docks_visible(!ui_hidden && _show_gameobject_browser.get(),
                               {_gameobject_browser_dock,
                                _gameobject_model_picker_dock});
    if (!ui_hidden && _show_gameobject_browser.get())
    {
      if (_gameobject_browser_dock)
      {
        _main_window->resizeDocks({_gameobject_browser_dock}, {330}, Qt::Horizontal);
      }
      if (_gameobject_model_picker_dock)
      {
        _main_window->resizeDocks({_gameobject_model_picker_dock}, {260}, Qt::Vertical);
      }
    }
    break;
  case editing_mode::paint:
    _texture_browser_dock->setVisible(!ui_hidden && _settings->value("map_view/texture_browser", false).toBool());
    _texture_palette_dock->setVisible(!ui_hidden && _settings->value("map_view/texture_palette", false).toBool());
    break;
  default:
    break;
  }

  
}

void MapView::ResetSelectedObjectRotation()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      WMOInstance* wmo = static_cast<WMOInstance*>(obj);
      _world->updateTilesWMO(wmo, model_update::remove);
      wmo->resetDirection();
      _world->updateTilesWMO(wmo, model_update::add);
    }
    else if (obj->which() == eMODEL)
    {
      ModelInstance* m2 = static_cast<ModelInstance*>(obj);
      _world->updateTilesModel(m2, model_update::remove);
      m2->resetDirection();
      m2->recalcExtents();
      _world->updateTilesModel(m2, model_update::add);
    }
  }

  _rotation_editor_need_update = true;
}

void MapView::snap_selected_models_to_the_ground()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  _world->snap_selected_models_to_the_ground();
  _rotation_editor_need_update = true;
}


void MapView::DeleteSelectedObjects()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  _world->delete_selected_models();
  _rotation_editor_need_update = true;
}


void MapView::changeZoneIDValue (int set)
{
  _selected_area_id = set;
}


QWidgetAction* MapView::createTextSeparator(const QString& text)
{
  auto* pLabel = new QLabel(text);
  //pLabel->setMinimumWidth(this->minimumWidth() - 4);
  pLabel->setAlignment(Qt::AlignCenter);
  auto* separator = new QWidgetAction(this);
  separator->setDefaultWidget(pLabel);
  return separator;
}

void MapView::enterEvent(QEvent* event)
{
  // check if noggit is the currently active windows
  if (static_cast<QApplication*>(QApplication::instance())->applicationState() & Qt::ApplicationActive)
  {
    activateWindow();
  }
}

void MapView::setupViewportOverlay()
{
  _overlay_widget = new QWidget(this);
  _viewport_overlay_ui = new ::Ui::MapViewOverlay();
  _viewport_overlay_ui->setupUi(_overlay_widget);
  _overlay_widget->setAttribute(Qt::WA_TranslucentBackground);
  _overlay_widget->setMouseTracking(true);
  _overlay_widget->setGeometry(0,0, width(), height());

  _viewport_overlay_ui->gizmoVisibleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_VISIBILITY));
  _viewport_overlay_ui->gizmoModeButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_LOCAL));
  _viewport_overlay_ui->gizmoRotateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_ROTATE));
  _viewport_overlay_ui->gizmoScaleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_SCALE));
  _viewport_overlay_ui->gizmoTranslateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_TRANSLATE));

  connect(this, &MapView::resized
    ,[this]()
          {
            _overlay_widget->setGeometry(0, 0, width(), height());
          }
  );

  connect(_viewport_overlay_ui->gizmoVisibleButton, &QPushButton::clicked
    ,[this]()
          {
            _gizmo_on.set(_viewport_overlay_ui->gizmoVisibleButton->isChecked());
          }
  );

  connect(&_gizmo_on, &Noggit::BoolToggleProperty::changed
    ,[this](bool state)
          {
            _viewport_overlay_ui->gizmoVisibleButton->setChecked(state);
          }
  );

  connect(_viewport_overlay_ui->gizmoModeButton, &QPushButton::clicked, [this]()
  {
      if (_viewport_overlay_ui->gizmoModeButton->isChecked())
      {
          _gizmo_mode = ImGuizmo::MODE::WORLD;
      }
      else
      {
          _gizmo_mode = ImGuizmo::MODE::LOCAL;
      }
  });

  connect(_viewport_overlay_ui->gizmoTranslateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
    });

  connect(_viewport_overlay_ui->gizmoRotateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
    });

  connect(_viewport_overlay_ui->gizmoScaleButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
    });
}

void MapView::updateGizmoOverlay(ImGuizmo::OPERATION operation)
{
  if (operation == ImGuizmo::OPERATION::TRANSLATE)
  {
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoTranslateButton->isChecked())
      _viewport_overlay_ui->gizmoTranslateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::ROTATE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoRotateButton->isChecked())
      _viewport_overlay_ui->gizmoRotateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::SCALE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoScaleButton->isChecked())
      _viewport_overlay_ui->gizmoScaleButton->setChecked(true);
  }

  _gizmo_operation = operation;
}

void MapView::setupRaiseLowerUi()
{
  terrainTool = new Noggit::Ui::TerrainTool(this, this);
  _tool_panel_dock->registerTool("Raise | Lower", terrainTool);

  connect(terrainTool
    , &Noggit::Ui::TerrainTool::updateVertices
    , [this](int vertex_mode, math::degrees const& angle, math::degrees const& orientation)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            _world->orientVertices(vertex_mode == eVertexMode_Mouse
                                   ? _cursor_pos
                                   : _world->vertexCenter()
              , angle
              , orientation
            );
          }
  );

  terrainTool->storeCursorPos(&_cursor_pos);

}

void MapView::setupFlattenBlurUi()
{
  flattenTool = new Noggit::Ui::flatten_blur_tool(this);
  _tool_panel_dock->registerTool("Flatten | Blur", flattenTool);
}

void MapView::setupTexturePainterUi()
{
  /* Tool */
  texturingTool = new Noggit::Ui::texturing_tool(&_camera.position, this, &_show_texture_palette_small_window, this);
  _tool_panel_dock->registerTool("Texture Painter", texturingTool);

  // Connects
  connect( texturingTool->texture_swap_tool()->texture_display()
    , &Noggit::Ui::current_texture::texture_dropped
    , [=] (std::string const& filename)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());

             texturingTool->texture_swap_tool()->set_texture(filename);
           }
  );

  connect( texturingTool->_current_texture
    , &Noggit::Ui::current_texture::texture_dropped
    , [=] (std::string const& filename)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());

             Noggit::Ui::selected_texture::set({filename, _context});
           }
  );

  connect(texturingTool->_current_texture, &Noggit::Ui::current_texture::clicked
    , [=]
          {
            _texture_browser_dock->setVisible(!_texture_browser_dock->isVisible());
          }
  );

  /* Additional tools */

  /* Texture Browser */

  // Dock
  _texture_browser_dock = new QDockWidget("Texture Browser", this);
  _texture_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                     | QDockWidget::DockWidgetFloatable
                                     | QDockWidget::DockWidgetClosable);
  _texture_browser_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_browser_dock);
  _texture_browser_dock->hide();

  connect(_texture_browser_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/texture_browser", visible);
            _settings->sync();
          });

  connect(this, &QObject::destroyed, _texture_browser_dock, &QObject::deleteLater);
  // End Dock

  TexturePalette = new Noggit::Ui::tileset_chooser(this);
  _texture_browser_dock->setWidget(TexturePalette);
  connect(this, &QObject::destroyed, TexturePalette, &QObject::deleteLater);

  connect(TexturePalette, &Noggit::Ui::tileset_chooser::selected
    , [=](std::string const& filename)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            Noggit::Ui::selected_texture::set({filename, _context});
            texturingTool->_current_texture->set_texture(filename);
            TexturePicker->setMainTexture(texturingTool->_current_texture);
            TexturePicker->updateSelection();
          }
  );

  connect ( TexturePalette, &Noggit::Ui::widget::visibilityChanged
    , &_show_texture_palette_window, &Noggit::BoolToggleProperty::set
  );

  connect ( &_show_texture_palette_window, &Noggit::BoolToggleProperty::changed
    ,  [this]
            {
              if ((terrainMode == editing_mode::paint || terrainMode == editing_mode::stamp)  && !ui_hidden)
              {
                _texture_browser_dock->setVisible(_show_texture_palette_window.get());
              }
              else
              {
                QSignalBlocker const _ (_show_texture_palette_window);
                _show_texture_palette_window.set(false);
              }
            }
  );


  /* Texture Palette Small */
  _texture_palette_small = new Noggit::Ui::texture_palette_small(_project, _world->getMapID(), this);

  // Dock
  _texture_palette_dock = new QDockWidget("Texture Palette", this);
  _texture_palette_dock->setFeatures(QDockWidget::DockWidgetMovable
                                     | QDockWidget::DockWidgetFloatable
                                     | QDockWidget::DockWidgetClosable
  );

  _texture_palette_dock->setWidget(_texture_palette_small);
  _texture_palette_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);;

  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_palette_dock);
  // End Dock

  connect(_texture_palette_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/texture_palette", visible);
            _settings->sync();
          });

  connect(_texture_palette_small, &Noggit::Ui::texture_palette_small::selected
    , [=](std::string const& filename)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            Noggit::Ui::selected_texture::set({filename, _context});
            texturingTool->_current_texture->set_texture(filename);
          }
  );
  connect(this, &QObject::destroyed, _texture_palette_small, &QObject::deleteLater);

  connect(&_show_texture_palette_small_window, &Noggit::BoolToggleProperty::changed
    , _texture_palette_dock, [this]
          {
            QSignalBlocker const blocker(_show_texture_palette_small_window);
            if (terrainMode == editing_mode::paint && !ui_hidden)
            {
              _texture_palette_dock->setVisible(_show_texture_palette_small_window.get());
            }
            else
            {
              _show_texture_palette_small_window.set(false);
            }
          }
  );
  connect(_texture_palette_dock, &QDockWidget::visibilityChanged
    , &_show_texture_palette_small_window, &Noggit::BoolToggleProperty::set
  );

  connect(texturingTool->_current_texture, &Noggit::Ui::current_texture::texture_updated
          , [=]()
      {
       _world->notifyTileRendererOnSelectedTextureChange();
      }
  );

  /* Texture Picker */

  // Dock
  _texture_picker_dock = new QDockWidget("Texture picker", this);
  _texture_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _texture_picker_dock);
  _texture_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _texture_picker_dock->setFloating(true);
  _texture_picker_dock->hide();
  connect(this, &QObject::destroyed, _texture_picker_dock, &QObject::deleteLater);
  // End Dock

  TexturePicker = new Noggit::Ui::texture_picker(texturingTool->_current_texture, this);
  _texture_picker_dock->setWidget(TexturePicker);
  connect(this, &QObject::destroyed, TexturePicker, &QObject::deleteLater);

  connect( TexturePicker
    , &Noggit::Ui::texture_picker::set_texture
    , [=] (scoped_blp_texture_reference texture)
           {
             makeCurrent();
             OpenGL::context::scoped_setter const _(::gl, context());
             Noggit::Ui::selected_texture::set(std::move(texture));
           }
  );
  connect(TexturePicker, &Noggit::Ui::texture_picker::shift_left
    , [=]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            TexturePicker->shiftSelectedTextureLeft();
          }
  );
  connect(TexturePicker, &Noggit::Ui::texture_picker::shift_right
    , [=]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            TexturePicker->shiftSelectedTextureRight();
          }
  );

}

void MapView::setupHoleCutterUi()
{
  holeTool = new Noggit::Ui::hole_tool(this);
  _tool_panel_dock->registerTool("Hole Cutter", holeTool);
}

void MapView::setupAreaDesignatorUi()
{
  ZoneIDBrowser = new Noggit::Ui::zone_id_browser(this);
  _tool_panel_dock->registerTool("Area Designator", ZoneIDBrowser);

  ZoneIDBrowser->setMapID(_world->getMapID());
  connect(ZoneIDBrowser, &Noggit::Ui::zone_id_browser::selected
    , [this](int area_id) { changeZoneIDValue(area_id); }
  );
}

void MapView::setupFlagUi()
{
  auto placeholder = new QWidget(this);
  _tool_panel_dock->registerTool("Flag", placeholder);
}

void MapView::setupWaterEditorUi()
{
  guiWater = new Noggit::Ui::water(&_displayed_water_layer, &_display_all_water_layers, this);
  _tool_panel_dock->registerTool("Water Editor", guiWater);

  connect(guiWater, &Noggit::Ui::water::regenerate_water_opacity
    , [this](float factor)
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER);
            _world->autoGenWaterTrans(_camera.position, factor);
            NOGGIT_ACTION_MGR->endAction();
          }
  );

  connect(guiWater, &Noggit::Ui::water::crop_water
    , [this]
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER);
            _world->CropWaterADT(_camera.position);
            NOGGIT_ACTION_MGR->endAction();
          }
  );
}
void MapView::setupVertexPainterUi()
{
  shaderTool = new Noggit::Ui::ShaderTool(this, this);
  _tool_panel_dock->registerTool("Vertex Painter", shaderTool);
}

void MapView::setupScriptingUi()
{
  scriptingTool = new Noggit::Scripting::scripting_tool(this, this, _settings);
  _tool_panel_dock->registerTool("Scripting", scriptingTool);
}

void MapView::setupObjectEditorUi()
{
  /* Tool */
  objectEditor = new Noggit::Ui::object_editor(this
    , _world.get()
    , &_move_model_to_cursor_position
    , &_snap_multi_selection_to_ground
    , &_use_median_pivot_point
    , &_object_paste_params
    , &_rotate_along_ground
    , &_rotate_along_ground_smooth
    , &_rotate_along_ground_random
    , &_move_model_snap_to_objects
    , this
  );
  _tool_panel_dock->registerTool("Object Editor", objectEditor);

  /* Additional tools */

  /* Area selection */
  _area_selection = new QRubberBand(QRubberBand::Rectangle, this);

  /* Object Palette */
  _object_palette = new Noggit::Ui::ObjectPalette(this, _project, this);
  _object_palette->hide();

  // Dock
  _object_palette_dock = new QDockWidget("Object Palette", this);
  _object_palette_dock->setFeatures(QDockWidget::DockWidgetMovable
                                    | QDockWidget::DockWidgetFloatable
                                    | QDockWidget::DockWidgetClosable
  );

  _object_palette_dock->setWidget(_object_palette);
  _object_palette_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _object_palette_dock);
  connect(this, &QObject::destroyed, _texture_palette_dock, &QObject::deleteLater);
  // End Dock

  connect(_object_palette_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/object_palette", visible);
            _settings->sync();
          });

}

void MapView::setupCreatureEditorUi()
{
  // The coordinate editor lives as a column inside the NPC Model Picker splitter (built later in
  // setupCreatureModelPickerUi); we just build the panel widget here and hand it off via the member.
  auto container = new QWidget(this);
  container->setMinimumWidth(240);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  auto coord_title = new QLabel("Creature Coordinates", container);
  coord_title->setStyleSheet("font-weight: bold;");
  layout->addWidget(coord_title);

  auto hint = new QLabel("Left click: select or drag\nShift: add or box-select", container);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  _creature_editor_info = new QLabel("No spawn selected", container);
  _creature_editor_info->setWordWrap(true);
  _creature_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
  layout->addWidget(_creature_editor_info);

  auto make_spin = [&](double lo, double hi, double step) {
    auto* sb = new QDoubleSpinBox(container);
    sb->setRange(lo, hi);
    sb->setDecimals(3);
    sb->setSingleStep(step);
    sb->setEnabled(false);
    return sb;
  };

  // Noggit client coordinates span 0..2*ZEROPOINT (~34133) on normal maps and +/-ZEROPOINT on global
  // WMO maps, so the X/Z range must be wide enough not to clamp distant spawns.
  _spawn_edit_x           = make_spin(-64000.0, 64000.0, 0.1);
  _spawn_edit_y           = make_spin(-20000.0, 20000.0, 0.1);
  _spawn_edit_z           = make_spin(-64000.0, 64000.0, 0.1);
  _spawn_edit_orientation = make_spin(     0.0,   360.0, 1.0);
  _spawn_edit_orientation->setWrapping(true);
  _spawn_edit_orientation->setSuffix(QString::fromUtf8("\xc2\xb0"));  // ┬░

  auto form = new QFormLayout();
  form->setContentsMargins(0, 4, 0, 0);
  form->setSpacing(3);
  form->addRow("X:", _spawn_edit_x);
  form->addRow("Y (height):", _spawn_edit_y);
  form->addRow("Z:", _spawn_edit_z);
  form->addRow("Orientation:", _spawn_edit_orientation);
  layout->addLayout(form);

  auto on_change = [this](double) {
    if (!_selected_creature_spawn_guid)
      return;
    auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
    if (!spawn)
      return;

    glm::vec3 const new_pos(
      static_cast<float>(_spawn_edit_x->value()),
      static_cast<float>(_spawn_edit_y->value()),
      static_cast<float>(_spawn_edit_z->value()));
    float const new_orientation = static_cast<float>(_spawn_edit_orientation->value());
    if (glm::distance(new_pos, spawn->pos) > 0.0001f
        || std::abs(new_orientation - spawn->orientation) > 0.0001f)
    {
      // Ctrl+Z op for the coordinate-panel edit; bursts of steps coalesce into one op.
      SpawnUndoOp op;
      op.kind = SpawnUndoOp::Kind::Move;
      op.from_spinbox = true;
      op.moves.push_back({spawn->guid, spawn->pos, spawn->orientation});
      pushCreatureUndoOp(std::move(op));
    }
    spawn->pos = new_pos;
    spawn->orientation = new_orientation;
    spawn->dirty = spawn->pending_create
                || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f
                || spawn->extDirty();

    if (spawn->model_instance)
    {
      spawn->model_instance->pos = spawn->pos;
      spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
      spawn->model_instance->recalcExtents();
    }

    updateDatabaseStatus();
    refreshCreatureBrowserItems({spawn->guid}); // full rebuild here lagged every spinbox step
    _needs_redraw = true;
  };

  connect(_spawn_edit_x,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_y,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_z,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_spawn_edit_orientation, qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);

  _creature_editor_panel = container;
}

void MapView::setupCreatureBrowserUi()
{
  // [game mode 2026-08-10] speed panel: slider + client-speed presets (walk 2.5 / run 7 / mounted 14),
  // visible only while Game View is active. Presets drive the slider; the slider allows any value.
  {
    _game_mode_speed_dock = new QDockWidget("Game Mode", _main_window);
    _game_mode_speed_dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    _game_mode_speed_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto speed_container = new QWidget(_game_mode_speed_dock);
    auto speed_layout = new QVBoxLayout(speed_container);
    speed_layout->setContentsMargins(6, 6, 6, 6);

    auto speed_label = new QLabel("Speed: 7.0 yd/s", speed_container);
    speed_layout->addWidget(speed_label);

    auto speed_slider = new QSlider(Qt::Horizontal, speed_container);
    speed_slider->setRange(5, 400); // 0.5 .. 40.0 yd/s, in tenths
    speed_slider->setValue(70);
    speed_layout->addWidget(speed_slider);

    auto walk_radio = new QRadioButton("Walking (2.5 yd/s)", speed_container);
    auto run_radio = new QRadioButton("Running (7 yd/s)", speed_container);
    auto mount_radio = new QRadioButton("Mounted (14 yd/s)", speed_container);
    run_radio->setChecked(true);
    speed_layout->addWidget(walk_radio);
    speed_layout->addWidget(run_radio);
    speed_layout->addWidget(mount_radio);

    // 3rd-person character (mouse wheel zooms out to see it): rendered as this creature display id
    auto char_label = new QLabel("Character display id:", speed_container);
    speed_layout->addWidget(char_label);
    auto char_display = new DisplayIdSpinBox(speed_container);
    char_display->setRange(1, 1000000);
    // remembered PER PROJECT: QSettings key scoped by the project path, loaded here and saved on
    // every edit, so reopening the project brings back the last character model used
    QString char_display_key("gameMode/characterDisplayId");
    if (auto const* proj = Noggit::Project::CurrentProject::get())
    {
      QString suffix = QString::fromStdString(proj->ProjectPath);
      suffix.replace('\\', '_').replace('/', '_').replace(':', '_');
      char_display_key += "/" + suffix;
    }
    _game_character_display_id =
      std::clamp(_settings->value(char_display_key, _game_character_display_id).toInt(), 1, 1000000);
    char_display->setValue(_game_character_display_id);
    char_display->row_ids = [this]() -> std::vector<std::uint32_t> const*
    {
      return _world ? &_world->creatureDisplayIds() : nullptr;
    };
    char_display->setToolTip("CreatureDisplayInfo id the 3rd-person character renders as\n(zoom out with the mouse wheel in Game View to see it)");
    speed_layout->addWidget(char_display);
    speed_layout->addStretch();

    speed_container->setLayout(speed_layout);
    _game_mode_speed_dock->setWidget(speed_container);
    _main_window->addDockWidget(Qt::RightDockWidgetArea, _game_mode_speed_dock);
    _game_mode_speed_dock->hide();
    connect(this, &QObject::destroyed, _game_mode_speed_dock, &QObject::deleteLater);

    connect(speed_slider, &QSlider::valueChanged, [this, speed_label](int v)
    {
      _game_mode_speed = v / 10.0f;
      speed_label->setText(QString("Speed: %1 yd/s").arg(_game_mode_speed, 0, 'f', 1));
    });
    auto const apply_preset = [speed_slider](float v)
    {
      speed_slider->setValue(static_cast<int>(std::lround(v * 10.0f))); // slider signal updates the rest
    };
    connect(walk_radio, &QRadioButton::clicked, [apply_preset] { apply_preset(2.5f); });
    connect(run_radio, &QRadioButton::clicked, [apply_preset] { apply_preset(7.0f); });
    connect(mount_radio, &QRadioButton::clicked, [apply_preset] { apply_preset(14.0f); });
    connect(char_display, qOverload<int>(&QSpinBox::valueChanged), [this, char_display_key](int v)
    {
      _game_character_display_id = v; // picked up next game tick (setGameCharacterDisplayId no-ops when unchanged)
      _settings->setValue(char_display_key, v); // per-project memory
    });

    connect(&_game_mode_camera, &Noggit::BoolToggleProperty::changed,
            _game_mode_speed_dock, [this](bool state) { _game_mode_speed_dock->setVisible(state); });
  }

  _creature_browser_dock = new QDockWidget("Creature Browser", _main_window);
  _creature_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                      | QDockWidget::DockWidgetFloatable
                                      | QDockWidget::DockWidgetClosable);
  _creature_browser_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
  _creature_browser_dock->setMinimumWidth(330);
  _creature_browser_dock->resize(380, 520);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _creature_browser_dock);
  connect(this, &QObject::destroyed, _creature_browser_dock, &QObject::deleteLater);

  auto container = new QWidget(this);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  _creature_search_field = new QLineEdit(container);
  _creature_search_field->setPlaceholderText("Search by creature name, guid, or entry");
  layout->addWidget(_creature_search_field);

  _creature_zone_filter = new QCheckBox("Zone only (current zone)", container);
  _creature_zone_filter->setToolTip("Only list creatures whose position is in the same zone as the camera"
                                    " (e.g. Searing Gorge). Spawns in unloaded tiles are excluded.");
  layout->addWidget(_creature_zone_filter);
  connect(_creature_zone_filter, &QCheckBox::toggled, [this]() { rebuildCreatureBrowserList(true); });

  // Type/rank filter, mirroring the NPC model picker. Filters the current-map spawn list by the spawned
  // creature's template type/rank (looked up per spawn entry from _creature_template_filter_info, which
  // the model picker fills when it loads creature_template). The Type combo is populated there too.
  {
    auto type_row = new QHBoxLayout();
    type_row->addWidget(new QLabel("Type", container));
    _creature_browser_type_filter = new QComboBox(container);
    _creature_browser_type_filter->addItem("All types"); // index 0 -> no type filter
    _creature_browser_type_filter->setToolTip("Filter the spawn list by creature type.");
    type_row->addWidget(_creature_browser_type_filter, 1);
    layout->addLayout(type_row);

    auto flag_row = new QHBoxLayout();
    _creature_browser_elite = new QCheckBox("Elite", container);
    _creature_browser_boss = new QCheckBox("Boss", container);
    _creature_browser_civilian = new QCheckBox("Civilian", container);
    _creature_browser_trainer = new QCheckBox("Trainer", container);
    _creature_browser_elite->setToolTip("Show rank 1 and 2 spawns.");
    _creature_browser_boss->setToolTip("Show rank 3 and higher spawns.");
    flag_row->addWidget(_creature_browser_elite);
    flag_row->addWidget(_creature_browser_boss);
    flag_row->addWidget(_creature_browser_civilian);
    flag_row->addWidget(_creature_browser_trainer);
    flag_row->addStretch(1);
    layout->addLayout(flag_row);

    connect(_creature_browser_type_filter, qOverload<int>(&QComboBox::currentIndexChanged),
            [this]() { rebuildCreatureBrowserList(true); });
    for (auto* cb : {_creature_browser_elite, _creature_browser_boss,
                     _creature_browser_civilian, _creature_browser_trainer})
    {
      connect(cb, &QCheckBox::toggled, [this]() { rebuildCreatureBrowserList(true); });
    }
  }

  _creature_list_widget = new QListWidget(container);
  _creature_list_widget->setSelectionMode(QAbstractItemView::SingleSelection);
  _creature_list_widget->setMinimumHeight(360);
  layout->addWidget(_creature_list_widget, 1);

  _creature_browser_status = new QLabel(container);
  _creature_browser_status->setWordWrap(true);
  layout->addWidget(_creature_browser_status);

  _creature_browser_dock->setWidget(container);
  _creature_browser_dock->setVisible(false);

  connect(_creature_browser_dock, &QDockWidget::visibilityChanged,
          [this](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue("map_view/creature_browser", visible);
            _settings->sync();
          });

  connect(&_show_creature_browser, &Noggit::BoolToggleProperty::changed,
          [this](bool visible)
          {
            bool const show = visible && !ui_hidden && terrainMode == editing_mode::creature;
            if (ui_hidden && visible)
            {
              return;
            }

            if (_creature_actions_overlay)
            {
              _creature_actions_overlay->setVisible(show);
            }

            for (auto* dock : {_creature_browser_dock,
                               _creature_editor_dock,
                               _creature_model_picker_dock})
            {
              if (dock)
              {
                dock->setVisible(show);
              }
            }
          });
  connect(_creature_browser_dock, &QDockWidget::visibilityChanged,
          &_show_creature_browser, &Noggit::BoolToggleProperty::set);

  connect(_creature_search_field, &QLineEdit::textChanged,
          [this]()
          {
            rebuildCreatureBrowserList(true);
          });
  connect(_creature_list_widget, &QListWidget::itemClicked,
          this, &MapView::jumpToCreatureListItem);
  // Arrow keys confirm as the highlight moves: navigating the spawn list selects + jumps to each
  // spawn immediately. Focus-gated so rebuilds/programmatic highlighting never re-trigger it.
  connect(_creature_list_widget, &QListWidget::currentItemChanged,
          [this](QListWidgetItem* current, QListWidgetItem*)
          {
            if (current && _creature_list_widget->hasFocus())
            {
              jumpToCreatureListItem(current);
            }
          });

  updateCreatureBrowserStatus();
}

void MapView::setupCreatureActionsUi()
{
  if (!_overlay_widget)
  {
    return;
  }

  _creature_actions_overlay = new QWidget(_overlay_widget);
  _creature_actions_overlay->setObjectName("creatureActionsOverlay");
  _creature_actions_overlay->setAttribute(Qt::WA_StyledBackground, true);
  _creature_actions_overlay->setStyleSheet(
    "#creatureActionsOverlay { background: rgba(28, 31, 37, 210); border: 1px solid rgba(85, 91, 103, 180); }"
    "#creatureActionsOverlay QPushButton { padding: 5px 9px; }");

  auto layout = new QHBoxLayout(_creature_actions_overlay);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);

  auto reload_button = new QPushButton("Reload Spawns", _creature_actions_overlay);
  auto save_button = new QPushButton("Export SQL", _creature_actions_overlay);
  auto revert_button = new QPushButton("Discard Pending", _creature_actions_overlay);
  auto pending_button = new QPushButton("Pending \xE2\x96\xBE", _creature_actions_overlay);
  pending_button->setToolTip("Show the list of pending creature updates waiting for SQL export.");
  layout->addWidget(reload_button);
  layout->addWidget(save_button);
  layout->addWidget(revert_button);
  layout->addWidget(pending_button);

  // Toggleable dropdown listing every pending change (new / moved / deleted) awaiting SQL export.
  _creature_pending_popup = new QWidget(this, Qt::Popup);
  _creature_pending_popup->setObjectName("creaturePendingPopup");
  _creature_pending_popup->setAttribute(Qt::WA_StyledBackground, true);
  _creature_pending_popup->setStyleSheet(
    "#creaturePendingPopup { background: rgba(28, 31, 37, 235); border: 1px solid rgba(85, 91, 103, 200); }");
  auto pending_layout = new QVBoxLayout(_creature_pending_popup);
  pending_layout->setContentsMargins(6, 6, 6, 6);
  pending_layout->setSpacing(4);
  auto pending_title = new QLabel("Pending creature updates", _creature_pending_popup);
  pending_title->setStyleSheet("font-weight: bold; color: #ddd;");
  pending_layout->addWidget(pending_title);
  _creature_pending_list = new QListWidget(_creature_pending_popup);
  _creature_pending_list->setMinimumSize(360, 220);
  _creature_pending_list->setSelectionMode(QListWidget::NoSelection);
  pending_layout->addWidget(_creature_pending_list);

  auto refresh_pending = [this]()
  {
    if (!_creature_pending_list)
    {
      return;
    }
    _creature_pending_list->clear();
    int count = 0;
    for (auto const& spawn : _world->creatureSpawns())
    {
      if (!spawn.dirty)
      {
        continue;
      }
      // A spawn created and deleted this session never hit the DB -> nothing to export.
      if (spawn.pending_delete && spawn.pending_create)
      {
        continue;
      }
      QString action;
      if (spawn.pending_delete)
      {
        action = "DELETE";
      }
      else if (spawn.pending_create)
      {
        action = "NEW";
      }
      else
      {
        action = "MOVE";
      }
      QString const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
      _creature_pending_list->addItem(
        QString("[%1] guid %2  entry %3  %4").arg(action).arg(spawn.guid).arg(spawn.entry).arg(name));
      ++count;
    }
    if (count == 0)
    {
      _creature_pending_list->addItem("No pending updates.");
    }
  };

  connect(pending_button, &QPushButton::clicked,
          [this, pending_button, refresh_pending]()
          {
            if (!_creature_pending_popup)
            {
              return;
            }
            if (_creature_pending_popup->isVisible())
            {
              _creature_pending_popup->hide();
              return;
            }
            refresh_pending();
            _creature_pending_popup->adjustSize();
            QPoint const below = pending_button->mapToGlobal(QPoint(0, pending_button->height() + 2));
            _creature_pending_popup->move(below);
            _creature_pending_popup->show();
          });

  auto place_actions = [this]()
  {
    if (!_creature_actions_overlay)
    {
      return;
    }

    _creature_actions_overlay->adjustSize();
    _creature_actions_overlay->move(std::max(8, width() - _creature_actions_overlay->width() - 14), 8);
    _creature_actions_overlay->raise();
  };

  _creature_actions_overlay->setVisible(false);
  place_actions();
  connect(this, &MapView::resized, this, place_actions);
  connect(this, &QObject::destroyed, _creature_actions_overlay, &QObject::deleteLater);

  connect(reload_button, &QPushButton::clicked,
          [this]()
          {
            refreshCreatureSpawnOverlay(true);
          });
  connect(save_button, &QPushButton::clicked,
          [this]()
          {
            saveDirtyCreatureSpawns();
          });
  connect(revert_button, &QPushButton::clicked,
          [this]()
          {
            discardPendingCreatureSpawns();
          });
}

void MapView::setupCreatureModelPickerUi()
{
  _creature_model_picker_dock = new QDockWidget("NPC Model Picker", _main_window);
  _creature_model_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                           | QDockWidget::DockWidgetFloatable
                                           | QDockWidget::DockWidgetClosable);
  _creature_model_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _creature_model_picker_dock->setMinimumHeight(220);

  auto container = new QWidget(this);
  auto root_layout = new QVBoxLayout(container);
  root_layout->setContentsMargins(6, 6, 6, 6);
  root_layout->setSpacing(6);

  // Filters now live in a small vertical panel beside the list (added to the splitter below).
  auto filter_panel = new QWidget(container);
  filter_panel->setMaximumWidth(150);
  auto filter_layout = new QVBoxLayout(filter_panel);
  filter_layout->setContentsMargins(4, 4, 4, 4);
  filter_layout->setSpacing(4);
  auto filter_title = new QLabel("Filters", filter_panel);
  filter_title->setStyleSheet("font-weight: bold;");
  filter_layout->addWidget(filter_title);
  auto type_label = new QLabel("Type", filter_panel);
  auto type_filter = new QComboBox(filter_panel);
  type_filter->setToolTip("Filter entries by creature type. Populated from the loaded creature_template list.");
  type_filter->addItem("All types");  // index 0: no data -> no type filter
  auto elite_only = new QCheckBox("Elite", filter_panel);
  auto boss_only = new QCheckBox("Boss", filter_panel);
  auto civilian_only = new QCheckBox("Civilian", filter_panel);
  auto trainer_only = new QCheckBox("Trainer", filter_panel);
  elite_only->setToolTip("Show rank 1 and 2 entries.");
  boss_only->setToolTip("Show rank 3 and higher entries.");
  civilian_only->setToolTip("Show entries with the civilian type flag.");
  trainer_only->setToolTip("Show entries with trainer NPC flags.");
  filter_layout->addWidget(type_label);
  filter_layout->addWidget(type_filter);
  filter_layout->addWidget(elite_only);
  filter_layout->addWidget(boss_only);
  filter_layout->addWidget(civilian_only);
  filter_layout->addWidget(trainer_only);
  filter_layout->addStretch();

  auto splitter = new QSplitter(Qt::Horizontal, container);

  // Search box now sits directly above the model tree (list) column at the list width.
  auto list_column = new QWidget(splitter);
  auto list_column_layout = new QVBoxLayout(list_column);
  list_column_layout->setContentsMargins(0, 0, 0, 0);
  list_column_layout->setSpacing(4);

  auto search_box = new QLineEdit(list_column);
  search_box->setPlaceholderText("Search by entry id or name...");
  search_box->setClearButtonEnabled(true);
  search_box->setToolTip("Filter the list by creature_template entry id or name (case-insensitive).");
  list_column_layout->addWidget(search_box);

  _creature_model_tree = new QTreeWidget(list_column);
  _creature_model_tree->setHeaderHidden(true);
  _creature_model_tree->setMinimumWidth(360);
  list_column_layout->addWidget(_creature_model_tree, 1);

  // Create / edit NPC (creature_template) -- see NpcEditorDialog.
  auto* npc_buttons = new QHBoxLayout();
  npc_buttons->setSpacing(4);
  auto* new_npc_button = new QPushButton("New NPC from selected...", list_column);
  new_npc_button->setToolTip("Create a new NPC that starts as a copy of the selected one, then change its\n"
                             "name, looks, faction and jobs. It is written to the database right away.");
  auto* edit_npc_button = new QPushButton("Edit NPC...", list_column);
  new_npc_button->setEnabled(false);
  edit_npc_button->setEnabled(false);
  auto* delete_npc_button = new QPushButton("Delete NPC...", list_column);
  delete_npc_button->setEnabled(false);
  auto* quests_button = new QPushButton("Quests...", list_column);
  quests_button->setToolTip("Browse, create and edit quests. With an NPC selected, shows that NPC's quests.");
  quests_button->setEnabled(false);
  npc_buttons->addWidget(new_npc_button);
  npc_buttons->addWidget(edit_npc_button);
  npc_buttons->addWidget(delete_npc_button);
  npc_buttons->addWidget(quests_button);
  list_column_layout->addLayout(npc_buttons);

  auto preview = new CreaturePreviewModelViewer(splitter);
  preview->setMinimumSize(360, 220);

  auto spawn_box = new QGroupBox("Edit/New Creature", splitter);
  _creature_spawn_box = spawn_box;
  // Two form columns: identity on the left, the extended `creature` behaviour columns on the right
  // (full tortoise-wow spawn schema; each field's meaning RE'd from the live server source --
  // Object.h SPAWN_FLAG_*, MotionMaster.h motion types, RandomMovementGenerator, Object.cpp
  // visibility checks). Fields whose column the connected DB lacks are disabled.
  auto spawn_columns = new QHBoxLayout(spawn_box);
  spawn_columns->setContentsMargins(6, 4, 6, 4);
  spawn_columns->setSpacing(12);
  auto spawn_layout = new QFormLayout();
  spawn_layout->setContentsMargins(0, 0, 0, 0);
  spawn_layout->setSpacing(3);
  auto spawn_layout_ext = new QFormLayout();
  spawn_layout_ext->setContentsMargins(0, 0, 0, 0);
  spawn_layout_ext->setSpacing(3);
  spawn_columns->addLayout(spawn_layout, 1);
  spawn_columns->addLayout(spawn_layout_ext, 1);
  _creature_spawn_form_left = spawn_layout;
  _creature_spawn_form_ext = spawn_layout_ext;

  auto guid_field = new QLineEdit(spawn_box);
  auto entry_field = new QLineEdit(spawn_box);
  auto display_field = new QLineEdit(spawn_box);
  _creature_spawn_guid_field = guid_field;
  _creature_spawn_entry_field = entry_field;
  _creature_spawn_display_field = display_field;
  auto add_button = new QPushButton("Add Pending Spawn", spawn_box);
  add_button->setEnabled(false);
  guid_field->setPlaceholderText("GUID");
  entry_field->setPlaceholderText("Entry");
  display_field->setPlaceholderText("Display ID");
  spawn_layout->addRow("GUID:", guid_field);
  spawn_layout->addRow("Entry:", entry_field);
  spawn_layout->addRow("Display:", display_field);

  // id2/id3/id4: alternate creature_template entries -- the server picks one NON-ZERO id at random
  // on every (re)spawn (Creature.h:309 creature_id[urand(0, count-1)]). 0 = slot unused.
  {
    _creature_spawn_alt_row = new QWidget(spawn_box);
    auto alt_row = new QHBoxLayout(_creature_spawn_alt_row);
    alt_row->setContentsMargins(0, 0, 0, 0);
    alt_row->setSpacing(3);
    _creature_spawn_id2_field = new QLineEdit(_creature_spawn_alt_row);
    _creature_spawn_id3_field = new QLineEdit(_creature_spawn_alt_row);
    _creature_spawn_id4_field = new QLineEdit(_creature_spawn_alt_row);
    for (auto* f : {_creature_spawn_id2_field, _creature_spawn_id3_field, _creature_spawn_id4_field})
    {
      f->setPlaceholderText("0");
      f->setToolTip("Alternate creature_template entry (id2/id3/id4).\n"
                    "The server picks ONE of the non-zero ids at random on every (re)spawn\n"
                    "(random-variant spawns). 0 = slot unused.\n"
                    "Turtle schema: the creature.id2/id3/id4 columns. cmangos schema: exported as\n"
                    "creature_spawn_entry rows with creature.id = 0 (same random-pick behaviour).");
      alt_row->addWidget(f);
    }
    spawn_layout->addRow("Alt ids:", _creature_spawn_alt_row);
  }

  // cmangos-only columns (mangos-wotlk ObjectMgr::LoadCreatures): hidden on Turtle-schema DBs.
  {
    _creature_spawn_spawnmask_button = new QPushButton("1 (Normal)", spawn_box);
    _creature_spawn_spawnmask_menu = new QMenu(spawn_box);
    struct MaskDef { std::uint32_t bit; char const* label; };
    static constexpr MaskDef spawn_mask_defs[] = {
      {0x1, "0x1 Normal / 10-man normal (open world uses just this)"},
      {0x2, "0x2 Heroic dungeon / 25-man normal"},
      {0x4, "0x4 10-man heroic raid"},
      {0x8, "0x8 25-man heroic raid"},
    };
    for (auto const& def : spawn_mask_defs)
    {
      auto* act = _creature_spawn_spawnmask_menu->addAction(QString::fromLatin1(def.label));
      act->setCheckable(true);
      act->setData(def.bit);
    }
    _creature_spawn_spawnmask_button->setMenu(_creature_spawn_spawnmask_menu);
    _creature_spawn_spawnmask_button->setToolTip("spawnMask -- which map DIFFICULTIES this spawn exists in\n"
                                                 "(bit k = Difficulty k; the server rejects bits the map does not\n"
                                                 "support). Open-world spawns use 1. 0 would spawn in nothing.");
    spawn_layout->addRow("Spawn mask:", _creature_spawn_spawnmask_button);

    _creature_spawn_phasemask = new QSpinBox(spawn_box);
    _creature_spawn_phasemask->setRange(1, 0xFFFF);
    _creature_spawn_phasemask->setValue(1);
    _creature_spawn_phasemask->setToolTip("phaseMask -- WotLK phasing bitmask: the spawn is visible to players\n"
                                          "whose phase shares a bit with it. 1 = the normal world phase\n"
                                          "(the server coerces 0 to 1: 0 would be visible to no one).");
    spawn_layout->addRow("Phase:", _creature_spawn_phasemask);
  }
  spawn_layout->addRow(add_button);

  // --- extended behaviour column ---
  {
    _creature_spawn_respawn_row = new QWidget(spawn_box);
    auto respawn_row = new QHBoxLayout(_creature_spawn_respawn_row);
    respawn_row->setContentsMargins(0, 0, 0, 0);
    respawn_row->setSpacing(3);
    _creature_spawn_respawn_min = new QSpinBox(_creature_spawn_respawn_row);
    _creature_spawn_respawn_max = new QSpinBox(_creature_spawn_respawn_row);
    for (auto* sb : {_creature_spawn_respawn_min, _creature_spawn_respawn_max})
    {
      sb->setRange(0, 7 * 24 * 3600);
      sb->setSuffix(" s");
      sb->setToolTip("Respawn delay range in seconds (spawntimesecsmin/max).\n"
                     "The server rolls a uniform random delay between min and max on each death.");
      respawn_row->addWidget(sb);
    }
    spawn_layout_ext->addRow("Respawn:", _creature_spawn_respawn_row);

    auto wander = new FocusReportingDoubleSpinBox(spawn_box);
    _creature_spawn_wander = wander;
    wander->setRange(0.0, 500.0);
    wander->setDecimals(1);
    wander->setSingleStep(1.0);
    wander->setSuffix(" yd");
    wander->setToolTip("wander_distance -- radius in YARDS around the spawn point the creature\n"
                       "wanders inside when Movement is 1 (Random): the server picks random\n"
                       "points within this distance (RandomMovementGenerator). With Random\n"
                       "movement and 0 here the server falls back to 5 yd.\n"
                       "While this field is focused the orange ground ring shows the radius.");
    spawn_layout_ext->addRow("Wander:", wander);

    _creature_spawn_pct_row = new QWidget(spawn_box);
    auto pct_row = new QHBoxLayout(_creature_spawn_pct_row);
    pct_row->setContentsMargins(0, 0, 0, 0);
    pct_row->setSpacing(3);
    _creature_spawn_health_pct = new QSpinBox(_creature_spawn_pct_row);
    _creature_spawn_mana_pct = new QSpinBox(_creature_spawn_pct_row);
    _creature_spawn_health_pct->setRange(0, 100);
    _creature_spawn_mana_pct->setRange(0, 100);
    _creature_spawn_health_pct->setValue(100);
    _creature_spawn_mana_pct->setValue(100);
    _creature_spawn_health_pct->setSuffix(" %hp");
    _creature_spawn_mana_pct->setSuffix(" %mp");
    _creature_spawn_health_pct->setToolTip("health_percent: the spawn starts at this percent of its maximum health.");
    _creature_spawn_mana_pct->setToolTip("mana_percent: the spawn starts at this percent of its maximum mana.");
    pct_row->addWidget(_creature_spawn_health_pct);
    pct_row->addWidget(_creature_spawn_mana_pct);
    spawn_layout_ext->addRow("Start at:", _creature_spawn_pct_row);

    _creature_spawn_movement = new QComboBox(spawn_box);
    _creature_spawn_movement->addItem("0 - Idle (stands at spawn)");
    _creature_spawn_movement->addItem("1 - Random (wanders in wander radius)");
    _creature_spawn_movement->addItem("2 - Waypoint (follows creature_movement path)");
    _creature_spawn_movement->setToolTip("movement_type (MotionMaster.h):\n"
                                         "0 = Idle -- stands still at the spawn point\n"
                                         "1 = Random -- wanders within wander_distance of the spawn\n"
                                         "2 = Waypoint -- follows its waypoint path from the creature_movement table");
    spawn_layout_ext->addRow("Movement:", _creature_spawn_movement);

    // spawn_flags bitmask (server Object.h SPAWN_FLAG_*): a checkable dropdown so every flag's
    // meaning is spelled out; the button text shows the packed value.
    _creature_spawn_flags_button = new QPushButton("0", spawn_box);
    _creature_spawn_flags_menu = new QMenu(spawn_box);
    struct FlagDef { std::uint32_t bit; char const* label; };
    static constexpr FlagDef spawn_flag_defs[] = {
      {0x01,  "0x01 Active - always updated, even with no players nearby"},
      {0x02,  "0x02 Disabled - spawn is not loaded at all"},
      {0x04,  "0x04 Random respawn time - randomizes the respawn delay"},
      {0x08,  "0x08 Dynamic respawn time - respawn speeds up with server population"},
      {0x10,  "0x10 Force dynamic elite - dynamic-respawn rules treat it as elite"},
      {0x20,  "0x20 Evade out of home area - evades when pulled out of its home area"},
      {0x40,  "0x40 Not visible - spawned but invisible to players"},
      {0x80,  "0x80 Dead - spawns as a corpse"},
      {0x100, "0x100 No dynamic respawn - excluded from dynamic-respawn scaling"},
    };
    for (auto const& def : spawn_flag_defs)
    {
      auto* act = _creature_spawn_flags_menu->addAction(QString::fromLatin1(def.label));
      act->setCheckable(true);
      act->setData(def.bit);
    }
    _creature_spawn_flags_button->setMenu(_creature_spawn_flags_menu);
    _creature_spawn_flags_button->setToolTip("spawn_flags bitmask (server Object.h SPAWN_FLAG_*).\n"
                                             "Check any combination; the button shows the packed value.");
    spawn_layout_ext->addRow("Flags:", _creature_spawn_flags_button);

    _creature_spawn_visibility = new QDoubleSpinBox(spawn_box);
    _creature_spawn_visibility->setRange(0.0, 5000.0);
    _creature_spawn_visibility->setDecimals(1);
    _creature_spawn_visibility->setSingleStep(10.0);
    _creature_spawn_visibility->setSuffix(" yd");
    _creature_spawn_visibility->setSpecialValueText("default");
    _creature_spawn_visibility->setToolTip("visibility_mod -- overrides how far away players still SEE this spawn:\n"
                                           "visible within max(normal visibility distance, this value) yards\n"
                                           "(server Object.cpp). 0 = normal (~90 yd zone visibility). Used for\n"
                                           "world bosses / huge creatures that must render from very far.");
    spawn_layout_ext->addRow("Visibility:", _creature_spawn_visibility);
  }

  splitter->addWidget(list_column);
  splitter->addWidget(filter_panel);
  splitter->addWidget(preview);
  splitter->addWidget(spawn_box);
  if (_creature_editor_panel)
  {
    splitter->addWidget(_creature_editor_panel);
  }
  splitter->setStretchFactor(0, 2);  // model tree
  splitter->setStretchFactor(1, 0);  // filter panel
  splitter->setStretchFactor(2, 3);  // preview
  splitter->setStretchFactor(3, 2);  // edit/new creature (given the coordinate panel's spare space)
  splitter->setStretchFactor(4, 0);  // coordinate editor: starts (and stays) at its minimum width
  splitter->setSizes({380, 130, 520, 520, 240});
  root_layout->addWidget(splitter, 1);

  // Extended-field change handlers: edits write through to the SELECTED spawn (marking it dirty for
  // the SQL export); while authoring a New spawn the values are captured by Add Pending Spawn.
  auto on_ext_changed = [this]()
  {
    if (_creature_spawn_form_updating || !_selected_creature_spawn_guid)
    {
      return;
    }
    applyCreatureExtFormToSpawn(*_selected_creature_spawn_guid);
  };
  for (auto* f : {_creature_spawn_id2_field, _creature_spawn_id3_field, _creature_spawn_id4_field})
  {
    connect(f, &QLineEdit::textEdited, on_ext_changed);
  }
  connect(_creature_spawn_respawn_min, qOverload<int>(&QSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_respawn_max, qOverload<int>(&QSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_wander, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [this, on_ext_changed](double)
          {
            on_ext_changed();
            updateWanderVisualization(); // live ring radius while the value spins
          });
  connect(_creature_spawn_health_pct, qOverload<int>(&QSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_mana_pct, qOverload<int>(&QSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_movement, qOverload<int>(&QComboBox::currentIndexChanged), on_ext_changed);
  connect(_creature_spawn_visibility, qOverload<double>(&QDoubleSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_flags_menu, &QMenu::triggered,
          [this, on_ext_changed](QAction*)
          {
            std::uint32_t v = 0;
            for (auto* act : _creature_spawn_flags_menu->actions())
            {
              if (act->isChecked())
              {
                v |= act->data().toUInt();
              }
            }
            _creature_spawn_flags_value = v;
            updateCreatureSpawnFlagsButton();
            on_ext_changed();
          });
  connect(_creature_spawn_phasemask, qOverload<int>(&QSpinBox::valueChanged), on_ext_changed);
  connect(_creature_spawn_spawnmask_menu, &QMenu::triggered,
          [this, on_ext_changed](QAction*)
          {
            std::uint32_t v = 0;
            for (auto* act : _creature_spawn_spawnmask_menu->actions())
            {
              if (act->isChecked())
              {
                v |= act->data().toUInt();
              }
            }
            _creature_spawn_spawnmask_value = v ? v : 1u; // 0 spawns in nothing -- coerce like the server
            _creature_spawn_spawnmask_button->setText(QString::number(_creature_spawn_spawnmask_value)
              + (_creature_spawn_spawnmask_value == 1 ? " (Normal)" : ""));
            on_ext_changed();
          });
  // The wander-radius ground ring follows the field's keyboard focus ("highlighted and being edited").
  static_cast<FocusReportingDoubleSpinBox*>(_creature_spawn_wander)->on_focus =
    [this](bool focused)
    {
      _creature_wander_field_focused = focused;
      updateWanderVisualization();
    };

  _creature_model_picker_status = new QLabel("Loading creature_template entries...", container);
  _creature_model_picker_status->setWordWrap(true);
  root_layout->addWidget(_creature_model_picker_status);

  struct TemplatePickerEntry
  {
    std::uint32_t entry = 0;
    std::uint32_t faction = 0;
    std::uint32_t creature_type = 0;
    std::uint32_t rank = 0;
    std::uint32_t npc_flags = 0;
    std::uint32_t type_flags = 0;
    std::uint32_t flags_extra = 0;
    std::uint32_t display_id = 0;
    std::uint32_t model_id = 0;
    std::string name;
    std::string path;
    float template_scale = 1.0f;
    float model_scale = 1.0f;
  };

  auto normalize_picker_path = [](std::string path)
  {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    if (auto extension_pos = path.rfind(".mdx"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (auto extension_pos = path.rfind(".mdl"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (path.rfind('.') == std::string::npos)
    {
      path += ".m2";
    }
    return path;
  };

  // model_scale = CreatureModelData.ModelScale (M, intrinsic). display_scale_out =
  // CreatureDisplayInfo.CreatureModelScale (D), the object-scale fallback used when
  // creature_template.scale is 0 (server ObjectMgr.cpp:1436). Final render = (template.scale or D) * M.
  // D is a fallback, NOT an extra multiplier -- do not fold it into model_scale.
  auto resolve_display_model = [&](std::uint32_t display_id,
                                   std::uint32_t& model_id,
                                   std::string& model_path,
                                   float& model_scale,
                                   float& display_scale_out)
  {
    try
    {
      auto display = gCreatureDisplayInfoDB.getByID(display_id);
      model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      auto model = gCreatureModelDataDB.getByID(model_id);
      model_path = normalize_picker_path(model.getString(CreatureModelDataDB::ModelName));
      float display_scale = display.getFloat(CreatureDisplayInfoDB::CreatureModelScale);
      float model_data_scale = model.getFloat(CreatureModelDataDB::ModelScale);
      display_scale_out = display_scale > 0.0f ? display_scale : 1.0f;
      model_scale = model_data_scale > 0.0f ? model_data_scale : 1.0f;
      return !model_path.empty();
    }
    catch (DBCFile::NotFound const&)
    {
      model_id = 0;
      model_path.clear();
      model_scale = 1.0f;
      display_scale_out = 1.0f;
      return false;
    }
  };

  auto template_entries = std::make_shared<std::vector<TemplatePickerEntry>>();

#ifdef USE_MYSQL_UID_STORAGE
  std::string template_error;
  auto records = mysql::getCreatureTemplates(25000, &template_error);
  if (!records.empty())
  {
    // Opening the content database picks the editors' ID range for this schema (vmangos 90000+,
    // Turtle / tortoise-wow 6000000+), which decides below which NPCs count as the user's own.
    mysql::content::Database::open();
  }
  template_entries->reserve(records.size());
  for (auto const& record : records)
  {
    TemplatePickerEntry entry;
    entry.entry = record.entry;
    entry.faction = record.faction;
    entry.creature_type = record.creature_type;
    entry.rank = record.rank;
    entry.npc_flags = record.npc_flags;
    entry.type_flags = record.type_flags;
    entry.flags_extra = record.flags_extra;
    entry.display_id = record.display_id;
    entry.name = record.name;
    entry.template_scale = record.template_scale;
    if (entry.display_id)
    {
      float display_scale = 1.0f;
      resolve_display_model(entry.display_id, entry.model_id, entry.path, entry.model_scale, display_scale);
      // creature_template.scale of 0 -> fall back to CreatureDisplayInfo scale D (server behavior).
      if (entry.template_scale <= 0.0f)
      {
        entry.template_scale = display_scale;
      }
    }
    if (entry.template_scale <= 0.0f)
    {
      entry.template_scale = 1.0f;
    }
    template_entries->push_back(std::move(entry));
  }
#else
  std::string template_error = "Build does not include MySQL support.";
#endif

  // Populate the type dropdown with every distinct creature type present in the loaded list.
  {
    std::set<std::uint32_t> distinct_types;
    for (auto const& entry : *template_entries)
    {
      distinct_types.insert(entry.creature_type);
    }
    for (auto const creature_type : distinct_types)  // std::set keeps them sorted
    {
      QString const label = creature_type
        ? QString("%1 (%2)").arg(creature_type_label(creature_type)).arg(creature_type)
        : QString("None (0)");
      type_filter->addItem(label, static_cast<qulonglong>(creature_type));
    }
  }

  // Share the loaded creature_template type/rank info with the current-map creature browser so it can
  // offer the same Type/Elite/Boss/Civilian/Trainer filter, and populate its Type combo identically.
  {
    _creature_template_filter_info.clear();
    for (auto const& entry : *template_entries)
    {
      _creature_template_filter_info[entry.entry] =
        CreatureFilterInfo{entry.creature_type, entry.rank, entry.type_flags, entry.flags_extra, entry.npc_flags};
    }
    if (_creature_browser_type_filter)
    {
      std::set<std::uint32_t> distinct_types;
      for (auto const& entry : *template_entries)
      {
        distinct_types.insert(entry.creature_type);
      }
      QSignalBlocker blocker(_creature_browser_type_filter);
      for (auto const creature_type : distinct_types)
      {
        QString const label = creature_type
          ? QString("%1 (%2)").arg(creature_type_label(creature_type)).arg(creature_type)
          : QString("None (0)");
        _creature_browser_type_filter->addItem(label, static_cast<qulonglong>(creature_type));
      }
    }
    rebuildCreatureBrowserList(true); // refresh now that the filter data is available
  }

  auto selected_template = std::make_shared<std::optional<TemplatePickerEntry>>();

  auto suggested_guid = [this]()
  {
    std::uint32_t highest = 0;
    for (auto const& spawn : _world->creatureSpawns())
    {
      highest = std::max(highest, spawn.guid);
    }
    return highest + 1;
  };

  auto passes_filters = [=](TemplatePickerEntry const& entry)
  {
    auto const needle = search_box->text().trimmed();
    if (!needle.isEmpty())
    {
      QString const name = QString::fromStdString(entry.name);
      QString const id = QString::number(entry.entry);
      if (!name.contains(needle, Qt::CaseInsensitive) && !id.contains(needle))
      {
        return false;
      }
    }
    auto const type_data = type_filter->currentData();
    if (type_data.isValid() && entry.creature_type != static_cast<std::uint32_t>(type_data.toULongLong()))
    {
      return false;
    }
    if (elite_only->isChecked() && !(entry.rank == 1u || entry.rank == 2u))
    {
      return false;
    }
    if (boss_only->isChecked() && entry.rank < 3u && (entry.type_flags & 0x4u) == 0u)
    {
      return false;
    }
    if (civilian_only->isChecked() && (entry.flags_extra & 0x2u) == 0u && (entry.type_flags & 0x80u) == 0u)
    {
      return false;
    }
    if (trainer_only->isChecked() && (entry.npc_flags & 0x10u) == 0u)
    {
      return false;
    }
    return true;
  };

  auto rebuild_template_tree = [=]()
  {
    _creature_model_tree->clear();
    std::size_t visible_count = 0;
    std::size_t previewable_count = 0;

    for (auto const& entry : *template_entries)
    {
      if (!passes_filters(entry))
      {
        continue;
      }

      ++visible_count;
      if (!entry.path.empty())
      {
        ++previewable_count;
      }

      auto* row = new QTreeWidgetItem(_creature_model_tree);
      row->setText(0, QString("%1 - %2").arg(entry.entry).arg(QString::fromStdString(entry.name)));
      row->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.entry));
      row->setData(0, Qt::UserRole + 1, true);
      if (entry.path.empty())
      {
        row->setForeground(0, QColor(135, 135, 135));
      }
    }

    _creature_model_tree->sortItems(0, Qt::AscendingOrder);

    if (_creature_model_picker_status)
    {
      QString message = QString("%1 creature_template entr%2 shown, %3 previewable")
                          .arg(visible_count)
                          .arg(visible_count == 1 ? "y" : "ies")
                          .arg(previewable_count);
      if (!template_error.empty())
      {
        message = QString("Template load failed: %1").arg(QString::fromStdString(template_error));
      }
      _creature_model_picker_status->setText(message);
    }
  };

  auto update_add_button = [=]()
  {
    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    guid_field->text().toUInt(&guid_ok);
    entry_field->text().toUInt(&entry_ok);
    display_field->text().toUInt(&display_ok);
    add_button->setEnabled(guid_ok && entry_ok && display_ok && selected_template && selected_template->has_value()
                           && !selected_template->value().path.empty());

    bool const has_selection = selected_template && selected_template->has_value();
    bool const is_custom = has_selection && Noggit::Content::isCustom(selected_template->value().entry);
#ifdef USE_MYSQL_UID_STORAGE
    new_npc_button->setEnabled(has_selection);
    edit_npc_button->setEnabled(is_custom);
    delete_npc_button->setEnabled(is_custom);
    delete_npc_button->setToolTip(is_custom ? QString("Delete this NPC, its spawns and its dialogue.")
                                            : QString("Only NPCs made with \"New NPC from selected\" can be deleted."));
#endif
    edit_npc_button->setToolTip(is_custom || !has_selection
      ? QString("Change the selected custom NPC.")
      : QString("Only NPCs made with \"New NPC from selected\" (ID %1 and up) can be edited, so the game's\n"
                "own NPCs stay intact. Make a copy of this one instead.").arg(Noggit::Content::customIds().entry_start));
  };

  auto select_template_entry = [=](std::uint32_t entry_id)
  {
    auto found = std::find_if(template_entries->begin(), template_entries->end(),
      [entry_id](TemplatePickerEntry const& entry)
      {
        return entry.entry == entry_id;
      });

    if (found == template_entries->end())
    {
      selected_template->reset();
      update_add_button();
      return;
    }

    *selected_template = *found;
    guid_field->setText(QString::number(suggested_guid()));
    entry_field->setText(QString::number(found->entry));
    display_field->setText(QString::number(found->display_id));

    if (!found->path.empty())
    {
      World::CreatureSpawnOverlay preview_spawn;
      preview_spawn.guid = suggested_guid();
      preview_spawn.entry = found->entry;
      preview_spawn.display_id = found->display_id;
      preview_spawn.name = found->name;
      preview_spawn.template_scale = found->template_scale;
      preview_spawn.model_scale = found->model_scale;
      preview_spawn.model_path = found->path;
      preview_spawn.is_character_model = preview_spawn.model_path.rfind("character/", 0) == 0;

      try
      {
        preview->setCreatureSpawnPreview(*_world, preview_spawn);
        _creature_model_picker_status->setText(QString("%1 | entry %2 | display %3 | %4")
                                                 .arg(QString::fromStdString(found->name))
                                                 .arg(found->entry)
                                                 .arg(found->display_id)
                                                 .arg(found->creature_type
                                                   ? creature_type_label(found->creature_type)
                                                   : QString("faction %1").arg(found->faction)));
      }
      catch (std::exception const& error)
      {
        _creature_model_picker_status->setText(QString("Preview failed for %1: %2")
                                                 .arg(QString::fromStdString(found->path))
                                                 .arg(error.what()));
      }
      catch (...)
      {
        _creature_model_picker_status->setText(QString("Preview failed for %1")
                                                 .arg(QString::fromStdString(found->path)));
      }
    }
    else
    {
      _creature_model_picker_status->setText(QString("%1 has no previewable CreatureDisplayInfo model")
                                               .arg(QString::fromStdString(found->name)));
    }

    update_add_button();
  };

  auto add_pending_spawn = [=]()
  {
    if (!selected_template || !selected_template->has_value())
    {
      _main_window->statusBar()->showMessage("Select a creature_template entry first", 5000);
      return;
    }

    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    auto const guid = guid_field->text().toUInt(&guid_ok);
    auto const entry_id = entry_field->text().toUInt(&entry_ok);
    auto const display_id = display_field->text().toUInt(&display_ok);
    if (!guid_ok || !entry_ok || !display_ok || !guid || !entry_id || !display_id)
    {
      _main_window->statusBar()->showMessage("Enter a valid GUID, entry, and display ID", 5000);
      return;
    }

    if (_world->findCreatureSpawn(guid))
    {
      _main_window->statusBar()->showMessage(QString("Creature GUID %1 already exists in this overlay").arg(guid), 5000);
      return;
    }

    TemplatePickerEntry entry = selected_template->value();
    if (entry.display_id != display_id)
    {
      entry.display_id = display_id;
      float display_scale = 1.0f;
      resolve_display_model(entry.display_id, entry.model_id, entry.path, entry.model_scale, display_scale);
      if (entry.template_scale <= 0.0f)
      {
        entry.template_scale = display_scale;
      }
    }

    if (entry.path.empty())
    {
      _main_window->statusBar()->showMessage(QString("Creature template %1 has no previewable model").arg(entry_id), 5000);
      return;
    }

    glm::vec3 spawn_pos = _camera.position + _camera.direction() * 8.0f;
    if (spawn_pos.y < -5000.0f)
    {
      spawn_pos = _cursor_pos;
    }

    World::CreatureSpawnOverlay spawn;
    spawn.guid = guid;
    spawn.entry = entry_id;
    spawn.display_id = entry.display_id;
    spawn.name = entry.name.empty() ? "New pending NPC" : entry.name;
    spawn.pos = spawn_pos;
    spawn.original_pos = spawn.pos;
    spawn.orientation = _camera.yaw()._;
    spawn.original_orientation = spawn.orientation;
    spawn.animation_time_offset = static_cast<int>(((guid * 1103515245u) + (entry_id * 12345u)) % 3500u);
    spawn.template_scale = entry.template_scale;
    spawn.model_scale = entry.model_scale;
    spawn.model_path = entry.path;
    spawn.is_character_model = spawn.model_path.rfind("character/", 0) == 0;
    spawn.pending_create = true;
    spawn.dirty = true;
    spawn.selected = true;

    for (auto& existing_spawn : _world->creatureSpawns())
    {
      existing_spawn.selected = false;
    }

    _world->creatureSpawns().push_back(std::move(spawn));
    auto& added_spawn = _world->creatureSpawns().back();
    _world->ensureCreatureSpawnModel(added_spawn);

    {
      SpawnUndoOp op;
      op.kind = SpawnUndoOp::Kind::Create;
      op.guids = {guid};
      pushCreatureUndoOp(std::move(op));
    }

    _selected_creature_spawn_guid = guid;
    // Capture the extended-column widgets (respawn/wander/movement/flags/...) into the new spawn so
    // authored values ride along into the INSERT export.
    applyCreatureExtFormToSpawn(guid);
    scheduleCreatureBrowserRebuild();
    refreshCreatureEditorKnobs();
    updateDatabaseStatus();
    guid_field->setText(QString::number(suggested_guid()));
    _needs_redraw = true;
    _main_window->statusBar()->showMessage(QString("Pending creature spawn %1 added in front of camera").arg(guid), 5000);
  };

  connect(_creature_model_tree, &QTreeWidget::itemClicked,
          [=](QTreeWidgetItem* item, int)
          {
            if (!item || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });
  // Arrow-key navigation CONFIRMS as it moves: stepping with Up/Down previews + selects each entry
  // immediately (no Enter/Space needed). Focus-gated so programmatic tree rebuilds never fire it.
  connect(_creature_model_tree, &QTreeWidget::currentItemChanged,
          [=](QTreeWidgetItem* item, QTreeWidgetItem*)
          {
            if (!item || !_creature_model_tree->hasFocus() || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });

#ifdef USE_MYSQL_UID_STORAGE
  // The content editors (NPCs, quests, items) run on a ContentSession -- an open database plus the pick lists.
  // The map view gives it what only it knows: the cursor position, the 3D preview, reloading the spawns.
  auto const apply_npc_look = [=](CreaturePreviewModelViewer* preview, std::uint32_t display_id, float scale)
  {
    World::CreatureSpawnOverlay spawn;
    spawn.display_id = display_id;
    float display_scale = 1.0f;
    std::uint32_t model_id = 0;
    if (!resolve_display_model(display_id, model_id, spawn.model_path, spawn.model_scale, display_scale))
    {
      LogError << "NPC editor preview: display " << display_id << " has no model" << std::endl;
      return;
    }
    spawn.template_scale = scale > 0.0f ? scale : display_scale;
    spawn.is_character_model = spawn.model_path.rfind("character/", 0) == 0;
    try
    {
      preview->setCreatureSpawnPreview(*_world, spawn);
    }
    catch (std::exception const& e)
    {
      LogError << "NPC editor preview: " << spawn.model_path << " failed: " << e.what() << std::endl;
    }
  };
  auto const open_session = [=]() -> std::unique_ptr<Noggit::Ui::Content::ContentSession>
  {
    auto session = Noggit::Ui::Content::ContentSession::open(this);
    if (!session)
    {
      return nullptr;
    }
    session->cursor_position = [this]() -> std::optional<Noggit::Ui::Content::WorldPosition>
    {
      bool const global_wmo = _world->mapIndex.hasAGlobalWMO();
      auto const server = client_to_server_creature_position(_cursor_pos, global_wmo);
      return Noggit::Ui::Content::WorldPosition{static_cast<std::uint32_t>(_world->getMapID()), server.x, server.y,
                                                server.z, client_to_server_creature_orientation(_camera.yaw()._)};
    };
    session->make_npc_preview = [=]
    {
      auto* preview = new CreaturePreviewModelViewer();
      // initializeGL emits resized(): apply the look held back until then -- on the next event-loop pass,
      // not from inside initializeGL.
      QObject::connect(preview, &Noggit::Ui::Tools::AssetBrowser::ModelViewer::resized, preview, [=]
      {
        if (preview->pending_look)
        {
          QTimer::singleShot(0, preview, [=]
          {
            if (auto const look = std::exchange(preview->pending_look, std::nullopt))
            {
              apply_npc_look(preview, look->first, look->second);
            }
          });
        }
      });
      return preview;
    };
    session->show_npc_look = [=](QWidget* widget, std::uint32_t display_id, float scale)
    {
      // A QOpenGLWidget has no GL context until it is first painted: loading a model before that
      // dereferences null, so hold the look until the widget is initialized.
      auto* preview = static_cast<CreaturePreviewModelViewer*>(widget);
      if (!preview->context() || !preview->context()->isValid())
      {
        preview->pending_look = std::make_pair(display_id, scale);
        return;
      }
      preview->pending_look.reset();
      apply_npc_look(preview, display_id, scale);
    };
    session->on_world_changed = [this] { refreshCreatureSpawnOverlay(true); };
    return session;
  };

  // Shows a new / changed NPC in the picker list without re-reading every template.
  auto const show_saved_npc = [=](TemplatePickerEntry const& source, Noggit::Ui::Npc::NpcSaved const& saved)
  {
    auto const& fields = saved.content.fields;
    TemplatePickerEntry updated = source;
    updated.entry = saved.entry;
    updated.name = fields.name.value_or(source.name);
    updated.faction = fields.faction.value_or(source.faction);
    updated.npc_flags = fields.npc_flags.value_or(source.npc_flags);
    updated.rank = fields.rank.value_or(source.rank);
    if (fields.display_id || fields.scale)
    {
      updated.display_id = fields.display_id.value_or(source.display_id);
      float display_scale = 1.0f;
      resolve_display_model(updated.display_id, updated.model_id, updated.path, updated.model_scale, display_scale);
      float const scale = fields.scale.value_or(saved.before.scale.value_or(0.0f));
      updated.template_scale = scale > 0.0f ? scale : display_scale;
    }
    template_entries->erase(std::remove_if(template_entries->begin(), template_entries->end(),
                                           [&](TemplatePickerEntry const& e) { return e.entry == saved.entry; }),
                            template_entries->end());
    template_entries->push_back(updated);
    _creature_template_filter_info[saved.entry] =
      CreatureFilterInfo{updated.creature_type, updated.rank, updated.type_flags, updated.flags_extra, updated.npc_flags};
    search_box->setText(QString::number(saved.entry)); // rebuilds the list down to this NPC
    select_template_entry(saved.entry);
    if (_creature_model_tree->topLevelItemCount() > 0)
    {
      _creature_model_tree->setCurrentItem(_creature_model_tree->topLevelItem(0));
    }
  };

  auto const run_npc_editor = [=](bool editing)
  {
    if (!selected_template || !selected_template->has_value())
    {
      return;
    }
    TemplatePickerEntry const source = selected_template->value();
    if (auto session = open_session())
    {
      if (auto const saved = Noggit::Ui::Npc::editNpc(*session, this, editing, source.entry))
      {
        show_saved_npc(source, *saved);
      }
    }
  };
  connect(new_npc_button, &QPushButton::clicked, [=] { run_npc_editor(false); });
  connect(edit_npc_button, &QPushButton::clicked, [=] { run_npc_editor(true); });

  connect(delete_npc_button, &QPushButton::clicked, [=]
  {
    if (!selected_template || !selected_template->has_value())
    {
      return;
    }
    std::uint32_t const entry = selected_template->value().entry;
    auto session = open_session();
    if (!session || !Noggit::Ui::Npc::deleteNpc(*session, this, entry))
    {
      return;
    }
    template_entries->erase(std::remove_if(template_entries->begin(), template_entries->end(),
                                           [entry](TemplatePickerEntry const& e) { return e.entry == entry; }),
                            template_entries->end());
    _creature_template_filter_info.erase(entry);
    selected_template->reset();
    search_box->clear();
    rebuild_template_tree();
    update_add_button();
  });

  // Quest browser / editor, focused on the selected NPC when there is one.
  quests_button->setEnabled(true);
  connect(quests_button, &QPushButton::clicked, [=]
  {
    auto session = open_session();
    if (!session)
    {
      return;
    }
    std::uint32_t const focus = selected_template && selected_template->has_value() ? selected_template->value().entry : 0;
    Noggit::Ui::Quest::QuestBrowserDialog browser(*session, focus, this);
    browser.exec();
    // NPCs that became quest givers: keep the picker's filters in step.
    auto const& creatures = *session->lookups().creatures;
    for (auto& entry : *template_entries)
    {
      auto const flags = creatures.kindOf(entry.entry);
      if ((flags & Noggit::Quest::QUEST_GIVER_FLAG) && !(entry.npc_flags & Noggit::Quest::QUEST_GIVER_FLAG))
      {
        entry.npc_flags |= Noggit::Quest::QUEST_GIVER_FLAG;
        if (auto found = _creature_template_filter_info.find(entry.entry); found != _creature_template_filter_info.end())
        {
          found->second.npc_flags = entry.npc_flags;
        }
      }
    }
  });
#endif

  connect(guid_field, &QLineEdit::textChanged, update_add_button);
  connect(entry_field, &QLineEdit::textChanged, update_add_button);
  connect(display_field, &QLineEdit::textChanged, update_add_button);
  connect(type_filter, qOverload<int>(&QComboBox::currentIndexChanged), rebuild_template_tree);
  connect(elite_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(boss_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(civilian_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(trainer_only, &QCheckBox::stateChanged, rebuild_template_tree);
  connect(search_box, &QLineEdit::textChanged, rebuild_template_tree);
  connect(add_button, &QPushButton::clicked, add_pending_spawn);
  preview->on_double_click = add_pending_spawn;

  rebuild_template_tree();

  _creature_model_picker_dock->setWidget(container);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _creature_model_picker_dock);
  _creature_model_picker_dock->setVisible(false);
  connect(this, &QObject::destroyed, _creature_model_picker_dock, &QObject::deleteLater);
}

void MapView::setupGameObjectEditorUi()
{
  // GameObjects have no model picker, so the coordinate editor panel is hosted inside the browser dock
  // (added below the list in setupGameObjectBrowserUi); we just build the panel widget here.
  auto container = new QWidget(this);
  container->setMinimumWidth(240);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  auto coord_title = new QLabel("GameObject Coordinates", container);
  coord_title->setStyleSheet("font-weight: bold;");
  layout->addWidget(coord_title);

  auto hint = new QLabel("Left click: select or drag\nShift: add or box-select", container);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  _gameobject_editor_info = new QLabel("No spawn selected", container);
  _gameobject_editor_info->setWordWrap(true);
  _gameobject_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
  layout->addWidget(_gameobject_editor_info);

  auto make_spin = [&](double lo, double hi, double step) {
    auto* sb = new QDoubleSpinBox(container);
    sb->setRange(lo, hi);
    sb->setDecimals(3);
    sb->setSingleStep(step);
    sb->setEnabled(false);
    return sb;
  };

  // Match the creature editor: client coordinates can exceed 20000, so use a wide range to avoid clamping.
  _go_spawn_edit_x           = make_spin(-64000.0, 64000.0, 0.1);
  _go_spawn_edit_y           = make_spin(-20000.0, 20000.0, 0.1);
  _go_spawn_edit_z           = make_spin(-64000.0, 64000.0, 0.1);
  _go_spawn_edit_orientation = make_spin(     0.0,   360.0, 1.0);
  _go_spawn_edit_orientation->setWrapping(true);
  _go_spawn_edit_orientation->setSuffix(QString::fromUtf8("\xc2\xb0"));  // ┬░

  auto form = new QFormLayout();
  form->setContentsMargins(0, 4, 0, 0);
  form->setSpacing(3);
  form->addRow("X:", _go_spawn_edit_x);
  form->addRow("Y (height):", _go_spawn_edit_y);
  form->addRow("Z:", _go_spawn_edit_z);
  form->addRow("Orientation:", _go_spawn_edit_orientation);
  layout->addLayout(form);

  auto on_change = [this](double) {
    if (!_selected_gameobject_spawn_guid)
      return;
    auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
    if (!spawn)
      return;

    glm::vec3 const new_pos(
      static_cast<float>(_go_spawn_edit_x->value()),
      static_cast<float>(_go_spawn_edit_y->value()),
      static_cast<float>(_go_spawn_edit_z->value()));
    float const new_orientation = static_cast<float>(_go_spawn_edit_orientation->value());
    if (glm::distance(new_pos, spawn->pos) > 0.0001f
        || std::abs(new_orientation - spawn->orientation) > 0.0001f)
    {
      SpawnUndoOp op;
      op.kind = SpawnUndoOp::Kind::Move;
      op.from_spinbox = true;
      op.moves.push_back({spawn->guid, spawn->pos, spawn->orientation});
      pushGameObjectUndoOp(std::move(op));
    }
    spawn->pos = new_pos;
    spawn->orientation = new_orientation;
    spawn->dirty = spawn->pending_create
                || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

    if (spawn->model_instance)
    {
      spawn->model_instance->pos = spawn->pos;
      spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
      spawn->model_instance->recalcExtents();
    }

    updateGameObjectBrowserStatus();
    refreshGameObjectBrowserItems({spawn->guid});
    _needs_redraw = true;
  };

  connect(_go_spawn_edit_x,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_y,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_z,           qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);
  connect(_go_spawn_edit_orientation, qOverload<double>(&QDoubleSpinBox::valueChanged), on_change);

  _gameobject_editor_panel = container;
}

void MapView::setupGameObjectBrowserUi()
{
  _gameobject_browser_dock = new QDockWidget("GameObject Browser", _main_window);
  _gameobject_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                        | QDockWidget::DockWidgetFloatable
                                        | QDockWidget::DockWidgetClosable);
  _gameobject_browser_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
  _gameobject_browser_dock->setMinimumWidth(330);
  _gameobject_browser_dock->resize(380, 520);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _gameobject_browser_dock);
  connect(this, &QObject::destroyed, _gameobject_browser_dock, &QObject::deleteLater);

  auto container = new QWidget(this);
  auto layout = new QVBoxLayout(container);
  layout->setContentsMargins(6, 6, 6, 6);

  _gameobject_search_field = new QLineEdit(container);
  _gameobject_search_field->setPlaceholderText("Search by gameobject name, guid, or entry");
  layout->addWidget(_gameobject_search_field);

  _gameobject_zone_filter = new QCheckBox("Zone only (current zone)", container);
  _gameobject_zone_filter->setToolTip("Only list gameobjects whose position is in the same zone as the"
                                      " camera. Spawns in unloaded tiles are excluded.");
  layout->addWidget(_gameobject_zone_filter);
  connect(_gameobject_zone_filter, &QCheckBox::toggled, [this]() { rebuildGameObjectBrowserList(true); });

  // Type filter mirroring the gameobject model picker. Filters the current-map spawn list by the
  // spawned object's template type (looked up per spawn entry from _gameobject_template_filter_type;
  // the combo is populated by the model picker when it loads gameobject_template).
  {
    auto type_row = new QHBoxLayout();
    type_row->addWidget(new QLabel("Type", container));
    _gameobject_browser_type_filter = new QComboBox(container);
    _gameobject_browser_type_filter->addItem("All types"); // index 0 -> no type filter
    _gameobject_browser_type_filter->setToolTip("Filter the spawn list by gameobject type.");
    type_row->addWidget(_gameobject_browser_type_filter, 1);
    layout->addLayout(type_row);
    connect(_gameobject_browser_type_filter, qOverload<int>(&QComboBox::currentIndexChanged),
            [this]() { rebuildGameObjectBrowserList(true); });
  }

  _gameobject_list_widget = new QListWidget(container);
  _gameobject_list_widget->setSelectionMode(QAbstractItemView::SingleSelection);
  _gameobject_list_widget->setMinimumHeight(280);
  layout->addWidget(_gameobject_list_widget, 1);

  _gameobject_browser_status = new QLabel(container);
  _gameobject_browser_status->setWordWrap(true);
  layout->addWidget(_gameobject_browser_status);

  // The coordinate editor panel now lives in the model picker (last splitter column),
  // mirroring the creature tool.

  _gameobject_browser_dock->setWidget(container);
  _gameobject_browser_dock->setVisible(false);

  connect(_gameobject_browser_dock, &QDockWidget::visibilityChanged,
          [this](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue("map_view/gameobject_browser", visible);
            _settings->sync();
          });

  connect(&_show_gameobject_browser, &Noggit::BoolToggleProperty::changed,
          [this](bool visible)
          {
            bool const show = visible && !ui_hidden && terrainMode == editing_mode::gameobject;
            if (ui_hidden && visible)
            {
              return;
            }

            if (_gameobject_actions_overlay)
            {
              _gameobject_actions_overlay->setVisible(show);
            }

            if (_gameobject_browser_dock)
            {
              _gameobject_browser_dock->setVisible(show);
            }

            if (_gameobject_model_picker_dock)
            {
              _gameobject_model_picker_dock->setVisible(show);
            }
          });
  connect(_gameobject_browser_dock, &QDockWidget::visibilityChanged,
          &_show_gameobject_browser, &Noggit::BoolToggleProperty::set);

  connect(_gameobject_search_field, &QLineEdit::textChanged,
          [this]()
          {
            rebuildGameObjectBrowserList(true);
          });
  connect(_gameobject_list_widget, &QListWidget::itemClicked,
          this, &MapView::jumpToGameObjectListItem);
  // Arrow keys confirm as the highlight moves (see the creature browser above).
  connect(_gameobject_list_widget, &QListWidget::currentItemChanged,
          [this](QListWidgetItem* current, QListWidgetItem*)
          {
            if (current && _gameobject_list_widget->hasFocus())
            {
              jumpToGameObjectListItem(current);
            }
          });

  updateGameObjectBrowserStatus();
}

void MapView::setupGameObjectModelPickerUi()
{
  _gameobject_model_picker_dock = new QDockWidget("GameObject Model Picker", _main_window);
  _gameobject_model_picker_dock->setFeatures(QDockWidget::DockWidgetMovable
                                             | QDockWidget::DockWidgetFloatable
                                             | QDockWidget::DockWidgetClosable);
  _gameobject_model_picker_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  _gameobject_model_picker_dock->setMinimumHeight(220);

  auto container = new QWidget(this);
  auto root_layout = new QVBoxLayout(container);
  root_layout->setContentsMargins(6, 6, 6, 6);
  root_layout->setSpacing(6);

  // Gameobject "Type" label helper (mirrors creature_type_label, but local to the picker).
  auto gameobject_type_label = [](std::uint32_t type) -> QString
  {
    switch (type)
    {
      case 0:  return "Door";
      case 1:  return "Button";
      case 2:  return "Quest Giver";
      case 3:  return "Chest";
      case 4:  return "Binding";
      case 5:  return "Generic";
      case 6:  return "Trap";
      case 7:  return "Chair";
      case 8:  return "Spell Focus";
      case 9:  return "Text";
      case 10: return "Goober";
      case 11: return "Transport";
      case 12: return "Area Damage";
      case 13: return "Camera";
      case 14: return "Map Object";
      case 15: return "Mo Transport";
      case 17: return "Fishing Node";
      case 21: return "Door (summon)";
      case 22: return "Summoning Ritual";
      case 25: return "Auction House";
      case 26: return "Guard";
      default: return QString("Type %1").arg(type);
    }
  };

  // Filters live in a thin vertical panel beside the list (added to the splitter below).
  auto filter_panel = new QWidget(container);
  filter_panel->setMaximumWidth(150);
  auto filter_layout = new QVBoxLayout(filter_panel);
  filter_layout->setContentsMargins(4, 4, 4, 4);
  filter_layout->setSpacing(4);
  auto filter_title = new QLabel("Filters", filter_panel);
  filter_title->setStyleSheet("font-weight: bold;");
  filter_layout->addWidget(filter_title);
  auto type_label = new QLabel("Type", filter_panel);
  auto type_filter = new QComboBox(filter_panel);
  type_filter->setToolTip("Filter entries by gameobject type. Populated from the loaded gameobject_template list.");
  type_filter->addItem("All types");  // index 0: no data -> no type filter
  filter_layout->addWidget(type_label);
  filter_layout->addWidget(type_filter);
  filter_layout->addStretch();

  auto splitter = new QSplitter(Qt::Horizontal, container);

  // Search box sits directly above the model tree (list) column at the list width.
  auto list_column = new QWidget(splitter);
  auto list_column_layout = new QVBoxLayout(list_column);
  list_column_layout->setContentsMargins(0, 0, 0, 0);
  list_column_layout->setSpacing(4);

  auto search_box = new QLineEdit(list_column);
  search_box->setPlaceholderText("Search by entry id or name...");
  search_box->setClearButtonEnabled(true);
  search_box->setToolTip("Filter the list by gameobject_template entry id or name (case-insensitive).");
  list_column_layout->addWidget(search_box);

  _gameobject_model_tree = new QTreeWidget(list_column);
  _gameobject_model_tree->setHeaderHidden(true);
  _gameobject_model_tree->setMinimumWidth(360);
  list_column_layout->addWidget(_gameobject_model_tree, 1);

  auto preview = new CreaturePreviewModelViewer(splitter);
  preview->setMinimumSize(360, 220);

  auto spawn_box = new QGroupBox("Edit/New GameObject", splitter);
  _gameobject_spawn_box = spawn_box;
  auto spawn_layout = new QFormLayout(spawn_box);
  auto guid_field = new QLineEdit(spawn_box);
  auto entry_field = new QLineEdit(spawn_box);
  auto display_field = new QLineEdit(spawn_box);
  _gameobject_spawn_guid_field = guid_field;
  _gameobject_spawn_entry_field = entry_field;
  _gameobject_spawn_display_field = display_field;
  auto add_button = new QPushButton("Add Pending Spawn", spawn_box);
  add_button->setEnabled(false);
  guid_field->setPlaceholderText("GUID");
  entry_field->setPlaceholderText("Entry");
  display_field->setPlaceholderText("Display ID");
  spawn_layout->addRow("GUID:", guid_field);
  spawn_layout->addRow("Entry:", entry_field);
  spawn_layout->addRow("Display:", display_field);
  spawn_layout->addRow(add_button);

  splitter->addWidget(list_column);
  splitter->addWidget(filter_panel);
  splitter->addWidget(preview);
  splitter->addWidget(spawn_box);
  if (_gameobject_editor_panel)
  {
    splitter->addWidget(_gameobject_editor_panel);
  }
  splitter->setStretchFactor(0, 2);  // model tree
  splitter->setStretchFactor(1, 0);  // filter panel
  splitter->setStretchFactor(2, 3);  // preview
  splitter->setStretchFactor(3, 1);  // new spawn
  splitter->setStretchFactor(4, 1);  // coordinate editor
  splitter->setSizes({380, 130, 520, 240, 280});
  root_layout->addWidget(splitter, 1);

  _gameobject_model_picker_status = new QLabel("Loading gameobject_template entries...", container);
  _gameobject_model_picker_status->setWordWrap(true);
  root_layout->addWidget(_gameobject_model_picker_status);

  struct TemplatePickerEntry
  {
    std::uint32_t entry = 0;
    std::uint32_t type = 0;
    std::uint32_t display_id = 0;
    std::string name;
    std::string path;
    float template_scale = 1.0f;
  };

  auto normalize_picker_path = [](std::string path)
  {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    if (auto extension_pos = path.rfind(".mdx"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (auto extension_pos = path.rfind(".mdl"); extension_pos != std::string::npos)
    {
      path.replace(extension_pos, 4, ".m2");
    }
    else if (path.rfind('.') == std::string::npos)
    {
      path += ".m2";
    }
    return path;
  };

  auto resolve_display_model = [&](std::uint32_t display_id, std::string& model_path)
  {
    try
    {
      auto display = gGameObjectDisplayInfoDB.getByID(display_id);
      model_path = normalize_picker_path(display.getString(GameObjectDisplayInfoDB::ModelName));
      return !model_path.empty();
    }
    catch (DBCFile::NotFound const&)
    {
      model_path.clear();
      return false;
    }
  };

  auto template_entries = std::make_shared<std::vector<TemplatePickerEntry>>();

#ifdef USE_MYSQL_UID_STORAGE
  std::string template_error;
  auto records = mysql::getGameObjectTemplates(25000, &template_error);
  template_entries->reserve(records.size());
  for (auto const& record : records)
  {
    TemplatePickerEntry entry;
    entry.entry = record.entry;
    entry.type = record.type;
    entry.display_id = record.display_id;
    entry.name = record.name;
    entry.template_scale = record.template_scale;
    if (entry.display_id)
    {
      resolve_display_model(entry.display_id, entry.path);
    }
    template_entries->push_back(std::move(entry));
  }
#else
  std::string template_error = "Build does not include MySQL support.";
#endif

  // Populate the type dropdown with every distinct gameobject type present in the loaded list.
  {
    std::set<std::uint32_t> distinct_types;
    for (auto const& entry : *template_entries)
    {
      distinct_types.insert(entry.type);
    }
    for (auto const type : distinct_types)  // std::set keeps them sorted
    {
      type_filter->addItem(QString("%1 (%2)").arg(gameobject_type_label(type)).arg(type),
                           static_cast<qulonglong>(type));
    }
  }

  // Share the gameobject template type info with the current-map browser so it can offer the same Type
  // filter, and populate its Type combo identically.
  {
    _gameobject_template_filter_type.clear();
    for (auto const& entry : *template_entries)
    {
      _gameobject_template_filter_type[entry.entry] = entry.type;
    }
    if (_gameobject_browser_type_filter)
    {
      std::set<std::uint32_t> distinct_types;
      for (auto const& entry : *template_entries)
      {
        distinct_types.insert(entry.type);
      }
      QSignalBlocker blocker(_gameobject_browser_type_filter);
      for (auto const type : distinct_types)
      {
        _gameobject_browser_type_filter->addItem(
          QString("%1 (%2)").arg(gameobject_type_label(type)).arg(type), static_cast<qulonglong>(type));
      }
    }
    rebuildGameObjectBrowserList(true);
  }

  auto selected_template = std::make_shared<std::optional<TemplatePickerEntry>>();

  auto suggested_guid = [this]()
  {
    std::uint32_t highest = 0;
    for (auto const& spawn : _world->gameObjectSpawns())
    {
      highest = std::max(highest, spawn.guid);
    }
    return highest + 1;
  };

  auto passes_filters = [=](TemplatePickerEntry const& entry)
  {
    auto const needle = search_box->text().trimmed();
    if (!needle.isEmpty())
    {
      QString const name = QString::fromStdString(entry.name);
      QString const id = QString::number(entry.entry);
      if (!name.contains(needle, Qt::CaseInsensitive) && !id.contains(needle))
      {
        return false;
      }
    }
    auto const type_data = type_filter->currentData();
    if (type_data.isValid() && entry.type != static_cast<std::uint32_t>(type_data.toULongLong()))
    {
      return false;
    }
    return true;
  };

  auto rebuild_template_tree = [=]()
  {
    _gameobject_model_tree->clear();
    std::size_t visible_count = 0;
    std::size_t previewable_count = 0;

    for (auto const& entry : *template_entries)
    {
      if (!passes_filters(entry))
      {
        continue;
      }

      ++visible_count;
      if (!entry.path.empty())
      {
        ++previewable_count;
      }

      auto* row = new QTreeWidgetItem(_gameobject_model_tree);
      row->setText(0, QString("%1 - %2").arg(entry.entry).arg(QString::fromStdString(entry.name)));
      row->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.entry));
      row->setData(0, Qt::UserRole + 1, true);
      if (entry.path.empty())
      {
        row->setForeground(0, QColor(135, 135, 135));
      }
    }

    _gameobject_model_tree->sortItems(0, Qt::AscendingOrder);

    if (_gameobject_model_picker_status)
    {
      QString message = QString("%1 gameobject_template entr%2 shown, %3 previewable")
                          .arg(visible_count)
                          .arg(visible_count == 1 ? "y" : "ies")
                          .arg(previewable_count);
      if (!template_error.empty())
      {
        message = QString("Template load failed: %1").arg(QString::fromStdString(template_error));
      }
      _gameobject_model_picker_status->setText(message);
    }
  };

  auto update_add_button = [=]()
  {
    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    guid_field->text().toUInt(&guid_ok);
    entry_field->text().toUInt(&entry_ok);
    display_field->text().toUInt(&display_ok);
    add_button->setEnabled(guid_ok && entry_ok && display_ok && selected_template && selected_template->has_value()
                           && !selected_template->value().path.empty());
  };

  auto select_template_entry = [=](std::uint32_t entry_id)
  {
    auto found = std::find_if(template_entries->begin(), template_entries->end(),
      [entry_id](TemplatePickerEntry const& entry)
      {
        return entry.entry == entry_id;
      });

    if (found == template_entries->end())
    {
      selected_template->reset();
      update_add_button();
      return;
    }

    *selected_template = *found;
    guid_field->setText(QString::number(suggested_guid()));
    entry_field->setText(QString::number(found->entry));
    display_field->setText(QString::number(found->display_id));

    if (!found->path.empty())
    {
      World::CreatureSpawnOverlay preview_spawn;
      preview_spawn.guid = suggested_guid();
      preview_spawn.entry = found->entry;
      preview_spawn.display_id = found->display_id;
      preview_spawn.name = found->name;
      preview_spawn.template_scale = found->template_scale;
      preview_spawn.model_scale = 1.0f;
      preview_spawn.model_path = found->path;
      preview_spawn.is_character_model = false;

      try
      {
        preview->setCreatureSpawnPreview(*_world, preview_spawn);
        _gameobject_model_picker_status->setText(QString("%1 | entry %2 | display %3 | %4")
                                                 .arg(QString::fromStdString(found->name))
                                                 .arg(found->entry)
                                                 .arg(found->display_id)
                                                 .arg(gameobject_type_label(found->type)));
      }
      catch (std::exception const& error)
      {
        _gameobject_model_picker_status->setText(QString("Preview failed for %1: %2")
                                                 .arg(QString::fromStdString(found->path))
                                                 .arg(error.what()));
      }
      catch (...)
      {
        _gameobject_model_picker_status->setText(QString("Preview failed for %1")
                                                 .arg(QString::fromStdString(found->path)));
      }
    }
    else
    {
      _gameobject_model_picker_status->setText(QString("%1 has no previewable GameObjectDisplayInfo model")
                                               .arg(QString::fromStdString(found->name)));
    }

    update_add_button();
  };

  auto add_pending_spawn = [=]()
  {
    if (!selected_template || !selected_template->has_value())
    {
      _main_window->statusBar()->showMessage("Select a gameobject_template entry first", 5000);
      return;
    }

    bool guid_ok = false;
    bool entry_ok = false;
    bool display_ok = false;
    auto const guid = guid_field->text().toUInt(&guid_ok);
    auto const entry_id = entry_field->text().toUInt(&entry_ok);
    auto const display_id = display_field->text().toUInt(&display_ok);
    if (!guid_ok || !entry_ok || !display_ok || !guid || !entry_id || !display_id)
    {
      _main_window->statusBar()->showMessage("Enter a valid GUID, entry, and display ID", 5000);
      return;
    }

    if (_world->findGameObjectSpawn(guid))
    {
      _main_window->statusBar()->showMessage(QString("GameObject GUID %1 already exists in this overlay").arg(guid), 5000);
      return;
    }

    TemplatePickerEntry entry = selected_template->value();
    if (entry.display_id != display_id)
    {
      entry.display_id = display_id;
      resolve_display_model(entry.display_id, entry.path);
    }

    if (entry.path.empty())
    {
      _main_window->statusBar()->showMessage(QString("GameObject template %1 has no previewable model").arg(entry_id), 5000);
      return;
    }

    glm::vec3 spawn_pos = _camera.position + _camera.direction() * 8.0f;
    if (spawn_pos.y < -5000.0f)
    {
      spawn_pos = _cursor_pos;
    }

    World::GameObjectSpawnOverlay spawn;
    spawn.guid = guid;
    spawn.entry = entry_id;
    spawn.display_id = entry.display_id;
    spawn.name = entry.name.empty() ? "New pending gameobject" : entry.name;
    spawn.pos = spawn_pos;
    spawn.original_pos = spawn.pos;
    spawn.orientation = _camera.yaw()._;
    spawn.original_orientation = spawn.orientation;
    spawn.template_scale = entry.template_scale;
    spawn.model_path = entry.path;
    spawn.pending_create = true;
    spawn.dirty = true;
    spawn.selected = true;

    for (auto& existing_spawn : _world->gameObjectSpawns())
    {
      existing_spawn.selected = false;
    }

    _world->gameObjectSpawns().push_back(std::move(spawn));
    auto& added_spawn = _world->gameObjectSpawns().back();
    _world->ensureGameObjectSpawnModel(added_spawn);

    {
      SpawnUndoOp op;
      op.kind = SpawnUndoOp::Kind::Create;
      op.guids = {guid};
      pushGameObjectUndoOp(std::move(op));
    }

    _selected_gameobject_spawn_guid = guid;
    scheduleGameObjectBrowserRebuild();
    refreshGameObjectEditorKnobs();
    updateGameObjectBrowserStatus();
    guid_field->setText(QString::number(suggested_guid()));
    _needs_redraw = true;
    _main_window->statusBar()->showMessage(QString("Pending gameobject spawn %1 added in front of camera").arg(guid), 5000);
  };

  connect(_gameobject_model_tree, &QTreeWidget::itemClicked,
          [=](QTreeWidgetItem* item, int)
          {
            if (!item || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });
  // Arrow-key navigation CONFIRMS as it moves: stepping with Up/Down previews + selects each entry
  // immediately (no Enter/Space needed). Focus-gated so programmatic tree rebuilds never fire it.
  connect(_gameobject_model_tree, &QTreeWidget::currentItemChanged,
          [=](QTreeWidgetItem* item, QTreeWidgetItem*)
          {
            if (!item || !_gameobject_model_tree->hasFocus() || !item->data(0, Qt::UserRole + 1).toBool())
            {
              return;
            }
            select_template_entry(static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toULongLong()));
          });

  connect(guid_field, &QLineEdit::textChanged, update_add_button);
  connect(entry_field, &QLineEdit::textChanged, update_add_button);
  connect(display_field, &QLineEdit::textChanged, update_add_button);
  connect(type_filter, qOverload<int>(&QComboBox::currentIndexChanged), rebuild_template_tree);
  connect(search_box, &QLineEdit::textChanged, rebuild_template_tree);
  connect(add_button, &QPushButton::clicked, add_pending_spawn);
  preview->on_double_click = add_pending_spawn;

  rebuild_template_tree();

  _gameobject_model_picker_dock->setWidget(container);
  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _gameobject_model_picker_dock);
  _gameobject_model_picker_dock->setVisible(false);
  connect(this, &QObject::destroyed, _gameobject_model_picker_dock, &QObject::deleteLater);
}

void MapView::setupGameObjectActionsUi()
{
  if (!_overlay_widget)
  {
    return;
  }

  _gameobject_actions_overlay = new QWidget(_overlay_widget);
  _gameobject_actions_overlay->setObjectName("gameobjectActionsOverlay");
  _gameobject_actions_overlay->setAttribute(Qt::WA_StyledBackground, true);
  _gameobject_actions_overlay->setStyleSheet(
    "#gameobjectActionsOverlay { background: rgba(28, 31, 37, 210); border: 1px solid rgba(85, 91, 103, 180); }"
    "#gameobjectActionsOverlay QPushButton { padding: 5px 9px; }");

  auto layout = new QHBoxLayout(_gameobject_actions_overlay);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);

  auto reload_button = new QPushButton("Reload Spawns", _gameobject_actions_overlay);
  auto save_button = new QPushButton("Export SQL", _gameobject_actions_overlay);
  auto revert_button = new QPushButton("Discard Pending", _gameobject_actions_overlay);
  auto pending_button = new QPushButton("Pending \xE2\x96\xBE", _gameobject_actions_overlay);
  pending_button->setToolTip("Show the list of pending gameobject updates waiting for SQL export.");
  layout->addWidget(reload_button);
  layout->addWidget(save_button);
  layout->addWidget(revert_button);
  layout->addWidget(pending_button);

  // Toggleable dropdown listing every pending change (moved / deleted) awaiting SQL export.
  _gameobject_pending_popup = new QWidget(this, Qt::Popup);
  _gameobject_pending_popup->setObjectName("gameobjectPendingPopup");
  _gameobject_pending_popup->setAttribute(Qt::WA_StyledBackground, true);
  _gameobject_pending_popup->setStyleSheet(
    "#gameobjectPendingPopup { background: rgba(28, 31, 37, 235); border: 1px solid rgba(85, 91, 103, 200); }");
  auto pending_layout = new QVBoxLayout(_gameobject_pending_popup);
  pending_layout->setContentsMargins(6, 6, 6, 6);
  pending_layout->setSpacing(4);
  auto pending_title = new QLabel("Pending gameobject updates", _gameobject_pending_popup);
  pending_title->setStyleSheet("font-weight: bold; color: #ddd;");
  pending_layout->addWidget(pending_title);
  _gameobject_pending_list = new QListWidget(_gameobject_pending_popup);
  _gameobject_pending_list->setMinimumSize(360, 220);
  _gameobject_pending_list->setSelectionMode(QListWidget::NoSelection);
  pending_layout->addWidget(_gameobject_pending_list);

  auto refresh_pending = [this]()
  {
    if (!_gameobject_pending_list)
    {
      return;
    }
    _gameobject_pending_list->clear();
    int count = 0;
    for (auto const& spawn : _world->gameObjectSpawns())
    {
      if (!spawn.dirty)
      {
        continue;
      }
      QString action = spawn.pending_delete ? "DELETE" : "MOVE";
      QString const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
      _gameobject_pending_list->addItem(
        QString("[%1] guid %2  entry %3  %4").arg(action).arg(spawn.guid).arg(spawn.entry).arg(name));
      ++count;
    }
    if (count == 0)
    {
      _gameobject_pending_list->addItem("No pending updates.");
    }
  };

  connect(pending_button, &QPushButton::clicked,
          [this, pending_button, refresh_pending]()
          {
            if (!_gameobject_pending_popup)
            {
              return;
            }
            if (_gameobject_pending_popup->isVisible())
            {
              _gameobject_pending_popup->hide();
              return;
            }
            refresh_pending();
            _gameobject_pending_popup->adjustSize();
            QPoint const below = pending_button->mapToGlobal(QPoint(0, pending_button->height() + 2));
            _gameobject_pending_popup->move(below);
            _gameobject_pending_popup->show();
          });

  auto place_actions = [this]()
  {
    if (!_gameobject_actions_overlay)
    {
      return;
    }

    _gameobject_actions_overlay->adjustSize();
    _gameobject_actions_overlay->move(std::max(8, width() - _gameobject_actions_overlay->width() - 14), 8);
    _gameobject_actions_overlay->raise();
  };

  _gameobject_actions_overlay->setVisible(false);
  place_actions();
  connect(this, &MapView::resized, this, place_actions);
  connect(this, &QObject::destroyed, _gameobject_actions_overlay, &QObject::deleteLater);

  connect(reload_button, &QPushButton::clicked,
          [this]()
          {
            _world->ensureGameObjectSpawnsLoaded();
            rebuildGameObjectBrowserList(true);
          });
  connect(save_button, &QPushButton::clicked,
          [this]()
          {
            saveDirtyGameObjectSpawns();
          });
  connect(revert_button, &QPushButton::clicked,
          [this]()
          {
            discardPendingGameObjectSpawns();
          });
}

QToolButton* MapView::makeSeasonalEventsToolButton(QWidget* parent)
{
  // Calendar section button hosted under the time-globe popup's slider. Its menu of checkable game-events
  // (checked = that event's creatures/objects render; nothing = base world only, the default) flies out to
  // the RIGHT of the popup instead of dropping down, so it expands the panel sideways. Rebuilt on open so
  // it always reflects the events in the loaded spawns. Affects both creature AND gameobject spawns.
  auto button = new QToolButton(parent);
  button->setIcon(make_calendar_icon());
  button->setIconSize(QSize(18, 18));
  button->setText("Seasonal Events");
  button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  button->setToolTip("Toggle which event's creatures and objects are shown.\n"
                     "Nothing checked = base world only.");
  button->setAutoRaise(true);

  // Not setMenu() (that drops DOWN): pop the menu manually anchored to the host popup's top-right corner so
  // it opens to the right, like a submenu flyout.
  auto menu = new MultiToggleMenu(button);
  connect(menu, &QMenu::aboutToShow, [this, menu]() { populateSeasonalEventsMenu(menu); });
  connect(button, &QToolButton::clicked, [button, menu]()
  {
    if (menu->isVisible())
    {
      menu->hide();
      return;
    }
    QWidget* const host = button->parentWidget() ? button->parentWidget() : button;
    QPoint const anchor = host->mapToGlobal(QPoint(host->width() + 2, 0));
    menu->popup(anchor);
  });
  return button;
}

QWidget* MapView::makeWeatherWidget(QWidget* parent)
{
  // Weather section hosted in the time-globe popup, beside the Seasonal Events button. The client
  // mechanism: the zone light blends toward its STORM Light.dbc param set (WPL's orange clear fog
  // becomes the authored beige storm fog) and the precipitation pass draws rain streaks/snow flakes
  // with the client's own textures. Runtime-only preview state (World::weather_type/intensity).
  auto box = new QWidget(parent);
  auto col = new QVBoxLayout(box);
  col->setContentsMargins(4, 2, 4, 2);
  col->setSpacing(2);

  auto row = new QHBoxLayout();
  row->setSpacing(4);
  auto group = new QButtonGroup(box);
  auto add_type = [&](char const* label, int type) {
    auto b = new QToolButton(box);
    b->setText(label);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setChecked(_world->weather_type == type);
    group->addButton(b, type);
    row->addWidget(b);
  };
  add_type("None", 0);
  add_type("Rain", 1);
  add_type("Snow", 2);
  group->setExclusive(true);
  connect(group, qOverload<int>(&QButtonGroup::idClicked),
          [this](int type) { _world->weather_type = type; });
  col->addLayout(row);

  auto slider = new QSlider(Qt::Horizontal, box);
  slider->setRange(0, 100);
  slider->setValue(static_cast<int>(_world->weather_intensity * 100.f));
  slider->setToolTip("Weather intensity");
  connect(slider, &QSlider::valueChanged,
          [this](int v) { _world->weather_intensity = v / 100.f; });
  col->addWidget(slider);

  return box;
}

void MapView::populateSeasonalEventsMenu(QMenu* menu)
{
  if (!menu)
  {
    return;
  }

  menu->clear();

  auto const entries = _world->spawnedEventEntries();
  if (entries.empty())
  {
    auto* empty = menu->addAction(_world->hasCreatureSpawnsLoaded()
                                    ? "No seasonal-event spawns on this map"
                                    : "Enable creature/object spawns first");
    empty->setEnabled(false);
    return;
  }

  auto* all_action = menu->addAction("Show all events");
  auto* none_action = menu->addAction("Hide all (base world only)");
  connect(all_action, &QAction::triggered, [this]()
  {
    for (auto entry : _world->spawnedEventEntries())
    {
      _world->setEventActive(entry, true);
    }
    update();
  });
  connect(none_action, &QAction::triggered, [this]()
  {
    _world->clearActiveEvents();
    update();
  });
  menu->addSeparator();

  auto const& names = _world->gameEventNames();
  for (std::int32_t entry : entries)
  {
    auto const name_it = names.find(entry);
    QString const label = name_it != names.end() && !name_it->second.empty()
      ? QString("%1 - %2").arg(entry).arg(QString::fromStdString(name_it->second))
      : QString("Event %1").arg(entry);

    auto* action = menu->addAction(label);
    action->setCheckable(true);
    action->setChecked(_world->isEventActive(entry));
    connect(action, &QAction::toggled, [this, entry](bool checked)
    {
      _world->setEventActive(entry, checked);
      update();
    });
  }
}

void MapView::setupMinimapEditorUi()
{
  minimapTool = new Noggit::Ui::MinimapCreator(this, _world.get(), this);
  _tool_panel_dock->registerTool("Minimap Editor", minimapTool);
}
void MapView::setupStampUi()
{
  stampTool = new Noggit::Ui::Tools::BrushStack(this, this);
  _tool_panel_dock->registerTool("Stamp", stampTool);
}

void MapView::setupLightEditorUi()
{
  lightEditor = new Noggit::Ui::Tools::LightEditor(this, this);
  _tool_panel_dock->registerTool("Light Editor", lightEditor);
}

void MapView::setupChunkManipulatorUi()
{
  _chunk_manipulator = new Noggit::Ui::Tools::ChunkManipulator::ChunkManipulatorPanel(this, this);
  _tool_panel_dock->registerTool("Chunk Manipulator", _chunk_manipulator);
}

void MapView::setupNodeEditor()
{
  _node_editor_dock = new QDockWidget("Node editor", this);
  _node_editor_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);

  _main_window->addDockWidget(Qt::LeftDockWidgetArea, _node_editor_dock);
  _node_editor_dock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);

  // QtNodes startup can still abort the process during style initialization.
  // Keep the dock shell, but do not restore/create the widget automatically.
  auto ensure_node_editor = [this]()
  {
    if (_node_editor_dock->widget())
    {
      return;
    }

    try
    {
      auto node_editor = new Noggit::Ui::Tools::NodeEditor::Ui::NodeEditorWidget(this);
      _node_editor_dock->setWidget(node_editor);
    }
    catch (...)
    {
      LogError << "Failed to initialize node editor dock during map view setup; hiding dock to keep map load alive."
               << std::endl;
      _settings->setValue("map_view/node_editor", false);
      _settings->sync();
      _node_editor_dock->hide();
    }
  };

  _settings->setValue("map_view/node_editor", false);
  _settings->sync();
  _node_editor_dock->hide();

  connect(_node_editor_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (visible)
            {
              _node_editor_dock->hide();
              return;
            }

            if (ui_hidden)
              return;

            _settings->setValue ("map_view/node_editor", visible);
            _settings->sync();
          });

  connect(this, &QObject::destroyed, _node_editor_dock, &QObject::deleteLater);

  connect ( &_show_node_editor, &Noggit::BoolToggleProperty::changed
    , _node_editor_dock, [this]
            {
              if (!ui_hidden)
                _node_editor_dock->setVisible(_show_node_editor.get());
            }
  );

  connect ( _node_editor_dock, &QDockWidget::visibilityChanged
    , &_show_node_editor, &Noggit::BoolToggleProperty::set
  );

}

void MapView::setupAssetBrowser()
{
  _asset_browser_dock = new QDockWidget("Asset browser", this);
  _asset_browser = new Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget(this, this);

  //_main_window->addDockWidget(Qt::BottomDockWidgetArea, _asset_browser_dock);
  _asset_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                   | QDockWidget::DockWidgetFloatable
                                   | QDockWidget::DockWidgetClosable);
  _asset_browser_dock->setAllowedAreas(Qt::NoDockWidgetArea);

  _asset_browser_dock->setFloating(true);
  _asset_browser_dock->hide();

  _asset_browser_dock->setWidget(_asset_browser);
  _asset_browser_dock->setWindowFlags(
    Qt::CustomizeWindowHint |
    Qt::Window | 
    Qt::WindowMinimizeButtonHint |
    Qt::WindowMaximizeButtonHint |
    Qt::WindowCloseButtonHint | 
    Qt::WindowStaysOnTopHint);

  connect(_asset_browser_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/asset_browser", visible);
            _settings->sync();
          });;

  connect(this, &QObject::destroyed, _asset_browser_dock, &QObject::deleteLater);

}

void MapView::setupDetailInfos()
{

  // Dock
  _detail_infos_dock = new QDockWidget("Detail info", this);
  _detail_infos_dock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);

  _detail_infos_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);


  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _detail_infos_dock);
  _detail_infos_dock->setFloating(true);
  _detail_infos_dock->hide();
  // End Dock

  guidetailInfos = new Noggit::Ui::detail_infos(this);
  _detail_infos_dock->setWidget(guidetailInfos);;


  connect ( &_show_detail_info_window, &Noggit::BoolToggleProperty::changed
    , guidetailInfos, [this]
            {
              if (!ui_hidden)
                _detail_infos_dock->setVisible(_show_detail_info_window.get());
            }
  );

  connect ( guidetailInfos, &Noggit::Ui::widget::visibilityChanged
    , &_show_detail_info_window, &Noggit::BoolToggleProperty::set
  );

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionBegin,
    [this](Noggit::Action*)
    {
      updateDetailInfos(true);
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionEnd,
    [this](Noggit::Action*)
    {
      updateDetailInfos(true);
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::currentActionChanged,
    [this](unsigned)
    {
      updateDetailInfos(true);
    });
}

void MapView::updateDetailInfos(bool no_sel_change_check)
{
  auto& current_selection = _world->current_selection();

  // update detail infos TODO: selection update signal.
  static std::uintptr_t last_sel = 0;

  if (guidetailInfos->isVisible())
  {
    if (current_selection.size() > 0)
    {
      selection_type& last_selection = const_cast<selection_type&>(current_selection.at(current_selection.size() - 1));

      switch (last_selection.index())
      {
        case eEntry_Object:
        {
          auto obj = std::get<selected_object_type>(last_selection);

          if (no_sel_change_check || reinterpret_cast<std::uintptr_t>(obj) != last_sel || NOGGIT_CUR_ACTION)
          {
            last_sel = reinterpret_cast<std::uintptr_t>(obj);
            obj->updateDetails(guidetailInfos);
          }
          break;
        }
        case eEntry_MapChunk:
        {
          selected_chunk_type& chunk_sel(std::get<selected_chunk_type>(last_selection));

          if (no_sel_change_check || reinterpret_cast<std::uintptr_t>(chunk_sel.chunk) != last_sel || NOGGIT_CUR_ACTION)
          {
            last_sel = reinterpret_cast<std::uintptr_t>(chunk_sel.chunk);
            chunk_sel.updateDetails(guidetailInfos);
          }
          break;
        }
      }
    }
    else
    {
      guidetailInfos->setText("");
    }
  }
}

void MapView::setupToolbars()
{
  _toolbar = new Noggit::Ui::toolbar([this] (editing_mode mode) { set_editing_mode (mode); });
  _toolbar->setOrientation(Qt::Vertical);
  auto right_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftToolbarHolder);
  right_toolbar_layout->addWidget( _toolbar);
  right_toolbar_layout->setDirection(QBoxLayout::LeftToRight);
  right_toolbar_layout->setContentsMargins(0, 5, 0, 5);
  connect (this, &QObject::destroyed, _toolbar, &QObject::deleteLater);

  auto left_sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftSecondaryToolbarHolder);
  left_sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _left_sec_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, terrainMode);
  connect(this, &QObject::destroyed, _left_sec_toolbar, &QObject::deleteLater);
  left_sec_toolbar_layout->addWidget( _left_sec_toolbar);

  auto top_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->upperToolbarHolder);
  top_toolbar_layout->setContentsMargins(5, 0, 5, 0);
  auto sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->secondaryToolbarHolder);
  sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _viewport_overlay_ui->secondaryToolbarHolder->hide();
  _secondary_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this);
  connect (this, &QObject::destroyed, _secondary_toolbar, &QObject::deleteLater);

  _view_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, _secondary_toolbar);
  connect (this, &QObject::destroyed, _view_toolbar, &QObject::deleteLater);

  top_toolbar_layout->addWidget( _view_toolbar);
  sec_toolbar_layout->addWidget( _secondary_toolbar);

  // WC3-style time-of-day globe: its own widget, NEVER inside a button layout (so it can't stretch the
  // toolbars to its height). The icon strip is split into a left half (_view_toolbar) and a right half
  // (right_toolbar); the globe sits between them and all three are laid out as siblings, vertically
  // centred, so the button strips keep their normal small height and only the globe is tall.
  _time_globe = new Noggit::Ui::TimeGlobeWidget(this);
  _time_globe->setFixedSize(_time_globe->sizeHint());

  auto* right_toolbar = new QToolBar(_overlay_widget);
  right_toolbar->setMovable(false);
  right_toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
  right_toolbar->setIconSize(_view_toolbar->iconSize());
  right_toolbar->setStyleSheet(_view_toolbar->styleSheet());
  right_toolbar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  right_toolbar->setContentsMargins(0, 0, 0, 0);
  // no padding between the button strips and the globe
  _view_toolbar->setContentsMargins(0, 0, 0, 0);
  top_toolbar_layout->setContentsMargins(0, 0, 0, 0);
  {
    auto const acts = _view_toolbar->actions();
    int const half = (static_cast<int>(acts.size()) + 1) / 2;
    for (int i = half; i < acts.size(); ++i)
    {
      QAction* const a = acts.at(i);
      _view_toolbar->removeAction(a);
      right_toolbar->addAction(a);
    }
  }
  // Hover tool-options (secondaryToolbarHolder, shown via getSecondaryToolBar()) stay in the row BELOW the
  // globe so they never shove it when they pop up on hover.
  _viewport_overlay_ui->horizontalLayout_4->removeWidget(_viewport_overlay_ui->secondaryToolbarHolder);
  if (auto* below = _viewport_overlay_ui->horizontalLayout_8)
    below->insertWidget(0, _viewport_overlay_ui->secondaryToolbarHolder, 0, Qt::AlignTop);

  // The MODE secondary tools (Patrol paths / Creature info in creature mode; flatten & texture options in
  // the terrain modes -> getLeftSecondaryToolbar()) live in leftSecondaryToolbarHolder, which the .ui put
  // in the SECOND row. The tall time globe -- added to the row ABOVE it -- pushed that whole second row
  // down, so the panel rendered well below the icon strip. Lift the holder INTO the globe's row at the FAR
  // LEFT, TOP-aligned, so it sits at the same Y as the icons again (it is hidden except in the modes that
  // populate it). A zero-width spacer on the far right, whose width tracks the holder (see eventFilter),
  // keeps the centred globe block at the true viewport centre when the holder appears -- nothing shoves
  // the globe.
  if (auto* below = _viewport_overlay_ui->horizontalLayout_8)
    below->removeWidget(_viewport_overlay_ui->leftSecondaryToolbarHolder);

  _globe_balance_spacer = new QWidget(_overlay_widget);
  _globe_balance_spacer->setFixedWidth(0);
  _globe_balance_spacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  _viewport_overlay_ui->leftSecondaryToolbarHolder->installEventFilter(this);

  if (auto* row = _viewport_overlay_ui->horizontalLayout_4)
  {
    // drop the trailing .ui spacer
    for (int i = row->count() - 1; i >= 0; --i)
    {
      if (row->itemAt(i)->spacerItem()) { delete row->takeAt(i); break; }
    }
    // Row order: [leftSecondary][stretch] main | globe | right-toolbar [stretch][balance-spacer]. Spacing 0
    // so the button strips butt up against the globe; top-aligned so only the tall globe hangs down.
    row->setSpacing(0);
    row->insertWidget(1, right_toolbar, 0, Qt::AlignTop);
    row->insertWidget(1, _time_globe, 0, Qt::AlignTop);
    row->insertStretch(0, 1);
    row->addStretch(1);
    row->addWidget(_globe_balance_spacer, 0, Qt::AlignTop);
    row->insertWidget(0, _viewport_overlay_ui->leftSecondaryToolbarHolder, 0, Qt::AlignTop);
    row->setAlignment(_viewport_overlay_ui->upperToolbarHolder, Qt::AlignTop);
    row->setAlignment(_viewport_overlay_ui->leftSecondaryToolbarHolder, Qt::AlignTop);
  }
}

void MapView::setupKeybindingsGui()
{
  _keybindings = new Noggit::Ui::help(this);
  _keybindings->hide();
  connect(this, &QObject::destroyed, _keybindings, &QObject::deleteLater);

  connect ( &_show_keybindings_window, &Noggit::BoolToggleProperty::changed
    , _keybindings, &QWidget::setVisible
  );

  connect ( _keybindings, &Noggit::Ui::widget::visibilityChanged
    , &_show_keybindings_window, &Noggit::BoolToggleProperty::set
  );
}

void MapView::setupFileMenu()
{
  auto file_menu (_main_window->_menuBar->addMenu ("Editor"));
  connect (this, &QObject::destroyed, file_menu, &QObject::deleteLater);

  ADD_ACTION (file_menu, "Save current tile", "Ctrl+Shift+S", [this] { save(save_mode::current); emit saved();});
  ADD_ACTION (file_menu, "Save changed tiles", QKeySequence::Save, [this] { save(save_mode::changed); emit saved(); });
  ADD_ACTION (file_menu, "Save all tiles", "Ctrl+Shift+A", [this] { save(save_mode::all); emit saved(); });
  ADD_ACTION(file_menu, "Generate new WDL", "", [this] 
      { 
     QMessageBox prompt;
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
     prompt.setText(std::string("Warning!\nThis will attempt to load all tiles in the map to generate a new WDL."
         "\nThis is likely to crash if there is any issue with any tile, it is recommended that you save your work first. Only use this if you really need a fresh WDL.").c_str());
     prompt.setInformativeText(std::string("Are you sure ?").c_str());
     prompt.setStandardButtons(QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
     prompt.setDefaultButton(QMessageBox::No);
     bool answer = prompt.exec() == QMessageBox::StandardButton::Yes;
     if (answer)
         _world->horizon.save_wdl(_world.get(), true);
      }
  );

  ADD_ACTION ( file_menu
  , "Reload tile"
  , "Shift+J"
  , [this]
               {
                 makeCurrent();
                 OpenGL::context::scoped_setter const _ (::gl, context());
                 _world->reload_tile (_camera.position);
                 _rotation_editor_need_update = true;
                 emit saved();
               }
  );

  file_menu->addSeparator();
  ADD_ACTION_NS (file_menu, "Force uid check on next opening", [this] { _force_uid_check = true; });
  file_menu->addSeparator();

  ADD_ACTION ( file_menu
  , "Add bookmark"
  , Qt::CTRL | Qt::Key_F5
      , [this]
      {

          auto bookmark = Noggit::Project::NoggitProjectBookmarkMap();
          bookmark.position = _camera.position;
          bookmark.camera_pitch = _camera.pitch()._;
          bookmark.camera_yaw = _camera.yaw()._;
          bookmark.map_id = _world->getMapID();
          bookmark.name = gAreaDB.getAreaName(_world->getAreaID(_camera.position));

        _project->createBookmark(bookmark);

      }
  );

  ADD_ACTION(file_menu
      , "Write coordinates to port.txt and copy to clipboard"
      , Qt::Key_G
      , [this]
      {
                 std::stringstream port_command;
                 port_command << ".go XYZ " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << _world->getMapID();
                 std::ofstream f("ports.txt", std::ios_base::app);
                 f << "Map: " << gAreaDB.getAreaName(_world->getAreaID (_camera.position)) << " on ADT " << std::floor(_camera.position.x / TILESIZE) << " " << std::floor(_camera.position.z / TILESIZE) << std::endl;
                 f << "Trinity/AC:" << std::endl << port_command.str() << std::endl;
                 // f << "ArcEmu:" << std::endl << ".worldport " << _world->getMapID() << " " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << std::endl << std::endl;
                 f.close();
                 QClipboard* clipboard = QGuiApplication::clipboard();
                 clipboard->setText(port_command.str().c_str(), QClipboard::Clipboard);
               }
  );

}

void MapView::setupEditMenu()
{
  auto edit_menu (_main_window->_menuBar->addMenu ("Edit"));
  connect (this, &QObject::destroyed, edit_menu, &QObject::deleteLater);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Selected object"));
  edit_menu->addSeparator();
  ADD_ACTION (edit_menu, "Delete", Qt::Key_Delete, [this]
  {
    if (terrainMode == editing_mode::creature)
    {
      deleteSelectedCreatureSpawns();
      return;
    }
    if (terrainMode == editing_mode::gameobject)
    {
      deleteSelectedGameObjectSpawns();
      return;
    }
    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
    DeleteSelectedObjects();
    NOGGIT_ACTION_MGR->endAction();
  });

  ADD_ACTION (edit_menu, "Reset rotation", "Ctrl+R",
              [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                ResetSelectedObjectRotation();
                NOGGIT_ACTION_MGR->endAction();
              });
  ADD_ACTION (edit_menu, "Set to ground", Qt::Key_PageDown,
              [this] {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                snap_selected_models_to_the_ground();
                NOGGIT_ACTION_MGR->endAction();

              });

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Options"));
  edit_menu->addSeparator();
  ADD_TOGGLE_NS (edit_menu, "Locked cursor mode", _locked_cursor_mode);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("State"));
  edit_menu->addSeparator();
  ADD_ACTION (edit_menu, "Undo", "Ctrl+Z", [this]
  {
    // In the spawn tools Ctrl+Z is DEDICATED to creature/gameobject edits: it walks only their own
    // undo stack (moves, additions, deletions) and NEVER falls through to the terrain/water/object
    // ActionManager history -- an empty stack is simply "nothing to undo" in these modes.
    if (terrainMode == editing_mode::creature)
    {
      if (!undoCreatureEdit())
      {
        _main_window->statusBar()->showMessage("Nothing to undo (creature edits)", 3000);
      }
      return;
    }
    if (terrainMode == editing_mode::gameobject)
    {
      if (!undoGameObjectEdit())
      {
        _main_window->statusBar()->showMessage("Nothing to undo (gameobject edits)", 3000);
      }
      return;
    }
    NOGGIT_ACTION_MGR->undo();
  });
  ADD_ACTION (edit_menu, "Redo", "Ctrl+Shift+Z", [this] { NOGGIT_ACTION_MGR->redo(); });
}

void MapView::setupAssistMenu()
{
  auto assist_menu (_main_window->_menuBar->addMenu ("Assist"));
  connect (this, &QObject::destroyed, assist_menu, &QObject::deleteLater);

  ADD_ACTION_NS (assist_menu, "Reload creature spawns", [this] { refreshCreatureSpawnOverlay(true); });
  ADD_ACTION_NS (assist_menu, "Export creature spawn SQL", [this] { saveDirtyCreatureSpawns(); });

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Database"));
  assist_menu->addSeparator();
#ifdef USE_MYSQL_UID_STORAGE
  // Push flows: everything targets the PROJECT's configured connection (Settings -> MySQL), so a
  // Turtle project only ever writes to its own tw_world-style schema.
  ADD_ACTION_NS (assist_menu, "Apply pending creature changes to database", [this] { applyDirtyCreatureSpawnsToDb(); });
  ADD_ACTION_NS (assist_menu, "Apply pending gameobject changes to database", [this] { applyDirtyGameObjectSpawnsToDb(); });
  ADD_ACTION_NS (assist_menu, "Apply SQL file to database...", [this] { applySqlFileToDb(); });
  ADD_ACTION_NS (assist_menu, "Reset database from SQL folders...", [this] { resetDatabaseFromSqlFolders(); });
#endif

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Model"));
  assist_menu->addSeparator();
  ADD_ACTION (assist_menu, "Last M2 from WMV", "Shift+V", [this] { objectEditor->import_last_model_from_wmv(eMODEL); });
  ADD_ACTION (assist_menu, "Last WMO from WMV", "Alt+V", [this] { objectEditor->import_last_model_from_wmv(eWMO); });
  ADD_ACTION_NS (assist_menu, "Helper models", [this] { objectEditor->helper_models_widget->show(); });

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Current ADT"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Set Area ID"
  , [this]
                  {
                    if (_selected_area_id != -1)
                    {
                      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_AREAID);
                      _world->setAreaID(_camera.position, _selected_area_id, true);
                      NOGGIT_ACTION_MGR->endAction();
                    }
                  }
  );

  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      _world->ensureAllTilesetsADT(_camera.position);
      NOGGIT_ACTION_MGR->endAction();

    }
  );

  auto cleanup_menu (assist_menu->addMenu ("Clean up"));

  ADD_ACTION_NS ( cleanup_menu
  , "Clear height map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
                    _world->clearHeight(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Remove texture duplicates"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->removeTexDuplicateOnADT(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->clearTextures(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures + set base"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->setBaseTexture(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear shadows"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNK_SHADOWS);
                    _world->clear_shadows(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear models"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                    _world->clearAllModelsOnADT(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                    _rotation_editor_need_update = true;
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear duplicate models"
  , [this]
                  {
                    DESTRUCTIVE_ACTION
                      (
                        makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());
                    _world->delete_duplicate_model_and_wmo_instances();
                    )
                  }
  );

  auto cur_adt_export_menu(assist_menu->addMenu("Export"));
  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _world->exportADTAlphamap(_camera.position);
  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());

    if (!!Noggit::Ui::selected_texture::get())
    {
      _world->exportADTAlphamap(_camera.position, Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
    }

  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export vertex color map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());

                    _world->exportADTVertexColorMap(_camera.position);
                  }
  );

  QDialog* heightmap_export_params = new QDialog(this);
  heightmap_export_params->setWindowFlags(Qt::Popup);
  heightmap_export_params->setWindowTitle("Heightmap Exporter");
  QVBoxLayout* heightmap_export_params_layout = new QVBoxLayout(heightmap_export_params);

  heightmap_export_params_layout->addWidget(new QLabel("Min:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_min = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_min->setRange(-10000000, 10000000);
  heightmap_export_params_layout->addWidget(heightmap_export_min);

  heightmap_export_params_layout->addWidget(new QLabel("Max:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_max = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_max->setRange(-10000000, 10000000);
  heightmap_export_max->setValue(100.0);
  heightmap_export_params_layout->addWidget(heightmap_export_max);

  QPushButton* heightmap_export_okay = new QPushButton("Okay", heightmap_export_params);
  heightmap_export_params_layout->addWidget(heightmap_export_okay);

  connect(heightmap_export_min, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_max->value() > value))
              heightmap_export_max->setValue(value + 1.0);

          });

  connect(heightmap_export_max, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_min->value() < value))
              heightmap_export_min->setValue(value - 1.0);

          });

  connect(heightmap_export_okay, &QPushButton::clicked
    ,[=]()
    {
      heightmap_export_params->accept();

    });



  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export heightmap"
  , [=]
              {
                QPoint new_pos = QCursor::pos();

                heightmap_export_params->setGeometry(new_pos.x(),
                new_pos.y(),
                heightmap_export_params->width(),
                heightmap_export_params->height());

                if (heightmap_export_params->exec() == QDialog::Accepted)
                {
                  makeCurrent();
                  OpenGL::context::scoped_setter const _(::gl, context());

                  _world->exportADTHeightmap(_camera.position, heightmap_export_min->value(), heightmap_export_max->value());
                }

              }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export normalmap"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportADTNormalmap(_camera.position);
      }
  );

  auto cur_adt_import_menu(assist_menu->addMenu("Import"));


  QDialog* adt_import_params = new QDialog(this);
  adt_import_params->setWindowFlags(Qt::Popup);
  adt_import_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_params_layout = new QVBoxLayout(adt_import_params);

  adt_import_params_layout->addWidget(new QLabel("Layer:", adt_import_params));
  QSpinBox* adt_import_params_layer = new QSpinBox(adt_import_params);
  adt_import_params_layer->setRange(1, 3);
  adt_import_params_layout->addWidget(adt_import_params_layer);

  QPushButton* adt_import_params_okay = new QPushButton("Okay", adt_import_params);
  adt_import_params_layout->addWidget(adt_import_params_okay);

  connect(adt_import_params_okay, &QPushButton::clicked
    ,[=]()
    {
      adt_import_params->accept();

    });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamap (file)"
  , [=]
                  {
                    QPoint new_pos = QCursor::pos();

                    adt_import_params->setGeometry(new_pos.x(),
                                                   new_pos.y(),
                                                   heightmap_export_params->width(),
                                                   heightmap_export_params->height());

                    if (adt_import_params->exec() == QDialog::Accepted)
                    {
                      makeCurrent();
                      OpenGL::context::scoped_setter const _(::gl, context());

                      QString filepath = QFileDialog::getOpenFileName(
                        this,
                        tr("Open alphamap"),
                        "",
                        "PNG file (*.png);;"
                      );

                      if(!QFileInfo::exists(filepath))
                        return;

                      QImage img;
                      img.load(filepath, "PNG");

                      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                      _world->importADTAlphamap(_camera.position, img, adt_import_params_layer->value());
                      NOGGIT_ACTION_MGR->endAction();
                    }

                  }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamap"
  , [=]
    {

        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importADTAlphamap(_camera.position);
        NOGGIT_ACTION_MGR->endAction();
    }
  );

  QDialog* adt_import_height_params = new QDialog(this);
  adt_import_height_params->setWindowFlags(Qt::Popup);
  adt_import_height_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_height_params_layout = new QVBoxLayout(adt_import_height_params);

  adt_import_height_params_layout->addWidget(new QLabel("Multiplier:", adt_import_height_params));
  QDoubleSpinBox* adt_import_height_params_multiplier = new QDoubleSpinBox(adt_import_height_params);
  adt_import_height_params_multiplier->setRange(0, 100000000);
  adt_import_height_params_layout->addWidget(adt_import_height_params_multiplier);

  adt_import_height_params_layout->addWidget(new QLabel("Mode:", adt_import_height_params));
  QComboBox* adt_import_height_params_mode = new QComboBox(adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_mode);
  adt_import_height_params_mode->addItems({"Set", "Add", "Subtract", "Multiply" });

  QCheckBox* adt_import_height_tiled_edges = new QCheckBox("Tiled Edges", adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_tiled_edges);

  QPushButton* adt_import_height_params_okay = new QPushButton("Okay", adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_okay);

  connect(adt_import_height_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_height_params->accept();

          });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap (file)"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          QString filepath = QFileDialog::getOpenFileName(
            this,
            tr("Open heightmap (257x257)"),
            "",
            "PNG file (*.png);;"
          );

          if(!QFileInfo::exists(filepath))
            return;

          QImage img;
          img.load(filepath, "PNG");

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, img, adt_import_height_params_multiplier->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, adt_import_height_params_multiplier->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  QDialog* adt_import_vcol_params = new QDialog(this);
  adt_import_vcol_params->setWindowFlags(Qt::Popup);
  adt_import_vcol_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_vcol_params_layout = new QVBoxLayout(adt_import_vcol_params);

  adt_import_vcol_params_layout->addWidget(new QLabel("Mode:", adt_import_vcol_params));
  QComboBox* adt_import_vcol_params_mode = new QComboBox(adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode);
  adt_import_vcol_params_mode->addItems({"Set", "Add", "Subtract", "Multiply"});

  QCheckBox* adt_import_vcol_params_mode_tiled_edges = new QCheckBox("Tiled Edges", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode_tiled_edges);

  QPushButton* adt_import_vcol_params_okay = new QPushButton("Okay", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_okay);

  connect(adt_import_vcol_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_vcol_params->accept();

          });


  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map (file)"
  , [=]
    {
      if (adt_import_vcol_params->exec() == QDialog::Accepted)
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        QString filepath = QFileDialog::getOpenFileName(
          this,
          tr("Open vertex color map (257x257)"),
          "",
          "PNG file (*.png);;"
        );

        if(!QFileInfo::exists(filepath))
          return;

        QImage img;
        img.load(filepath, "PNG");

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
        _world->importADTVertexColorMap(_camera.position, img, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
        NOGGIT_ACTION_MGR->endAction();
      }
    }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map"
  , [=]
      {
        if (adt_import_vcol_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importADTVertexColorMap(_camera.position, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );


  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Loaded ADTs"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Fix gaps"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
        _world->fixAllGaps();
        NOGGIT_ACTION_MGR->endAction();
      }
  );

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Global"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Map to big alpha"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        _world->convert_alphamap(true);
      )

    }
  );
  ADD_ACTION_NS ( assist_menu
  , "Map to old alpha"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->convert_alphamap(false);
      )
    }
  );


  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
      {
        DESTRUCTIVE_ACTION
        (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          _world->ensureAllTilesetsAllADTs();
        )

      }
  );

  auto all_adts_export_menu(assist_menu->addMenu("Export"));

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportAllADTsAlphamap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      if (!!Noggit::Ui::selected_texture::get())
      {
        _world->exportAllADTsAlphamap(Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
      }
    )
  }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export heightmap"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsHeightmap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export vertex color map"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsVertexColorMap();
      )
    }
  );

  auto all_adts_import_menu(assist_menu->addMenu("Import"));

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import alphamaps"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importAllADTsAlphamaps();
        NOGGIT_ACTION_MGR->endAction();

    )
  }
  );

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import heightmaps"
  , [=]
    {
      if (adt_import_height_params->exec() == QDialog::Accepted)
      {
        DESTRUCTIVE_ACTION
        (
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
            _world->importAllADTsHeightmaps(adt_import_height_params_multiplier->value(), adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
            NOGGIT_ACTION_MGR->endAction();
        )

      }
    }
  );

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import vertex color maps"
  , [=]
  {
    if (adt_import_vcol_params->exec() == QDialog::Accepted)
    {
      DESTRUCTIVE_ACTION
      (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importAllADTVertexColorMaps(adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
      )

    }
  }
  );

  auto debug_menu(assist_menu->addMenu("Debug"));

  ADD_ACTION_NS ( debug_menu
  , "Load all tiles"
  , [=]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _world->loadAllTiles();
  }
  );

}

void MapView::setupViewMenu()
{
  // Apply persisted graphics toggles (Settings > Graphics > Render features) over the header
  // defaults, so the user's render preferences survive restarts. Done BEFORE the ADD_TOGGLE macros
  // below so the View-menu checkmarks reflect the loaded state. The pure render-path toggles are
  // read live via property.get() each frame; the two terrain-param toggles also need their uniform
  // block synced once here (the ADD_TOGGLE_POST lambdas only run that on user toggle, not at setup).
  _draw_models.set           (_settings->value("render/doodads",          true ).toBool());
  _draw_wmo_doodads.set      (_settings->value("render/wmo_doodads",      true ).toBool());
  _draw_wmo.set              (_settings->value("render/wmo",              true ).toBool());
  _draw_terrain.set          (_settings->value("render/terrain",          true ).toBool());
  _draw_water.set            (_settings->value("render/water",            true ).toBool());
  _draw_clouds.set           (_settings->value("render/draw_clouds",      true ).toBool());
  _draw_sun.set              (_settings->value("render/draw_sun",         true ).toBool());
  _draw_moon.set             (_settings->value("render/draw_moon",        true ).toBool());
  _draw_model_animations.set (_settings->value("render/model_animations", true ).toBool());
  _draw_bloom.set            (_settings->value("render/bloom",            true ).toBool());
  _draw_ground_clutter.set   (_settings->value("render/ground_clutter",   true ).toBool());
  _wdl_horizon_occlusion.set (_settings->value("render/wdl_horizon_occlusion", false).toBool());
  _draw_fog.set              (_settings->value("render/fog",              true ).toBool());
  _draw_vertex_color.set     (_settings->value("render/vertex_color",     true ).toBool());
  _draw_baked_shadows.set    (_settings->value("render/baked_shadows",    true ).toBool());
  if (_world && _world->renderer())
  {
    auto* tp = _world->renderer()->getTerrainParamsUniformBlock();
    tp->draw_vertex_color = _draw_vertex_color.get();
    tp->draw_shadows = _draw_baked_shadows.get();
    _world->renderer()->markTerrainParamsUniformBlockDirty();
  }

  auto view_menu (_main_window->_menuBar->addMenu ("View"));
  connect (this, &QObject::destroyed, view_menu, &QObject::deleteLater);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Drawing"));
  view_menu->addSeparator();
  ADD_TOGGLE (view_menu, "Doodads",     Qt::Key_F1, _draw_models);
  ADD_TOGGLE (view_menu, "WMO doodads", Qt::Key_F2, _draw_wmo_doodads);
  ADD_TOGGLE (view_menu, "Terrain",     Qt::Key_F3, _draw_terrain);
  ADD_TOGGLE (view_menu, "Water",       Qt::Key_F4, _draw_water);
  // Clouds/sun/moon render-paths read the SETTINGS each frame (Skies::draw_clouds and the
  // WorldRender celestial block), so persist on toggle.
  ADD_TOGGLE_POST (view_menu, "Clouds", Qt::SHIFT | Qt::Key_F5, _draw_clouds,
                   [=]
                   {
                     _settings->setValue("render/draw_clouds", _draw_clouds.get());
                     _settings->sync();
                   });
  ADD_TOGGLE_POST (view_menu, "Sun", Qt::SHIFT | Qt::Key_F6, _draw_sun,
                   [=]
                   {
                     _settings->setValue("render/draw_sun", _draw_sun.get());
                     _settings->sync();
                   });
  ADD_TOGGLE_POST (view_menu, "Moon", Qt::SHIFT | Qt::Key_F7, _draw_moon,
                   [=]
                   {
                     _settings->setValue("render/draw_moon", _draw_moon.get());
                     _settings->sync();
                   });
  ADD_TOGGLE (view_menu, "Bloom",       Qt::Key_F5, _draw_bloom);
  ADD_TOGGLE (view_menu, "WMOs",        Qt::Key_F6, _draw_wmo);
  ADD_TOGGLE (view_menu, "Ground clutter", Qt::SHIFT | Qt::Key_G, _draw_ground_clutter);
  // WorldRender reads render/wdl_horizon_occlusion each frame, so persist on toggle.
  ADD_TOGGLE_POST (view_menu, "WDL horizon occlusion (perf)", 0, _wdl_horizon_occlusion,
                   [=]
                   {
                     _settings->setValue("render/wdl_horizon_occlusion", _wdl_horizon_occlusion.get());
                     _settings->sync();
                   });

  ADD_TOGGLE_POST (view_menu, "Lines", Qt::Key_F7, _draw_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = _draw_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Contours", Qt::Key_F9, _draw_contour,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_terrain_height_contour = _draw_contour.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Wireframe", Qt::Key_F10, _draw_wireframe,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_wireframe = _draw_wireframe.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE (view_menu, "Toggle Animation", Qt::Key_F11, _draw_model_animations);
  ADD_TOGGLE (view_menu, "Draw fog", Qt::Key_F12, _draw_fog);

  ADD_TOGGLE_POST (view_menu, "Hole lines", Qt::SHIFT | Qt::Key_F1, _draw_hole_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = _draw_hole_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST(view_menu, "Climb", Qt::SHIFT | Qt::Key_F2, _draw_climb,
                  [=]
                  {
                      _world->renderer()->getTerrainParamsUniformBlock()->draw_impassible_climb = _draw_climb.get();
                      _world->renderer()->markTerrainParamsUniformBlockDirty();
                  });

  ADD_TOGGLE_POST(view_menu, "Vertex Color", Qt::SHIFT | Qt::Key_F3, _draw_vertex_color,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_vertex_color = _draw_vertex_color.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_POST(view_menu, "Baked Shadows", Qt::SHIFT | Qt::Key_F4, _draw_baked_shadows,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_shadows = _draw_baked_shadows.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_NS (view_menu, "Flight Bounds", _draw_mfbo);

  ADD_TOGGLE_NS (view_menu, "Models with box", _draw_models_with_box);
  //! \todo space+h in object mode
  ADD_TOGGLE_NS (view_menu, "Hidden models", _draw_hidden_models);
  ADD_TOGGLE_NS (view_menu, "Creature spawns", _draw_creature_spawns);

  ADD_TOGGLE_NS(view_menu, "Game Mode", _game_mode_camera);

  auto debug_menu (view_menu->addMenu ("Debug"));
  ADD_TOGGLE_NS (debug_menu, "Occlusion boxes", _draw_occlusion_boxes);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Tools"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show Node Editor", "Shift+N", _show_node_editor);
  ADD_TOGGLE_NS (view_menu, "Creature browser", _show_creature_browser);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Minimap"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show", Qt::Key_M, _show_minimap_window);


  ADD_TOGGLE_NS(view_menu, "Show ADT borders", _show_minimap_borders);

  ADD_TOGGLE_NS(view_menu, "Show light zones", _show_minimap_skies);

  // [2026-09-06] Persist this toggle ONLY on the user's own menu click. Its change handler used to
  // write every value that reached the property -- so any programmatic set(false) (a harness
  // "clean capture", a mode switch, a startup gate) got PERSISTED and spawns came up off on the
  // next launch. Find the action ADD_TOGGLE_NS just created and mark user clicks.
  for (QAction* a : view_menu->actions())
    if (a->text() == "Creature spawns")
      connect(a, &QAction::triggered, this, [this](bool) { _creature_spawns_user_click = true; });
  connect(&_draw_creature_spawns, &Noggit::BoolToggleProperty::changed, [this](bool enabled)
  {
    _world->setDrawCreatureSpawns(enabled);
    // [2026-09-06 DIAG] name every write of this key -- the user's setting keeps flipping to false
    // with no user action, and this lambda is the only persisting writer in the tree.
    LogError << "[SPAWN-TOGGLE] view/creature_spawns <- " << (enabled ? "true" : "false") << std::endl;
    bool const user_click = _creature_spawns_user_click;
    _creature_spawns_user_click = false;
    if (!enabled && !user_click)
    {
      // Programmatic OFF: honour it for this session's drawing, but NEVER persist it. The user's
      // saved choice survives whatever a harness or mode switch does at runtime.
      LogError << "[SPAWN-TOGGLE] programmatic off -- NOT persisted" << std::endl;
      return;
    }
    _settings->setValue("view/creature_spawns", enabled);

    if (enabled)
    {
      refreshCreatureSpawnOverlay(false);
    }
    else
    {
      updateDatabaseStatus();
    }
  });

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Windows"));
  view_menu->addSeparator();

  auto hide_widgets = [=]
  {

    QWidget *widget_list[] =
      {
        _texture_browser_dock,
        _texture_picker_dock,
        _detail_infos_dock,
        _creature_actions_overlay,
        _creature_browser_dock,
        _creature_editor_dock,
        _creature_model_picker_dock,
        _keybindings,
        _minimap_dock,
        objectEditor->modelImport,
        objectEditor->rotationEditor,
        objectEditor->helper_models_widget,
        _texture_palette_small,
        _object_palette_dock,
        _asset_browser_dock,
        _overlay_widget,
        _tool_panel_dock

      };

    if (_main_window->displayed_widgets.empty())
    {
      for (auto widget : widget_list)
        if (widget && widget->isVisible())
        {
          _main_window->displayed_widgets.emplace(widget);
          widget->hide();
        }

    }
    else
    {
      for (auto widget : _main_window->displayed_widgets)
        if (widget)
          widget->show();

      _main_window->displayed_widgets.clear();
    }


    _main_window->statusBar()->setVisible(ui_hidden);
    _toolbar->setVisible(ui_hidden);
    _view_toolbar->setVisible(ui_hidden);

    ui_hidden = !ui_hidden;

    setToolPropertyWidgetVisibility(terrainMode);

  };

  ADD_ACTION(view_menu, "Toggle UI", Qt::Key_Tab, hide_widgets);

  ADD_TOGGLE (view_menu, "Detail infos", Qt::Key_F8, _show_detail_info_window);

  ADD_TOGGLE (view_menu, "Texture Browser", Qt::Key_X, _show_texture_palette_window);

  ADD_TOGGLE_NS(view_menu, "Texture palette", _show_texture_palette_small_window);

  addHotkey( Qt::Key_H
    , MOD_none
    , [this] { _show_texture_palette_small_window.toggle(); }
    , [this] { return terrainMode == editing_mode::paint; }
  );

  ADD_ACTION (view_menu, "Increase time speed", Qt::Key_N, [this] { mTimespeed += 90.0f; });
  ADD_ACTION (view_menu, "Decrease time speed", Qt::Key_B, [this] { mTimespeed = std::max (0.0f, mTimespeed - 90.0f); });
  ADD_ACTION (view_menu, "Pause time", Qt::Key_J, [this] { mTimespeed = 0.0f; });
  ADD_ACTION (view_menu, "Invert mouse", "I", [this] { mousedir *= -1.f; });
  ADD_ACTION (view_menu, "Decrease camera speed", Qt::Key_O, [this] { _camera.move_speed *= 0.5f; });
  ADD_ACTION (view_menu, "Increase camera speed", Qt::Key_P, [this] { _camera.move_speed *= 2.0f; });
  ADD_ACTION ( view_menu
  , "Turn camera around 180°"
  , "Shift+R"
  , [this]
               {
                 _camera.add_to_yaw(math::degrees(180.f));
                 _camera_moved_since_last_draw = true;
               }
  );

  ADD_ACTION ( view_menu
  , "Toggle tile mode"
  , Qt::Key_U
  , [this]
               {
                 if (NOGGIT_CUR_ACTION)
                   return;

                 if (_display_mode == display_mode::in_2D)
                 {
                   _display_mode = display_mode::in_3D;
                   set_editing_mode (saveterrainMode);
                 }
                 else
                 {
                   _display_mode = display_mode::in_2D;
                   saveterrainMode = terrainMode;
                   set_editing_mode (editing_mode::paint);
                 }
               }
  );

}

void MapView::setupHelpMenu()
{
  auto help_menu (_main_window->_menuBar->addMenu ("Help"));
  connect (this, &QObject::destroyed, help_menu, &QObject::deleteLater);

  ADD_TOGGLE (help_menu, "Key Bindings", "Ctrl+F1", _show_keybindings_window);

#if defined(_WIN32) || defined(WIN32)
  ADD_ACTION_NS ( help_menu
                , "WoW Modding Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Dnrztg7dCZ"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
  ADD_ACTION_NS ( help_menu
                , "Noggit Red Repository"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://gitlab.com/prophecy-rp/noggit-red/-/tree/noggit-shadowlands?ref_type=heads"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );

  ADD_ACTION_NS ( help_menu
                , "Noggit Red Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Tk2TpN8CaF"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
#endif

}

void MapView::refreshCreatureSpawnOverlay(bool force_reload)
{
  _world->setDrawCreatureSpawns(_draw_creature_spawns.get());

  if (!_draw_creature_spawns.get())
  {
    setSelectedCreatureSpawn(std::nullopt, false);
    rebuildCreatureBrowserList(false);
    updateDatabaseStatus();
    return;
  }

  if (force_reload || !_world->hasCreatureSpawnsLoaded())
  {
    _world->reloadCreatureSpawns();
    // Spawns (and gameobjects, loaded alongside) were re-read -> drop the zone-filter caches.
    _creature_zone_cache.clear();
    _gameobject_zone_cache.clear();
    _main_window->statusBar()->showMessage(QString::fromStdString(_world->creatureSpawnStatus()), 5000);
  }

  rebuildCreatureBrowserList(true);
  updateDatabaseStatus();
}

void MapView::updateDatabaseStatus()
{
#ifdef USE_MYSQL_UID_STORAGE
  QStringList parts;

  if (Noggit::mysqlSetting("enabled", false).toBool())
  {
    if (Noggit::mysqlSetting(Noggit::Ssh::Keys::enabled(), false).toBool())
    {
      auto const tunnel = Noggit::Ssh::TunnelConfig::fromProjectSettings();
      parts << QString("MySQL %1:%2 via SSH %3 (%4)")
                   .arg(tunnel.remote_db_host)
                   .arg(tunnel.remote_db_port)
                   .arg(tunnel.ssh_host)
                   .arg(Noggit::Ssh::SshTunnelManager::stateName(Noggit::Ssh::SshTunnelManager::instance().state()));
    }
    else
    {
      parts << QString("MySQL %1:%2")
                   .arg(Noggit::mysqlSetting("server", "127.0.0.1").toString())
                   .arg(Noggit::mysqlSetting("port", 3306).toString());
    }
  }

  if (_draw_creature_spawns.get())
  {
    parts << QString::fromStdString(_world->creatureSpawnStatus());

    std::size_t nearby_spawn_count = 0;
    float nearest_spawn_distance = std::numeric_limits<float>::max();
    constexpr float nearby_spawn_radius = 200.0f;

    for (auto const& spawn : _world->creatureSpawns())
    {
      float horizontal_distance = glm::distance(glm::vec2(_camera.position.x, _camera.position.z),
                                                glm::vec2(spawn.pos.x, spawn.pos.z));
      nearest_spawn_distance = std::min(nearest_spawn_distance, horizontal_distance);

      if (horizontal_distance <= nearby_spawn_radius)
      {
        ++nearby_spawn_count;
      }
    }

    if (!_world->creatureSpawns().empty())
    {
      parts << QString("nearby(%1): %2")
                   .arg(nearby_spawn_radius, 0, 'f', 0)
                   .arg(nearby_spawn_count);
      parts << QString("nearest: %1")
                   .arg(nearest_spawn_distance, 0, 'f', 1);
    }

    auto dirty_count = _world->dirtyCreatureSpawnCount();
    if (dirty_count > 0)
    {
      parts << QString("pending edits: %1").arg(dirty_count);
    }
  }

  _status_database->setText(parts.join(" | "));
#else
  _status_database->clear();
#endif

  updateCreatureBrowserStatus();
}

// One row's label. Shared by the full rebuild and by refreshCreatureBrowserItems(), so the two can
// never drift apart in what a row says.
QString MapView::creature_spawn_item_text(World::CreatureSpawnOverlay const& spawn)
{
  QString prefix;
  if (spawn.selected)
  {
    prefix += "[selected] ";
  }
  if (spawn.pending_create)
  {
    prefix += "[new] ";
  }
  if (spawn.dirty)
  {
    prefix += "[pending] ";
  }

  auto const name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
  return QString("%1%2 [entry %3] guid %4").arg(prefix).arg(name).arg(spawn.entry).arg(spawn.guid);
}

// Retext ONLY the rows whose spawns changed, instead of clearing and repopulating the whole list.
// Dragging flips a spawn's `dirty` flag, which changes its "[pending]" prefix -- but a full
// rebuildCreatureBrowserList() allocates a QListWidgetItem for every spawn in the world, which is a
// ~1s freeze on release. This walks the existing rows and touches the handful that moved.
void MapView::refreshCreatureBrowserItems(std::vector<std::uint32_t> const& guids)
{
  if (!_creature_list_widget || guids.empty())
  {
    return;
  }

  QSignalBlocker blocker(_creature_list_widget);
  for (int i = 0; i < _creature_list_widget->count(); ++i)
  {
    auto* item = _creature_list_widget->item(i);
    if (!item)
    {
      continue;
    }

    auto const guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
    if (std::find(guids.begin(), guids.end(), guid) == guids.end())
    {
      continue;
    }

    if (auto const* spawn = _world->findCreatureSpawn(guid))
    {
      item->setText(creature_spawn_item_text(*spawn));
    }
  }
}

// A SELECTION change does not change the list's CONTENT -- only which row is highlighted. Doing that
// through rebuildCreatureBrowserList() cleared the QListWidget and allocated a fresh item for every
// spawn in the world (plus a getZoneId() per spawn when the zone filter is on), which is the ~1s
// freeze on every click in the creature tool -- including a click on empty ground, which only
// deselects. Move the highlight instead; the rebuild is for content changes (create/delete/filter).
void MapView::highlightCreatureBrowserSelection()
{
  if (!_creature_list_widget)
  {
    return;
  }

  QSignalBlocker blocker(_creature_list_widget);

  if (_selected_creature_spawn_guid)
  {
    for (int i = 0; i < _creature_list_widget->count(); ++i)
    {
      auto* item = _creature_list_widget->item(i);
      if (item && static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong())
                    == *_selected_creature_spawn_guid)
      {
        _creature_list_widget->setCurrentItem(item);
        updateCreatureBrowserStatus();
        return;
      }
    }
  }

  // Nothing selected, or the selected spawn is filtered out of the list by the search / zone / type
  // filters -- a rebuild would not have highlighted anything either.
  _creature_list_widget->setCurrentRow(-1);
  updateCreatureBrowserStatus();
}

void MapView::highlightGameObjectBrowserSelection()
{
  if (!_gameobject_list_widget)
  {
    return;
  }

  QSignalBlocker blocker(_gameobject_list_widget);

  if (_selected_gameobject_spawn_guid)
  {
    for (int i = 0; i < _gameobject_list_widget->count(); ++i)
    {
      auto* item = _gameobject_list_widget->item(i);
      if (item && static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong())
                    == *_selected_gameobject_spawn_guid)
      {
        _gameobject_list_widget->setCurrentItem(item);
        updateGameObjectBrowserStatus();
        return;
      }
    }
  }

  _gameobject_list_widget->setCurrentRow(-1);
  updateGameObjectBrowserStatus();
}

void MapView::scheduleCreatureBrowserRebuild()
{
  if (_creature_browser_rebuild_pending)
  {
    return;
  }
  _creature_browser_rebuild_pending = true;
  QTimer::singleShot(250, this, [this]
  {
    _creature_browser_rebuild_pending = false;
    rebuildCreatureBrowserList(true);
  });
}

void MapView::scheduleGameObjectBrowserRebuild()
{
  if (_gameobject_browser_rebuild_pending)
  {
    return;
  }
  _gameobject_browser_rebuild_pending = true;
  QTimer::singleShot(250, this, [this]
  {
    _gameobject_browser_rebuild_pending = false;
    rebuildGameObjectBrowserList(true);
  });
}

void MapView::rebuildCreatureBrowserList(bool preserve_selection)
{
  if (!_creature_list_widget)
  {
    return;
  }

  auto selected_guid = preserve_selection ? _selected_creature_spawn_guid : std::optional<std::uint32_t>();
  auto search_text = _creature_search_field ? _creature_search_field->text().trimmed() : QString();
  auto search_lower = search_text.toLower();

  // "Zone only" filter: only keep spawns whose zone matches the camera's current zone. Disabled if
  // the camera's own zone can't be resolved (e.g. standing outside any loaded area).
  unsigned int camera_zone = (_creature_zone_filter && _creature_zone_filter->isChecked())
    ? _world->getZoneId(_camera.position) : 0u;
  bool const zone_only = camera_zone != 0u && camera_zone != static_cast<unsigned int>(-1);

  QSignalBlocker blocker(_creature_list_widget);
  _creature_list_widget->clear();

  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.pending_delete) // marked for deletion -> hidden from the list
    {
      continue;
    }
    if (zone_only)
    {
      unsigned int zone;
      auto cit = _creature_zone_cache.find(spawn.guid);
      if (cit != _creature_zone_cache.end())
      {
        zone = cit->second;
      }
      else
      {
        zone = _world->getZoneId(spawn.pos);
        if (zone != 0u && zone != static_cast<unsigned int>(-1))
          _creature_zone_cache[spawn.guid] = zone; // cache only resolved zones (retry unloaded tiles)
      }
      if (zone != camera_zone)
        continue;
    }
    auto name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
    auto entry_text = QString::number(spawn.entry);
    auto guid_text = QString::number(spawn.guid);

    if (!search_lower.isEmpty()
        && !name.toLower().contains(search_lower)
        && !entry_text.contains(search_lower)
        && !guid_text.contains(search_lower))
    {
      continue;
    }

    // Type/rank filter (same semantics as the NPC model picker), looked up by spawn entry. Spawns whose
    // creature_template info hasn't loaded yet are left visible (don't hide what we can't classify).
    {
      auto fit = _creature_template_filter_info.find(spawn.entry);
      if (fit != _creature_template_filter_info.end())
      {
        auto const& info = fit->second;
        auto const type_data = _creature_browser_type_filter ? _creature_browser_type_filter->currentData() : QVariant();
        if (type_data.isValid() && info.creature_type != static_cast<std::uint32_t>(type_data.toULongLong()))
          continue;
        if (_creature_browser_elite && _creature_browser_elite->isChecked() && !(info.rank == 1u || info.rank == 2u))
          continue;
        if (_creature_browser_boss && _creature_browser_boss->isChecked() && info.rank < 3u && (info.type_flags & 0x4u) == 0u)
          continue;
        if (_creature_browser_civilian && _creature_browser_civilian->isChecked() && (info.flags_extra & 0x2u) == 0u && (info.type_flags & 0x80u) == 0u)
          continue;
        if (_creature_browser_trainer && _creature_browser_trainer->isChecked() && (info.npc_flags & 0x10u) == 0u)
          continue;
      }
    }

    auto* item = new QListWidgetItem(creature_spawn_item_text(spawn), _creature_list_widget);
    item->setData(Qt::UserRole, static_cast<qulonglong>(spawn.guid));
    item->setData(Qt::UserRole + 1, static_cast<int>(_world->getMapID()));
    item->setData(Qt::UserRole + 6, true);

    if (selected_guid && *selected_guid == spawn.guid)
    {
      _creature_list_widget->setCurrentItem(item);
    }
  }

  updateCreatureBrowserStatus();
}

void MapView::updateCreatureBrowserStatus(QString const& override_text)
{
  if (!_creature_browser_status)
  {
    return;
  }

  if (!override_text.isEmpty())
  {
    _creature_browser_status->setText(override_text);
    return;
  }

  QStringList parts;
  parts << QString("Current map spawns: %1").arg(_world->creatureSpawnCount());
  parts << QString("models: %1").arg(_world->creatureSpawnModelCount());

  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count > 0)
  {
    parts << QString("pending edits: %1").arg(dirty_count);
  }

  auto selected_count = selectedCreatureSpawnCount();
  if (selected_count > 1)
  {
    parts << QString("selected: %1").arg(selected_count);
  }
  else if (_selected_creature_spawn_guid)
  {
    parts << QString("selected guid: %1").arg(*_selected_creature_spawn_guid);
  }

  _creature_browser_status->setText(parts.join(" | "));
}

std::size_t MapView::selectedCreatureSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_world->creatureSpawns().begin(),
                                                _world->creatureSpawns().end(),
    [](World::CreatureSpawnOverlay const& spawn)
    {
      return spawn.selected;
    }));
}

void MapView::setSelectedCreatureSpawn(std::optional<std::uint32_t> guid, bool update_browser)
{
  _selected_creature_spawn_guid = guid;

  // Collect the spawns whose selected flag actually flips: their row text carries a "[selected]"
  // prefix, so those are the only rows that need new text.
  std::vector<std::uint32_t> retext;
  for (auto& spawn : _world->creatureSpawns())
  {
    bool const now_selected = guid && spawn.guid == *guid;
    if (spawn.selected != now_selected)
    {
      retext.push_back(spawn.guid);
    }
    spawn.selected = now_selected;
  }

  // Populate the "Edit/New Creature" form from the selected spawn (empty = New). So clicking an existing
  // creature fills its guid/entry/display; deselecting clears back to the new-spawn state.
  if (_creature_spawn_guid_field)
  {
    World::CreatureSpawnOverlay const* sp = guid ? _world->findCreatureSpawn(*guid) : nullptr;
    if (sp)
    {
      _creature_spawn_guid_field->setText(QString::number(sp->guid));
      _creature_spawn_entry_field->setText(QString::number(sp->entry));
      _creature_spawn_display_field->setText(QString::number(sp->display_id));
    }
    else
    {
      _creature_spawn_guid_field->clear();
      _creature_spawn_entry_field->clear();
      _creature_spawn_display_field->clear();
    }
    // Extended columns follow the selection too (defaults when nothing is selected = New-spawn state).
    populateCreatureExtForm(sp ? sp->guid : 0);
  }

  if (update_browser)
  {
    // Selection only -- no content change. Retext the handful of rows whose prefix flipped and move
    // the highlight, instead of clearing and repopulating the whole list (the ~1s click freeze).
    refreshCreatureBrowserItems(retext);
    highlightCreatureBrowserSelection();
  }
  else
  {
    updateCreatureBrowserStatus();
  }

  refreshCreatureEditorKnobs();
}

void MapView::addCreatureSpawnToSelection(std::uint32_t guid, bool update_browser)
{
  bool found = false;
  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.guid == guid)
    {
      spawn.selected = true;
      found = true;
    }
  }

  if (!found)
  {
    return;
  }

  _selected_creature_spawn_guid = guid;

  if (update_browser)
  {
    // Only this row's "[selected]" prefix changed -- see setSelectedCreatureSpawn.
    refreshCreatureBrowserItems({guid});
    highlightCreatureBrowserSelection();
  }
  else
  {
    updateCreatureBrowserStatus();
  }

  refreshCreatureEditorKnobs();
}

void MapView::selectCreatureSpawnsInArea(QRect const& rect, bool add_to_selection)
{
  if (!_world->hasCreatureSpawnsLoaded())
  {
    return;
  }

  QRect const normalized_rect = rect.normalized();
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  if (!add_to_selection)
  {
    for (auto& spawn : _world->creatureSpawns())
    {
      spawn.selected = false;
    }
  }

  std::optional<std::uint32_t> primary_guid = add_to_selection ? _selected_creature_spawn_guid : std::optional<std::uint32_t>();

  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.event_suppressed) // hidden by the Seasonal Events filter -> not box-selectable
    {
      continue;
    }
    glm::vec3 const screen = glm::project(spawn.pos, mv, proj, vp);
    if (screen.z < 0.0f || screen.z > 1.0f)
    {
      continue;
    }

    QPoint const point(static_cast<int>(std::lround(screen.x)),
                       static_cast<int>(std::lround(float(height()) - screen.y)));
    if (!normalized_rect.contains(point))
    {
      continue;
    }

    spawn.selected = true;
    if (!primary_guid)
    {
      primary_guid = spawn.guid;
    }
  }

  if (primary_guid)
  {
    _selected_creature_spawn_guid = primary_guid;
  }
  else if (!add_to_selection)
  {
    _selected_creature_spawn_guid = std::optional<std::uint32_t>();
  }

  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
  updateDatabaseStatus();
}

void MapView::setCreatureInfoPanelVisible(bool visible)
{
  if (!_creature_info_panel)
  {
    // Free-floating tool window (movable by its own title bar), initially aligned with the left
    // secondary toolbar so it doesn't cover the side icon strip.
    _creature_info_panel = new Noggit::Ui::CreatureInfoPanel(this);
    _creature_info_panel->setWindowFlags(Qt::Tool);
    _creature_info_panel->setWindowTitle("Quick Facts");

    QWidget* bar = getLeftSecondaryToolbar();
    QPoint const initial = (bar && bar->isVisible())
      ? bar->mapToGlobal(QPoint(0, bar->height() + 6))
      : mapToGlobal(QPoint(60, 110));
    _creature_info_panel->move(initial);
  }

  if (!visible)
  {
    _creature_info_panel->hide();
    return;
  }

  if (_selected_creature_spawn_guid)
  {
    if (auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      _creature_info_panel->setCreature(spawn->entry);
    }
  }
  else
  {
    _creature_info_panel->clearCreature();
  }
  _creature_info_panel->show();
  _creature_info_panel->raise();
}

void MapView::refreshCreatureEditorKnobs()
{
  if (!_spawn_edit_x)
    return;

  // Keep the wander-radius ring centered on the (possibly just-dragged) selected spawn.
  updateWanderVisualization();

  // Keep the Quick Facts dropdown following the selection.
  if (_creature_info_panel && _creature_info_panel->isVisible())
  {
    if (_selected_creature_spawn_guid)
    {
      if (auto const* info_spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
      {
        _creature_info_panel->setCreature(info_spawn->entry);
      }
    }
    else
    {
      _creature_info_panel->clearCreature();
    }
  }

  auto disable_all = [this]() {
    _creature_editor_info->setText("No spawn selected");
    _creature_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
    for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
      w->setEnabled(false);
  };

  if (!_selected_creature_spawn_guid)
  {
    disable_all();
    return;
  }

  auto selected_count = selectedCreatureSpawnCount();
  if (selected_count > 1)
  {
    _creature_editor_info->setText(QString("%1 creature spawns selected\nPrimary GUID: %2")
                                     .arg(selected_count)
                                     .arg(*_selected_creature_spawn_guid));
    _creature_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");
    for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
      w->setEnabled(false);
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
  if (!spawn)
  {
    disable_all();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  _creature_editor_info->setText(
    QString("%1\nGUID: %2  Entry: %3").arg(name).arg(spawn->guid).arg(spawn->entry));
  _creature_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");

  for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
    w->blockSignals(true);

  _spawn_edit_x->setValue(static_cast<double>(spawn->pos.x));
  _spawn_edit_y->setValue(static_cast<double>(spawn->pos.y));
  _spawn_edit_z->setValue(static_cast<double>(spawn->pos.z));
  _spawn_edit_orientation->setValue(static_cast<double>(spawn->orientation));

  for (auto* w : {_spawn_edit_x, _spawn_edit_y, _spawn_edit_z, _spawn_edit_orientation})
  {
    w->setEnabled(true);
    w->blockSignals(false);
  }
}

void MapView::setHoveredCreatureSpawn(std::optional<std::uint32_t> guid)
{
  if (_hovered_creature_spawn_guid == guid)
  {
    return;
  }

  if (_hovered_creature_spawn_guid)
  {
    if (auto* previous = _world->findCreatureSpawn(*_hovered_creature_spawn_guid))
    {
      previous->hovered = false;
    }
  }

  _hovered_creature_spawn_guid = guid;

  if (_hovered_creature_spawn_guid)
  {
    if (auto* current = _world->findCreatureSpawn(*_hovered_creature_spawn_guid))
    {
      current->hovered = true;
    }
  }

  _needs_redraw = true;
}

std::optional<std::uint32_t> MapView::findCreatureSpawnAtCursor()
{
  if (!_world->hasCreatureSpawnsLoaded())
    return std::optional<std::uint32_t>();

  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  // Build the cursor ray in WORLD space (window Y is bottom-up for unProject; Qt mouse Y is top-down).
  float const wx = float(_last_mouse_pos.x());
  float const wy = float(height()) - float(_last_mouse_pos.y());
  glm::vec3 const ray_near = glm::unProject(glm::vec3(wx, wy, 0.0f), mv, proj, vp);
  glm::vec3 const ray_far  = glm::unProject(glm::vec3(wx, wy, 1.0f), mv, proj, vp);
  glm::vec3 const ray_dir  = ray_far - ray_near;

  // 1) Prefer a hit on the actual 3D MODEL MESH: click the creature's body, not just its ground disc.
  //    ModelInstance::intersect ray-casts the animated triangles in the instance's own transform; we
  //    take the spawn whose mesh the cursor ray strikes nearest the camera.
  {
    math::ray const world_ray(ray_near, ray_dir);
    int const base_animtime = static_cast<int>(_world->model_animtime);
    float best_dist = std::numeric_limits<float>::max();
    std::optional<std::uint32_t> best_guid;
    for (auto& spawn : _world->creatureSpawns())
    {
      if (spawn.pending_delete || spawn.event_suppressed || !spawn.model_instance.has_value())
        continue;
      auto& inst = *spawn.model_instance;
      if (!inst.model.get() || !inst.model->finishedLoading() || inst.model->loading_failed())
        continue;
      // Cheap world-AABB gate before the per-triangle intersect: with thousands of spawns the
      // full intersect per spawn per MOUSE MOVE was the 5-9ms hover stutter ([UIPROF]); the ray
      // misses virtually every box, so almost no spawn reaches the expensive path.
      if (!world_ray.intersect_bounds(inst.extents[0], inst.extents[1]))
        continue;
      selection_result hits;
      inst.intersect(mv, world_ray, &hits, base_animtime + spawn.animation_time_offset);
      for (auto const& h : hits)
      {
        if (h.first < best_dist)
        {
          best_dist = h.first;
          best_guid = spawn.guid;
        }
      }
    }
    if (best_guid)
      return best_guid;
  }

  // 2) Fallback: the ground selection-disc, so clicking the drawn circle still selects (handy when the
  //    body is off-screen or behind terrain -- e.g. a flyer whose disc sits on the ground below it).
  //    Same world centre (spawn.pos) and radius as the rendered marker; rank by relative distance from
  //    the centre (0 = centre, <1 = inside) so overlapping discs resolve to the most-centred one.
  float best_rel = 1.0f;
  std::optional<std::uint32_t> best_guid;
  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.pending_delete || spawn.event_suppressed)
      continue;
    float const ring_radius = spawn.selectionRingWorldRadius();
    if (std::abs(ray_dir.y) < 1e-6f)
      continue;
    float const t = (spawn.pos.y - ray_near.y) / ray_dir.y;
    if (t < 0.0f)
      continue;
    glm::vec3 const hit = ray_near + ray_dir * t;
    float const d = glm::length(glm::vec2(hit.x - spawn.pos.x, hit.z - spawn.pos.z));
    float const rel = d / ring_radius;
    if (rel < 1.0f && rel < best_rel)
    {
      best_rel = rel;
      best_guid = spawn.guid;
    }
  }
  return best_guid;
}

void MapView::updateCreatureSpawnHover(QPoint const& global_pos)
{
  if (terrainMode != editing_mode::creature || _dragging_creature_spawn || rightMouse)
  {
    setHoveredCreatureSpawn(std::optional<std::uint32_t>());
    QToolTip::hideText();
    return;
  }

  // Hover re-pick at most ~30x/s: mouse-move events can arrive far faster, and each pick walks the
  // spawn list. Keeping the previous hover for a frame or two is imperceptible.
  static QElapsedTimer hover_throttle;
  if (hover_throttle.isValid() && hover_throttle.elapsed() < 33)
  {
    return;
  }
  hover_throttle.restart();

  auto hovered_guid = findCreatureSpawnAtCursor();
  setHoveredCreatureSpawn(hovered_guid);

  if (!hovered_guid)
  {
    QToolTip::hideText();
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*hovered_guid);
  if (!spawn)
  {
    QToolTip::hideText();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  QToolTip::showText(global_pos, QString("%1\nGUID: %2\nEntry: %3")
                               .arg(name)
                               .arg(spawn->guid)
                               .arg(spawn->entry), this);
}

bool MapView::tryStartCreatureSpawnDrag()
{
  bool const creature_editor_mode = terrainMode == editing_mode::creature;
  bool const legacy_object_mode = terrainMode == editing_mode::object && _draw_creature_spawns.get();
  if ((!creature_editor_mode && !legacy_object_mode) || !_world->hasCreatureSpawnsLoaded())
  {
    return false;
  }

  std::optional<std::uint32_t> best_guid = findCreatureSpawnAtCursor();

  if (!best_guid)
  {
    return false;
  }

  auto const* clicked_spawn = _world->findCreatureSpawn(*best_guid);
  if (!clicked_spawn)
  {
    return false;
  }

  if (!clicked_spawn->selected || selectedCreatureSpawnCount() <= 1)
  {
    // update_browser=false: rebuildCreatureBrowserList() repopulates a QListWidget over every spawn in
    // the world, which is a ~1s hitch at the moment you grab something. The drag end already rebuilds
    // it (mouseReleaseEvent), so the list syncs there instead of stalling the grab.
    setSelectedCreatureSpawn(best_guid, /*update_browser*/ false);
  }

  _creature_drag_anchor_pos = _cursor_pos;
  _creature_drag_initial_positions.clear();
  for (auto const& spawn : _world->creatureSpawns())
  {
    if (spawn.selected)
    {
      _creature_drag_initial_positions.push_back({spawn.guid, spawn.pos, spawn.orientation});
    }
  }

  _dragging_creature_spawn = true;
  _main_window->statusBar()->showMessage(QString("Dragging %1 creature spawn(s). Release mouse, then use Export SQL.")
                                           .arg(_creature_drag_initial_positions.size()), 4000);
  return true;
}

void MapView::translateSelectedCreatureSpawns(glm::vec3 const& delta)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::CreatureSpawnOverlay& spawn, glm::vec3 const& pos)
  {
    spawn.pos = pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f
               || spawn.extDirty();

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  bool moved_any = false;
  SpawnUndoOp undo_op;
  undo_op.kind = SpawnUndoOp::Kind::Move;
  for (auto& spawn : _world->creatureSpawns())
  {
    if (!spawn.selected)
    {
      continue;
    }

    undo_op.moves.push_back({spawn.guid, spawn.pos, spawn.orientation});
    apply_position(spawn, spawn.pos + delta);
    moved_any = true;
  }

  if (!moved_any)
  {
    if (auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      undo_op.moves.push_back({spawn->guid, spawn->pos, spawn->orientation});
      apply_position(*spawn, spawn->pos + delta);
      moved_any = true;
    }
  }

  if (!moved_any)
  {
    return;
  }

  std::vector<std::uint32_t> moved_guids;
  moved_guids.reserve(undo_op.moves.size());
  for (auto const& m : undo_op.moves)
  {
    moved_guids.push_back(m.guid);
  }
  pushCreatureUndoOp(std::move(undo_op));
  updateDatabaseStatus();
  refreshCreatureBrowserItems(moved_guids); // only these rows' [pending] prefix can change
  refreshCreatureEditorKnobs();
}

void MapView::translateSelectedGameObjectSpawns(glm::vec3 const& delta)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::GameObjectSpawnOverlay& spawn, glm::vec3 const& pos)
  {
    spawn.pos = pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  bool moved_any = false;
  SpawnUndoOp undo_op;
  undo_op.kind = SpawnUndoOp::Kind::Move;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (!spawn.selected)
    {
      continue;
    }
    undo_op.moves.push_back({spawn.guid, spawn.pos, spawn.orientation});
    apply_position(spawn, spawn.pos + delta);
    moved_any = true;
  }

  if (!moved_any)
  {
    if (auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
    {
      undo_op.moves.push_back({spawn->guid, spawn->pos, spawn->orientation});
      apply_position(*spawn, spawn->pos + delta);
      moved_any = true;
    }
  }

  if (!moved_any)
  {
    return;
  }

  std::vector<std::uint32_t> moved_guids;
  moved_guids.reserve(undo_op.moves.size());
  for (auto const& m : undo_op.moves)
  {
    moved_guids.push_back(m.guid);
  }
  pushGameObjectUndoOp(std::move(undo_op));
  updateGameObjectBrowserStatus();
  refreshGameObjectBrowserItems(moved_guids);
  refreshGameObjectEditorKnobs();
}

void MapView::deleteSelectedCreatureSpawns()
{
  std::vector<std::uint32_t> deleted;

  auto mark = [&](World::CreatureSpawnOverlay& spawn)
  {
    if (spawn.pending_delete)
    {
      return;
    }
    spawn.pending_delete = true; // hidden from view/browser/picking; exported as DELETE
    spawn.dirty = true;          // count it as a pending change
    spawn.selected = false;
    spawn.hovered = false;
    deleted.push_back(spawn.guid);
  };

  for (auto& spawn : _world->creatureSpawns())
  {
    if (spawn.selected)
    {
      mark(spawn);
    }
  }

  if (deleted.empty() && _selected_creature_spawn_guid)
  {
    if (auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      mark(*spawn);
    }
  }

  if (deleted.empty())
  {
    return;
  }

  {
    SpawnUndoOp op;
    op.kind = SpawnUndoOp::Kind::Delete;
    op.guids = deleted;
    pushCreatureUndoOp(std::move(op));
  }
  setSelectedCreatureSpawn(std::nullopt, false);
  updateDatabaseStatus();
  scheduleCreatureBrowserRebuild();
  refreshCreatureEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Marked %1 creature spawn(s) for deletion (Ctrl+Z to undo)").arg(deleted.size()), 5000);
}

void MapView::pushCreatureUndoOp(SpawnUndoOp op)
{
  op.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

  // Spinbox coalescing: a held arrow / typed value fires valueChanged per step; merge a burst of
  // single-spawn spinbox moves into ONE op that restores the state before the burst began.
  if (op.from_spinbox && op.kind == SpawnUndoOp::Kind::Move && op.moves.size() == 1
      && !_creature_undo_ops.empty())
  {
    auto& top = _creature_undo_ops.back();
    if (top.from_spinbox && top.kind == SpawnUndoOp::Kind::Move && top.moves.size() == 1
        && top.moves[0].guid == op.moves[0].guid
        && op.timestamp_ms - top.timestamp_ms < 1500)
    {
      top.timestamp_ms = op.timestamp_ms; // keep the burst alive, keep the ORIGINAL before-state
      return;
    }
  }

  _creature_undo_ops.push_back(std::move(op));
  if (_creature_undo_ops.size() > 200)
  {
    _creature_undo_ops.erase(_creature_undo_ops.begin());
  }
}

void MapView::pushGameObjectUndoOp(SpawnUndoOp op)
{
  op.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

  if (op.from_spinbox && op.kind == SpawnUndoOp::Kind::Move && op.moves.size() == 1
      && !_gameobject_undo_ops.empty())
  {
    auto& top = _gameobject_undo_ops.back();
    if (top.from_spinbox && top.kind == SpawnUndoOp::Kind::Move && top.moves.size() == 1
        && top.moves[0].guid == op.moves[0].guid
        && op.timestamp_ms - top.timestamp_ms < 1500)
    {
      top.timestamp_ms = op.timestamp_ms;
      return;
    }
  }

  _gameobject_undo_ops.push_back(std::move(op));
  if (_gameobject_undo_ops.size() > 200)
  {
    _gameobject_undo_ops.erase(_gameobject_undo_ops.begin());
  }
}

bool MapView::undoCreatureEdit()
{
  if (_creature_undo_ops.empty())
  {
    return false;
  }

  auto const op = _creature_undo_ops.back();
  _creature_undo_ops.pop_back();

  switch (op.kind)
  {
    case SpawnUndoOp::Kind::Move:
    {
      std::vector<std::uint32_t> touched;
      for (auto const& state : op.moves)
      {
        auto* spawn = _world->findCreatureSpawn(state.guid);
        if (!spawn)
        {
          continue;
        }
        spawn->pos = state.pos;
        spawn->orientation = state.orientation;
        spawn->dirty = spawn->pending_create
                    || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                    || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f
                    || spawn->extDirty();
        if (spawn->model_instance)
        {
          spawn->model_instance->pos = spawn->pos;
          spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
          spawn->model_instance->recalcExtents();
        }
        touched.push_back(state.guid);
      }
      updateDatabaseStatus();
      refreshCreatureBrowserItems(touched);
      refreshCreatureEditorKnobs();
      _needs_redraw = true;
      _main_window->statusBar()->showMessage(
        QString("Undid move of %1 creature spawn(s)").arg(touched.size()), 5000);
      return true;
    }

    case SpawnUndoOp::Kind::Delete:
    {
      for (auto guid : op.guids)
      {
        if (auto* spawn = _world->findCreatureSpawn(guid))
        {
          spawn->pending_delete = false;
          // Keep it dirty only if it still has real edits (or is a never-saved spawn).
          spawn->dirty = spawn->pending_create
                      || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                      || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f
                      || spawn->extDirty();
        }
      }
      updateDatabaseStatus();
      scheduleCreatureBrowserRebuild();
      refreshCreatureEditorKnobs();
      _main_window->statusBar()->showMessage(
        QString("Restored %1 deleted creature spawn(s)").arg(op.guids.size()), 5000);
      return true;
    }

    case SpawnUndoOp::Kind::Create:
    {
      std::size_t removed = 0;
      std::size_t remarked = 0;
      try
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());

        auto& spawns = _world->creatureSpawns();
        for (auto it = spawns.begin(); it != spawns.end(); )
        {
          if (std::find(op.guids.begin(), op.guids.end(), it->guid) == op.guids.end())
          {
            ++it;
            continue;
          }
          if (_selected_creature_spawn_guid && *_selected_creature_spawn_guid == it->guid)
          {
            _selected_creature_spawn_guid = std::nullopt;
          }
          if (_hovered_creature_spawn_guid && *_hovered_creature_spawn_guid == it->guid)
          {
            _hovered_creature_spawn_guid = std::nullopt;
          }
          if (it->pending_create)
          {
            it = spawns.erase(it); // never reached the DB -> vanish outright
            ++removed;
          }
          else
          {
            // Already exported/applied: removing it now is a DELETE like any other.
            it->pending_delete = true;
            it->dirty = true;
            it->selected = false;
            ++remarked;
            ++it;
          }
        }
      }
      catch (...)
      {
        // GL context issues must not lose the editor state; report and carry on.
      }
      updateDatabaseStatus();
      scheduleCreatureBrowserRebuild();
      refreshCreatureEditorKnobs();
      _needs_redraw = true;
      _main_window->statusBar()->showMessage(
        QString("Undid creature spawn addition (%1 removed%2)")
          .arg(removed)
          .arg(remarked ? QString(", %1 marked for deletion").arg(remarked) : QString()), 5000);
      return true;
    }
  }

  return false;
}

// Turn every spawn currently being dragged by `degrees` about its own centre. Used by the wheel while
// a drag is in progress. Deliberately does NOT rebuild the browser list or the editor knobs -- those
// are the expensive calls that used to stall the drag; the orientation spinbox is nudged directly and
// the full resync happens on mouse-up like the position does.
void MapView::rotateDraggedSpawns(float degrees)
{
  auto const wrap360 = [](float v)
  {
    v = std::fmod(v, 360.0f);
    return v < 0.0f ? v + 360.0f : v;
  };

  if (_dragging_creature_spawn)
  {
    for (auto const& drag_state : _creature_drag_initial_positions)
    {
      auto* spawn = _world->findCreatureSpawn(drag_state.guid);
      if (!spawn)
      {
        continue;
      }

      spawn->orientation = wrap360(spawn->orientation + degrees);
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f
                  || spawn->extDirty();

      if (spawn->model_instance)
      {
        spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->model_instance->recalcExtents();
      }
      if (spawn->mount_instance)
      {
        spawn->mount_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->mount_instance->recalcExtents();
      }
    }

    if (_spawn_edit_orientation && _selected_creature_spawn_guid)
    {
      if (auto const* sel = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
      {
        QSignalBlocker blocker(_spawn_edit_orientation);
        _spawn_edit_orientation->setValue(static_cast<double>(sel->orientation));
      }
    }
    return;
  }

  if (_dragging_gameobject_spawn)
  {
    for (auto const& drag_state : _gameobject_drag_initial_positions)
    {
      auto* spawn = _world->findGameObjectSpawn(drag_state.guid);
      if (!spawn)
      {
        continue;
      }

      spawn->orientation = wrap360(spawn->orientation + degrees);
      spawn->dirty = spawn->pending_create
                  || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                  || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;

      if (spawn->model_instance)
      {
        spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->model_instance->recalcExtents();
      }
      if (spawn->wmo_instance)
      {
        spawn->wmo_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
        spawn->wmo_instance->recalcExtents();
      }
    }

    if (_go_spawn_edit_orientation && _selected_gameobject_spawn_guid)
    {
      if (auto const* sel = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
      {
        QSignalBlocker blocker(_go_spawn_edit_orientation);
        _go_spawn_edit_orientation->setValue(static_cast<double>(sel->orientation));
      }
    }
  }
}

void MapView::updateSelectedCreatureSpawnPosition(glm::vec3 const& pos)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::CreatureSpawnOverlay& spawn, glm::vec3 const& new_pos)
  {
    spawn.pos = new_pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f
               || spawn.extDirty();

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  if (_dragging_creature_spawn && _creature_drag_anchor_pos && !_creature_drag_initial_positions.empty())
  {
    if (_creature_drag_initial_positions.size() == 1)
    {
      // Single spawn: snap it directly to the ground point under the cursor so it follows the mouse
      // exactly (instead of keeping the click offset).
      if (auto* spawn = _world->findCreatureSpawn(_creature_drag_initial_positions[0].guid))
      {
        apply_position(*spawn, pos);
      }
    }
    else
    {
      // Multi-select: move the whole group together by the cursor delta to preserve their layout.
      glm::vec3 const delta = pos - *_creature_drag_anchor_pos;
      for (auto const& drag_state : _creature_drag_initial_positions)
      {
        if (auto* spawn = _world->findCreatureSpawn(drag_state.guid))
        {
          apply_position(*spawn, drag_state.pos + delta);
        }
      }
    }
  }
  else
  {
    auto* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
    if (!spawn)
    {
      return;
    }

    apply_position(*spawn, pos);
  }

  // PERF: this runs on EVERY mouse-move while dragging, and rebuildCreatureBrowserList() clears and
  // repopulates a QListWidget over every creature spawn in the world -- that is what dropped the drag
  // to ~1 fps. All three calls are pure UI sync with no bearing on the spawn's position, so defer them
  // to the end of the drag (mouseReleaseEvent), where they run exactly once.
  if (_dragging_creature_spawn)
  {
    return;
  }

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
}

void MapView::showSelectedCreatureSpawnMenu(QPoint const& global_pos)
{
  if (!_selected_creature_spawn_guid)
  {
    return;
  }

  auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid);
  if (!spawn)
  {
    return;
  }

  QMenu menu(this);
  menu.addAction(QString("NPC: %1").arg(QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name)))->setEnabled(false);
  menu.addAction(QString("Unique ID: %1").arg(spawn->guid))->setEnabled(false);
  menu.addAction(QString("Entry: %1").arg(spawn->entry))->setEnabled(false);
  menu.addAction(QString("Display ID: %1").arg(spawn->display_id))->setEnabled(false);
  menu.addAction(QString("Position: %1, %2, %3")
                   .arg(spawn->pos.x, 0, 'f', 2)
                   .arg(spawn->pos.y, 0, 'f', 2)
                   .arg(spawn->pos.z, 0, 'f', 2))->setEnabled(false);
  menu.addSeparator();
  auto* jump_action = menu.addAction("Center camera here");
  auto* save_action = menu.addAction("Save pending creature changes");
  auto* chosen = menu.exec(global_pos);

  if (chosen == jump_action)
  {
    focus_camera_on_target(spawn->pos);
  }
  else if (chosen == save_action)
  {
    saveDirtyCreatureSpawns();
  }
}

void MapView::discardPendingCreatureSpawns()
{
  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No creature spawn changes to discard", 4000);
    updateCreatureBrowserStatus();
    return;
  }

  _selected_creature_spawn_guid = std::nullopt;
  _hovered_creature_spawn_guid = std::nullopt;
  _dragging_creature_spawn = false;
  _creature_drag_anchor_pos = std::nullopt;
  _creature_drag_initial_positions.clear();
  _creature_undo_ops.clear();

  std::size_t removed_new = 0;
  std::size_t reverted_existing = 0;

  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    auto& spawns = _world->creatureSpawns();
    for (auto& spawn : spawns)
    {
      spawn.selected = false;
      spawn.hovered = false;

      if (!spawn.dirty || spawn.pending_create)
      {
        continue;
      }

      spawn.pos = spawn.original_pos;
      spawn.orientation = spawn.original_orientation;
      spawn.ext = spawn.original_ext; // revert edited extended columns too
      spawn.dirty = false;
      spawn.pending_delete = false; // restore any spawn that was marked for deletion
      if (spawn.model_instance)
      {
        spawn.model_instance->pos = spawn.pos;
        spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
        spawn.model_instance->recalcExtents();
      }
      ++reverted_existing;
    }

    auto pending_begin = std::remove_if(spawns.begin(), spawns.end(),
      [&removed_new](World::CreatureSpawnOverlay const& spawn)
      {
        if (!spawn.pending_create)
        {
          return false;
        }

        ++removed_new;
        return true;
      });
    spawns.erase(pending_begin, spawns.end());
  }
  catch (std::exception const& ex)
  {
    _main_window->statusBar()->showMessage(QString("Failed to discard creature spawn changes: %1").arg(ex.what()), 7000);
    updateCreatureBrowserStatus();
    return;
  }
  catch (...)
  {
    _main_window->statusBar()->showMessage("Failed to discard creature spawn changes: unknown error", 7000);
    updateCreatureBrowserStatus();
    return;
  }

  rebuildCreatureBrowserList(false);
  refreshCreatureEditorKnobs();
  updateDatabaseStatus();
  _needs_redraw = true;

  _main_window->statusBar()->showMessage(
    QString("Discarded %1 pending creature spawn(s), reverted %2 edited spawn(s)")
      .arg(removed_new)
      .arg(reverted_existing),
    5000);
}

void MapView::updateCreatureSpawnFlagsButton()
{
  if (!_creature_spawn_flags_button)
  {
    return;
  }
  QStringList names;
  static std::pair<std::uint32_t, char const*> const short_names[] = {
    {0x01, "Active"}, {0x02, "Disabled"}, {0x04, "RandRespawn"}, {0x08, "DynRespawn"},
    {0x10, "DynElite"}, {0x20, "EvadeHome"}, {0x40, "Invisible"}, {0x80, "Dead"}, {0x100, "NoDynRespawn"}};
  for (auto const& [bit, name] : short_names)
  {
    if (_creature_spawn_flags_value & bit)
    {
      names << name;
    }
  }
  QString text = QString::number(_creature_spawn_flags_value);
  if (!names.isEmpty())
  {
    text += " (" + names.join(", ") + ")";
  }
  _creature_spawn_flags_button->setText(text);
}

void MapView::updateWanderVisualization()
{
  if (!_world)
  {
    return;
  }
  // Ring only while the wander field is focused AND an existing/pending spawn gives it a center.
  if (_creature_wander_field_focused && _selected_creature_spawn_guid && _creature_spawn_wander)
  {
    if (auto const* spawn = _world->findCreatureSpawn(*_selected_creature_spawn_guid))
    {
      _world->wander_viz = World::WanderViz{spawn->pos, static_cast<float>(_creature_spawn_wander->value())};
      _needs_redraw = true;
      return;
    }
  }
  if (_world->wander_viz)
  {
    _world->wander_viz.reset();
    _needs_redraw = true;
  }
}

void MapView::applyCreatureExtFormToSpawn(std::uint32_t guid)
{
  auto* spawn = _world ? _world->findCreatureSpawn(guid) : nullptr;
  if (!spawn)
  {
    return;
  }
  auto parse_id = [](QLineEdit const* f) -> std::uint32_t
  {
    if (!f) return 0;
    bool ok = false;
    auto const v = f->text().trimmed().toUInt(&ok);
    return ok ? v : 0;
  };
  spawn->ext.id2 = parse_id(_creature_spawn_id2_field);
  spawn->ext.id3 = parse_id(_creature_spawn_id3_field);
  spawn->ext.id4 = parse_id(_creature_spawn_id4_field);
  spawn->ext.spawntimesecs_min = static_cast<std::uint32_t>(_creature_spawn_respawn_min->value());
  spawn->ext.spawntimesecs_max = static_cast<std::uint32_t>(_creature_spawn_respawn_max->value());
  spawn->ext.wander_distance = static_cast<float>(_creature_spawn_wander->value());
  spawn->ext.health_percent = static_cast<std::uint32_t>(_creature_spawn_health_pct->value());
  spawn->ext.mana_percent = static_cast<std::uint32_t>(_creature_spawn_mana_pct->value());
  spawn->ext.movement_type = static_cast<std::uint32_t>(std::max(0, _creature_spawn_movement->currentIndex()));
  spawn->ext.spawn_flags = _creature_spawn_flags_value;
  spawn->ext.visibility_mod = static_cast<float>(_creature_spawn_visibility->value());
  spawn->ext.spawn_mask = _creature_spawn_spawnmask_value;
  spawn->ext.phase_mask = static_cast<std::uint32_t>(_creature_spawn_phasemask->value());
  spawn->dirty = spawn->pending_create
              || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
              || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f
              || spawn->extDirty();
  updateDatabaseStatus();
  updateWanderVisualization();
}

void MapView::populateCreatureExtForm(std::uint32_t guid)
{
  if (!_creature_spawn_wander) // creature UI not built
  {
    return;
  }
  World::CreatureSpawnOverlay const* spawn = (guid && _world) ? _world->findCreatureSpawn(guid) : nullptr;
  World::CreatureSpawnOverlay::ExtFields const defaults;
  auto const& ext = spawn ? spawn->ext : defaults;

  _creature_spawn_form_updating = true;
  auto set_id = [](QLineEdit* f, std::uint32_t v)
  {
    if (f) f->setText(v ? QString::number(v) : QString());
  };
  set_id(_creature_spawn_id2_field, ext.id2);
  set_id(_creature_spawn_id3_field, ext.id3);
  set_id(_creature_spawn_id4_field, ext.id4);
  _creature_spawn_respawn_min->setValue(static_cast<int>(ext.spawntimesecs_min));
  _creature_spawn_respawn_max->setValue(static_cast<int>(ext.spawntimesecs_max));
  _creature_spawn_wander->setValue(ext.wander_distance);
  _creature_spawn_health_pct->setValue(static_cast<int>(ext.health_percent));
  _creature_spawn_mana_pct->setValue(static_cast<int>(ext.mana_percent));
  _creature_spawn_movement->setCurrentIndex(static_cast<int>(std::min(ext.movement_type, 2u)));
  _creature_spawn_flags_value = ext.spawn_flags;
  if (_creature_spawn_flags_menu)
  {
    for (auto* act : _creature_spawn_flags_menu->actions())
    {
      act->setChecked((ext.spawn_flags & act->data().toUInt()) != 0);
    }
  }
  updateCreatureSpawnFlagsButton();
  _creature_spawn_visibility->setValue(ext.visibility_mod);
  _creature_spawn_spawnmask_value = ext.spawn_mask ? ext.spawn_mask : 1u;
  if (_creature_spawn_spawnmask_menu)
  {
    for (auto* act : _creature_spawn_spawnmask_menu->actions())
    {
      act->setChecked((_creature_spawn_spawnmask_value & act->data().toUInt()) != 0);
    }
    _creature_spawn_spawnmask_button->setText(QString::number(_creature_spawn_spawnmask_value)
      + (_creature_spawn_spawnmask_value == 1 ? " (Normal)" : ""));
  }
  _creature_spawn_phasemask->setValue(static_cast<int>(std::clamp(ext.phase_mask, 1u, 0xFFFFu)));

  // HIDE rows whose column this DB schema simply does not have (their value could never be loaded
  // or exported) -- the panel only shows what the connected database can actually store. The
  // resolved names come from the spawn load.
  if (_world)
  {
    auto const& cols = _world->creatureSpawnColumns();
    auto set_row_visible = [](QFormLayout* form, QWidget* field, bool on)
    {
      if (!form || !field)
      {
        return;
      }
      if (QWidget* label = form->labelForField(field))
      {
        label->setVisible(on);
      }
      field->setVisible(on);
    };
    // Alt ids: Turtle id2..id4 columns, AzerothCore id2/id3, OR the cmangos creature_spawn_entry
    // table (exported as DELETE+INSERT rows there with creature.id = 0). Slots without a column
    // (e.g. id4 on AzerothCore) disable individually inside the visible row.
    bool const alt_ids_ok = !cols.id2_col.empty() || cols.spawn_entry_table;
    set_row_visible(_creature_spawn_form_left, _creature_spawn_alt_row, alt_ids_ok);
    _creature_spawn_id2_field->setEnabled(cols.spawn_entry_table || !cols.id2_col.empty());
    _creature_spawn_id3_field->setEnabled(cols.spawn_entry_table || !cols.id3_col.empty());
    _creature_spawn_id4_field->setEnabled(cols.spawn_entry_table || !cols.id4_col.empty());
    set_row_visible(_creature_spawn_form_left, _creature_spawn_spawnmask_button, !cols.spawn_mask_col.empty());
    set_row_visible(_creature_spawn_form_left, _creature_spawn_phasemask, !cols.phase_mask_col.empty());
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_respawn_row, !cols.respawn_min_col.empty());
    // One spawntimesecs column (AzerothCore/old mangos) -> a single Respawn spinner.
    _creature_spawn_respawn_max->setVisible(cols.respawn_min_col.empty()
                                            || cols.respawn_min_col != cols.respawn_max_col);
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_wander, !cols.wander_col.empty());
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_pct_row,
                    !cols.health_percent_col.empty() || !cols.mana_percent_col.empty());
    _creature_spawn_health_pct->setEnabled(!cols.health_percent_col.empty());
    _creature_spawn_mana_pct->setEnabled(!cols.mana_percent_col.empty());
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_movement, !cols.movement_col.empty());
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_flags_button, !cols.spawn_flags_col.empty());
    set_row_visible(_creature_spawn_form_ext, _creature_spawn_visibility, !cols.visibility_col.empty());

    // Health/mana semantics per schema: Turtle percents (0..100 %) vs AzerothCore ABSOLUTE
    // curhealth/curmana (0 = spawn at full; Creature.cpp:1755 uses GetMaxHealth() when 0).
    // Ranges must be (re)set BEFORE the values -- setValue clamps to the current range.
    if (cols.health_mana_absolute)
    {
      _creature_spawn_health_pct->setRange(0, 100000000);
      _creature_spawn_mana_pct->setRange(0, 100000000);
      _creature_spawn_health_pct->setSuffix(" hp");
      _creature_spawn_mana_pct->setSuffix(" mp");
      _creature_spawn_health_pct->setSpecialValueText("full");
      _creature_spawn_mana_pct->setSpecialValueText("full");
      _creature_spawn_health_pct->setToolTip("curhealth: ABSOLUTE health the spawn starts with (AzerothCore).\n0 = spawn at full health.");
      _creature_spawn_mana_pct->setToolTip("curmana: ABSOLUTE mana the spawn starts with (AzerothCore).\n0 = spawn at full mana.");
    }
    else
    {
      _creature_spawn_health_pct->setRange(0, 100);
      _creature_spawn_mana_pct->setRange(0, 100);
      _creature_spawn_health_pct->setSuffix(" %hp");
      _creature_spawn_mana_pct->setSuffix(" %mp");
      _creature_spawn_health_pct->setSpecialValueText(QString());
      _creature_spawn_mana_pct->setSpecialValueText(QString());
      _creature_spawn_health_pct->setToolTip("health_percent: the spawn starts at this percent of its maximum health.");
      _creature_spawn_mana_pct->setToolTip("mana_percent: the spawn starts at this percent of its maximum mana.");
    }
    _creature_spawn_health_pct->setValue(static_cast<int>(spawn ? ext.health_percent
                                                                : (cols.health_mana_absolute ? 0u : 100u)));
    _creature_spawn_mana_pct->setValue(static_cast<int>(spawn ? ext.mana_percent
                                                              : (cols.health_mana_absolute ? 0u : 100u)));
  }
  _creature_spawn_form_updating = false;
  updateWanderVisualization();
}

QString MapView::buildDirtyCreatureSpawnSql(bool rebase_state)
{
  QString sql;
  QTextStream stream(&sql);
  stream << "-- Noggit creature spawn update export\n";
  stream << "-- Map ID: " << _world->getMapID() << "\n";
  stream << "-- Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
  stream << "\n";

  for (auto& spawn : _world->creatureSpawns())
  {
    if (!spawn.dirty)
    {
      continue;
    }

    if (spawn.pending_delete)
    {
      // A spawn created this session and then deleted never reached the DB -> nothing to export.
      if (spawn.pending_create)
      {
        continue;
      }
      stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
             << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name) << "\n";
      stream << "DELETE FROM creature WHERE guid=" << spawn.guid << ";\n\n";
      continue;
    }

    auto server_pos = client_to_server_creature_position(spawn.pos, _world->mapIndex.hasAGlobalWMO());
    auto server_orientation = client_to_server_creature_orientation(spawn.orientation);
    stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
           << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name)
           << "\n";

    auto const& cols = _world->creatureSpawnColumns();
    auto fnum = [](float v) { return QString::number(v, 'f', 6); };

    // cmangos spawn-entry mode: alt ids live in creature_spawn_entry (guid, entry) with
    // creature.id = 0 (random pick per spawn, mangos-wotlk ObjectMgr::LoadCreatures).
    bool const spawn_entry_mode = cols.id2_col.empty() && cols.spawn_entry_table;
    QStringList desired_entries; // primary + alts, deduped, for spawn-entry mode
    if (spawn_entry_mode)
    {
      QList<std::uint32_t> ids{spawn.entry, spawn.ext.id2, spawn.ext.id3, spawn.ext.id4};
      for (auto const id : ids)
      {
        if (id && !desired_entries.contains(QString::number(id)))
        {
          desired_entries << QString::number(id);
        }
      }
    }
    bool const alts_changed = spawn.ext.id2 != spawn.original_ext.id2
                           || spawn.ext.id3 != spawn.original_ext.id3
                           || spawn.ext.id4 != spawn.original_ext.id4;

    if (spawn.pending_create)
    {
      stream << "-- Preview display ID: " << spawn.display_id << "\n";
      // Full-schema INSERT: every extended column the connected DB actually has rides along
      // (tortoise-wow: id2..id4/spawntimesecs/wander/health/mana/movement/spawn_flags/visibility_mod;
      // plain mangos schemas map to spawntimesecs/spawndist/MovementType; absent columns are omitted
      // and take the table's own DEFAULT).
      QStringList names;
      QStringList values;
      names << "guid" << QString::fromStdString(cols.entry_col) << "map"
            << "position_x" << "position_y" << "position_z" << "orientation";
      // spawn-entry mode with alt ids: creature.id must be 0 so the server rolls from the
      // creature_spawn_entry rows emitted after the INSERT.
      bool const use_spawn_entry_rows = spawn_entry_mode && desired_entries.size() > 1;
      values << QString::number(spawn.guid)
             << (use_spawn_entry_rows ? QString("0") : QString::number(spawn.entry))
             << QString::number(_world->getMapID())
             << fnum(server_pos.x) << fnum(server_pos.y) << fnum(server_pos.z)
             << fnum(server_orientation);
      auto add_col = [&](std::string const& col, QString const& value)
      {
        if (!col.empty())
        {
          names << QString::fromStdString(col);
          values << value;
        }
      };
      add_col(cols.id2_col, QString::number(spawn.ext.id2));
      add_col(cols.id3_col, QString::number(spawn.ext.id3));
      add_col(cols.id4_col, QString::number(spawn.ext.id4));
      add_col(cols.respawn_min_col, QString::number(spawn.ext.spawntimesecs_min));
      if (cols.respawn_max_col != cols.respawn_min_col) // one spawntimesecs column on plain mangos
      {
        add_col(cols.respawn_max_col, QString::number(spawn.ext.spawntimesecs_max));
      }
      add_col(cols.wander_col, fnum(spawn.ext.wander_distance));
      add_col(cols.health_percent_col, QString::number(spawn.ext.health_percent));
      add_col(cols.mana_percent_col, QString::number(spawn.ext.mana_percent));
      add_col(cols.movement_col, QString::number(spawn.ext.movement_type));
      add_col(cols.spawn_flags_col, QString::number(spawn.ext.spawn_flags));
      add_col(cols.visibility_col, fnum(spawn.ext.visibility_mod));
      add_col(cols.spawn_mask_col, QString::number(spawn.ext.spawn_mask ? spawn.ext.spawn_mask : 1u));
      add_col(cols.phase_mask_col, QString::number(spawn.ext.phase_mask ? spawn.ext.phase_mask : 1u));
      stream << "INSERT INTO creature (" << names.join(", ") << ")\n"
             << "VALUES (" << values.join(", ") << ");\n";
      if (use_spawn_entry_rows)
      {
        QStringList rows;
        for (auto const& e : desired_entries)
        {
          rows << QString("(%1, %2)").arg(spawn.guid).arg(e);
        }
        stream << "INSERT INTO creature_spawn_entry (guid, entry) VALUES " << rows.join(", ") << ";\n";
      }
      stream << "\n";
    }
    else
    {
      // UPDATE only what actually changed: the position block when moved/rotated, plus each edited
      // extended column (compared against its DB-loaded original).
      QStringList sets;
      bool const moved = glm::distance(spawn.pos, spawn.original_pos) > 0.01f
                      || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;
      if (moved)
      {
        sets << ("position_x = " + fnum(server_pos.x))
             << ("position_y = " + fnum(server_pos.y))
             << ("position_z = " + fnum(server_pos.z))
             << ("orientation = " + fnum(server_orientation));
      }
      auto set_col = [&](std::string const& col, bool changed, QString const& value)
      {
        if (!col.empty() && changed)
        {
          sets << (QString::fromStdString(col) + " = " + value);
        }
      };
      auto const& e = spawn.ext;
      auto const& o = spawn.original_ext;
      set_col(cols.id2_col, e.id2 != o.id2, QString::number(e.id2));
      set_col(cols.id3_col, e.id3 != o.id3, QString::number(e.id3));
      set_col(cols.id4_col, e.id4 != o.id4, QString::number(e.id4));
      // cmangos spawn-entry mode: rewrite the creature_spawn_entry rows and point creature.id at
      // 0 (multi-entry random pick) or back at the single entry.
      if (spawn_entry_mode && alts_changed)
      {
        stream << "DELETE FROM creature_spawn_entry WHERE guid = " << spawn.guid << ";\n";
        if (desired_entries.size() > 1)
        {
          QStringList rows;
          for (auto const& de : desired_entries)
          {
            rows << QString("(%1, %2)").arg(spawn.guid).arg(de);
          }
          stream << "INSERT INTO creature_spawn_entry (guid, entry) VALUES " << rows.join(", ") << ";\n";
          sets << (QString::fromStdString(cols.entry_col) + " = 0");
        }
        else
        {
          sets << (QString::fromStdString(cols.entry_col) + " = " + QString::number(spawn.entry));
        }
      }
      set_col(cols.respawn_min_col, e.spawntimesecs_min != o.spawntimesecs_min,
              QString::number(e.spawntimesecs_min));
      if (cols.respawn_max_col != cols.respawn_min_col)
      {
        set_col(cols.respawn_max_col, e.spawntimesecs_max != o.spawntimesecs_max,
                QString::number(e.spawntimesecs_max));
      }
      set_col(cols.wander_col, e.wander_distance != o.wander_distance, fnum(e.wander_distance));
      set_col(cols.health_percent_col, e.health_percent != o.health_percent,
              QString::number(e.health_percent));
      set_col(cols.mana_percent_col, e.mana_percent != o.mana_percent,
              QString::number(e.mana_percent));
      set_col(cols.movement_col, e.movement_type != o.movement_type,
              QString::number(e.movement_type));
      set_col(cols.spawn_flags_col, e.spawn_flags != o.spawn_flags,
              QString::number(e.spawn_flags));
      set_col(cols.visibility_col, e.visibility_mod != o.visibility_mod, fnum(e.visibility_mod));
      set_col(cols.spawn_mask_col, e.spawn_mask != o.spawn_mask,
              QString::number(e.spawn_mask ? e.spawn_mask : 1u));
      set_col(cols.phase_mask_col, e.phase_mask != o.phase_mask,
              QString::number(e.phase_mask ? e.phase_mask : 1u));
      if (!sets.isEmpty())
      {
        stream << "UPDATE creature\n"
               << "SET " << sets.join(",\n    ") << "\n"
               << "WHERE guid = " << spawn.guid << ";\n\n";
      }
    }

    if (rebase_state)
    {
      spawn.original_pos = spawn.pos;
      spawn.original_orientation = spawn.orientation;
      spawn.original_ext = spawn.ext;
      spawn.pending_create = false;
      spawn.dirty = false;
    }
  }

  return sql;
}

void MapView::saveDirtyCreatureSpawns()
{
  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No creature spawn changes to export", 4000);
    updateCreatureBrowserStatus();
    return;
  }

  QDir project_dir(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));
  QString export_dir_path = project_dir.filePath("sql_exports/creature_spawns");
  QDir export_dir(export_dir_path);
  if (!export_dir.exists() && !project_dir.mkpath("sql_exports/creature_spawns"))
  {
    auto message = QString("Failed to create creature SQL export folder: %1").arg(export_dir_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateCreatureBrowserStatus(message);
    return;
  }

  QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  QString file_name = QString("creature_updates_map%1_%2.sql").arg(_world->getMapID()).arg(timestamp);
  QString file_path = export_dir.filePath(file_name);

  QFile output(file_path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    auto message = QString("Failed to write creature SQL export: %1").arg(file_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateCreatureBrowserStatus(message);
    return;
  }

  QTextStream stream(&output);
  stream.setCodec("UTF-8");
  stream << buildDirtyCreatureSpawnSql(/*rebase_state*/ true);
  output.close();

  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  _main_window->statusBar()->showMessage(QString("Creature spawn SQL exported: %1").arg(file_path), 7000);
}

void MapView::jumpToCreatureListItem(QListWidgetItem* item)
{
  if (!item)
  {
    return;
  }

  auto guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
  auto map_id = item->data(Qt::UserRole + 1).toInt();
  auto is_current_map_item = item->data(Qt::UserRole + 6).toBool();

  if (is_current_map_item)
  {
    setSelectedCreatureSpawn(guid, true);
    auto const* spawn = _world->findCreatureSpawn(guid);
    if (spawn)
    {
      focus_camera_on_target(spawn->pos);
    }
    return;
  }

  glm::vec3 target_pos = server_to_client_creature_position(item->data(Qt::UserRole + 2).toFloat(),
                                                            item->data(Qt::UserRole + 3).toFloat(),
                                                            item->data(Qt::UserRole + 4).toFloat(),
                                                            _world->mapIndex.hasAGlobalWMO());
  _main_window->jumpToMapPosition(map_id, target_pos, math::degrees(30.f), math::degrees(90.f), false);
}

// ---------------------------------------------------------------------------
// GameObject tool (mirrors the creature tool above; no model picker / new-spawn creation).
// ---------------------------------------------------------------------------

// Gameobject counterpart of refreshCreatureBrowserItems -- see that function for why.
void MapView::refreshGameObjectBrowserItems(std::vector<std::uint32_t> const& guids)
{
  if (!_gameobject_list_widget || guids.empty())
  {
    return;
  }

  QSignalBlocker blocker(_gameobject_list_widget);
  for (int i = 0; i < _gameobject_list_widget->count(); ++i)
  {
    auto* item = _gameobject_list_widget->item(i);
    if (!item)
    {
      continue;
    }

    auto const guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
    if (std::find(guids.begin(), guids.end(), guid) == guids.end())
    {
      continue;
    }

    if (auto const* spawn = _world->findGameObjectSpawn(guid))
    {
      QString prefix;
      if (spawn->selected)
      {
        prefix += "[selected] ";
      }
      if (spawn->dirty)
      {
        prefix += "[pending] ";
      }

      auto const name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
      item->setText(QString("%1%2 [entry %3] guid %4")
                      .arg(prefix).arg(name).arg(spawn->entry).arg(spawn->guid));
    }
  }
}

void MapView::rebuildGameObjectBrowserList(bool preserve_selection)
{
  if (!_gameobject_list_widget)
  {
    return;
  }

  auto selected_guid = preserve_selection ? _selected_gameobject_spawn_guid : std::optional<std::uint32_t>();
  auto search_text = _gameobject_search_field ? _gameobject_search_field->text().trimmed() : QString();
  auto search_lower = search_text.toLower();

  unsigned int camera_zone = (_gameobject_zone_filter && _gameobject_zone_filter->isChecked())
    ? _world->getZoneId(_camera.position) : 0u;
  bool const zone_only = camera_zone != 0u && camera_zone != static_cast<unsigned int>(-1);

  QSignalBlocker blocker(_gameobject_list_widget);
  _gameobject_list_widget->clear();

  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.pending_delete) // marked for deletion -> hidden from the list
    {
      continue;
    }
    if (zone_only)
    {
      unsigned int zone;
      auto cit = _gameobject_zone_cache.find(spawn.guid);
      if (cit != _gameobject_zone_cache.end())
      {
        zone = cit->second;
      }
      else
      {
        zone = _world->getZoneId(spawn.pos);
        if (zone != 0u && zone != static_cast<unsigned int>(-1))
          _gameobject_zone_cache[spawn.guid] = zone;
      }
      if (zone != camera_zone)
        continue;
    }
    auto name = QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name);
    auto entry_text = QString::number(spawn.entry);
    auto guid_text = QString::number(spawn.guid);

    if (!search_lower.isEmpty()
        && !name.toLower().contains(search_lower)
        && !entry_text.contains(search_lower)
        && !guid_text.contains(search_lower))
    {
      continue;
    }

    // Type filter (mirrors the gameobject model picker), looked up by spawn entry. Spawns whose template
    // type hasn't loaded yet are left visible.
    if (_gameobject_browser_type_filter)
    {
      auto const type_data = _gameobject_browser_type_filter->currentData();
      if (type_data.isValid())
      {
        auto fit = _gameobject_template_filter_type.find(spawn.entry);
        if (fit != _gameobject_template_filter_type.end()
            && fit->second != static_cast<std::uint32_t>(type_data.toULongLong()))
        {
          continue;
        }
      }
    }

    QString prefix;
    if (spawn.selected)
    {
      prefix += "[selected] ";
    }
    if (spawn.dirty)
    {
      prefix += "[pending] ";
    }

    auto* item = new QListWidgetItem(QString("%1%2 [entry %3] guid %4")
                                       .arg(prefix)
                                       .arg(name)
                                       .arg(spawn.entry)
                                       .arg(spawn.guid),
                                     _gameobject_list_widget);
    item->setData(Qt::UserRole, static_cast<qulonglong>(spawn.guid));
    item->setData(Qt::UserRole + 1, static_cast<int>(_world->getMapID()));
    item->setData(Qt::UserRole + 6, true);

    if (selected_guid && *selected_guid == spawn.guid)
    {
      _gameobject_list_widget->setCurrentItem(item);
    }
  }

  updateGameObjectBrowserStatus();
}

void MapView::updateGameObjectBrowserStatus(QString const& override_text)
{
  if (!_gameobject_browser_status)
  {
    return;
  }

  if (!override_text.isEmpty())
  {
    _gameobject_browser_status->setText(override_text);
    return;
  }

  QStringList parts;
  parts << QString("Current map spawns: %1").arg(_world->gameObjectSpawnCount());
  parts << QString("models: %1").arg(_world->gameObjectSpawnModelCount());

  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count > 0)
  {
    parts << QString("pending edits: %1").arg(dirty_count);
  }

  auto selected_count = selectedGameObjectSpawnCount();
  if (selected_count > 1)
  {
    parts << QString("selected: %1").arg(selected_count);
  }
  else if (_selected_gameobject_spawn_guid)
  {
    parts << QString("selected guid: %1").arg(*_selected_gameobject_spawn_guid);
  }

  _gameobject_browser_status->setText(parts.join(" | "));
}

std::size_t MapView::selectedGameObjectSpawnCount() const
{
  return static_cast<std::size_t>(std::count_if(_world->gameObjectSpawns().begin(),
                                                _world->gameObjectSpawns().end(),
    [](World::GameObjectSpawnOverlay const& spawn)
    {
      return spawn.selected;
    }));
}

void MapView::setSelectedGameObjectSpawn(std::optional<std::uint32_t> guid, bool update_browser)
{
  _selected_gameobject_spawn_guid = guid;

  // Same as the creature path: only the rows whose selected flag flips need new text.
  std::vector<std::uint32_t> retext;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    bool const now_selected = guid && spawn.guid == *guid;
    if (spawn.selected != now_selected)
    {
      retext.push_back(spawn.guid);
    }
    spawn.selected = now_selected;
  }

  // Populate the "Edit/New GameObject" form from the selected spawn (empty = New).
  if (_gameobject_spawn_guid_field)
  {
    World::GameObjectSpawnOverlay const* sp = guid ? _world->findGameObjectSpawn(*guid) : nullptr;
    if (sp)
    {
      _gameobject_spawn_guid_field->setText(QString::number(sp->guid));
      _gameobject_spawn_entry_field->setText(QString::number(sp->entry));
      _gameobject_spawn_display_field->setText(QString::number(sp->display_id));
    }
    else
    {
      _gameobject_spawn_guid_field->clear();
      _gameobject_spawn_entry_field->clear();
      _gameobject_spawn_display_field->clear();
    }
  }

  if (update_browser)
  {
    refreshGameObjectBrowserItems(retext);
    highlightGameObjectBrowserSelection();
  }
  else
  {
    updateGameObjectBrowserStatus();
  }

  refreshGameObjectEditorKnobs();
}

void MapView::addGameObjectSpawnToSelection(std::uint32_t guid, bool update_browser)
{
  bool found = false;
  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.guid == guid)
    {
      spawn.selected = true;
      found = true;
    }
  }

  if (!found)
  {
    return;
  }

  _selected_gameobject_spawn_guid = guid;

  if (update_browser)
  {
    refreshGameObjectBrowserItems({guid});
    highlightGameObjectBrowserSelection();
  }
  else
  {
    updateGameObjectBrowserStatus();
  }

  refreshGameObjectEditorKnobs();
}

void MapView::selectGameObjectSpawnsInArea(QRect const& rect, bool add_to_selection)
{
  _world->ensureGameObjectSpawnsLoaded();

  QRect const normalized_rect = rect.normalized();
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  if (!add_to_selection)
  {
    for (auto& spawn : _world->gameObjectSpawns())
    {
      spawn.selected = false;
    }
  }

  std::optional<std::uint32_t> primary_guid = add_to_selection ? _selected_gameobject_spawn_guid : std::optional<std::uint32_t>();

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.event_suppressed) // hidden by the Seasonal Events filter -> not box-selectable
    {
      continue;
    }
    glm::vec3 const screen = glm::project(spawn.pos, mv, proj, vp);
    if (screen.z < 0.0f || screen.z > 1.0f)
    {
      continue;
    }

    QPoint const point(static_cast<int>(std::lround(screen.x)),
                       static_cast<int>(std::lround(float(height()) - screen.y)));
    if (!normalized_rect.contains(point))
    {
      continue;
    }

    spawn.selected = true;
    if (!primary_guid)
    {
      primary_guid = spawn.guid;
    }
  }

  if (primary_guid)
  {
    _selected_gameobject_spawn_guid = primary_guid;
  }
  else if (!add_to_selection)
  {
    _selected_gameobject_spawn_guid = std::optional<std::uint32_t>();
  }

  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
  updateGameObjectBrowserStatus();
}

void MapView::refreshGameObjectEditorKnobs()
{
  if (!_go_spawn_edit_x)
    return;

  auto disable_all = [this]() {
    _gameobject_editor_info->setText("No spawn selected");
    _gameobject_editor_info->setStyleSheet("font-style: italic; color: #888; padding: 2px 0;");
    for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
      w->setEnabled(false);
  };

  if (!_selected_gameobject_spawn_guid)
  {
    disable_all();
    return;
  }

  auto selected_count = selectedGameObjectSpawnCount();
  if (selected_count > 1)
  {
    _gameobject_editor_info->setText(QString("%1 gameobject spawns selected\nPrimary GUID: %2")
                                       .arg(selected_count)
                                       .arg(*_selected_gameobject_spawn_guid));
    _gameobject_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");
    for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
      w->setEnabled(false);
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
  if (!spawn)
  {
    disable_all();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  _gameobject_editor_info->setText(
    QString("%1\nGUID: %2  Entry: %3").arg(name).arg(spawn->guid).arg(spawn->entry));
  _gameobject_editor_info->setStyleSheet("font-weight: bold; padding: 2px 0;");

  for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
    w->blockSignals(true);

  _go_spawn_edit_x->setValue(static_cast<double>(spawn->pos.x));
  _go_spawn_edit_y->setValue(static_cast<double>(spawn->pos.y));
  _go_spawn_edit_z->setValue(static_cast<double>(spawn->pos.z));
  _go_spawn_edit_orientation->setValue(static_cast<double>(spawn->orientation));

  for (auto* w : {_go_spawn_edit_x, _go_spawn_edit_y, _go_spawn_edit_z, _go_spawn_edit_orientation})
  {
    w->setEnabled(true);
    w->blockSignals(false);
  }
}

void MapView::setHoveredGameObjectSpawn(std::optional<std::uint32_t> guid)
{
  if (_hovered_gameobject_spawn_guid == guid)
  {
    return;
  }

  if (_hovered_gameobject_spawn_guid)
  {
    if (auto* previous = _world->findGameObjectSpawn(*_hovered_gameobject_spawn_guid))
    {
      previous->hovered = false;
    }
  }

  _hovered_gameobject_spawn_guid = guid;

  if (_hovered_gameobject_spawn_guid)
  {
    if (auto* current = _world->findGameObjectSpawn(*_hovered_gameobject_spawn_guid))
    {
      current->hovered = true;
    }
  }

  _needs_redraw = true;
}

std::optional<std::uint32_t> MapView::findGameObjectSpawnAtCursor()
{
  glm::mat4x4 const mv = model_view();
  glm::mat4x4 const proj = projection();
  glm::vec4 const vp(0.0f, 0.0f, float(width()), float(height()));

  float const wx = float(_last_mouse_pos.x());
  float const wy = float(height()) - float(_last_mouse_pos.y());
  glm::vec3 const ray_near = glm::unProject(glm::vec3(wx, wy, 0.0f), mv, proj, vp);
  glm::vec3 const ray_far  = glm::unProject(glm::vec3(wx, wy, 1.0f), mv, proj, vp);
  glm::vec3 const ray_dir  = ray_far - ray_near;

  // 1) Prefer a hit on the actual 3D MODEL MESH: click the object's body, not just its ground disc.
  {
    math::ray const world_ray(ray_near, ray_dir);
    int const base_animtime = static_cast<int>(_world->model_animtime);
    float best_dist = std::numeric_limits<float>::max();
    std::optional<std::uint32_t> best_guid;
    for (auto& spawn : _world->gameObjectSpawns())
    {
      if (spawn.pending_delete || spawn.event_suppressed || !spawn.model_instance.has_value())
        continue;
      auto& inst = *spawn.model_instance;
      if (!inst.model.get() || !inst.model->finishedLoading() || inst.model->loading_failed())
        continue;
      if (!world_ray.intersect_bounds(inst.extents[0], inst.extents[1])) // see creature picker
        continue;
      selection_result hits;
      inst.intersect(mv, world_ray, &hits, base_animtime + spawn.animation_time_offset);
      for (auto const& h : hits)
      {
        if (h.first < best_dist)
        {
          best_dist = h.first;
          best_guid = spawn.guid;
        }
      }
    }
    if (best_guid)
      return best_guid;
  }

  // 2) Fallback: the ground selection-disc, so clicking the drawn circle still selects.
  float best_rel = 1.0f;
  std::optional<std::uint32_t> best_guid;
  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.pending_delete || spawn.event_suppressed)
      continue;
    float ring_radius = 0.5f;
    if (spawn.model_instance.has_value())
    {
      ring_radius = spawn.model_instance.value().selectionRingRadius();
    }
    ring_radius = std::max(0.25f, ring_radius);
    if (std::abs(ray_dir.y) < 1e-6f)
      continue;
    float const t = (spawn.pos.y - ray_near.y) / ray_dir.y;
    if (t < 0.0f)
      continue;
    glm::vec3 const hit = ray_near + ray_dir * t;
    float const d = glm::length(glm::vec2(hit.x - spawn.pos.x, hit.z - spawn.pos.z));
    float const rel = d / ring_radius;
    if (rel < 1.0f && rel < best_rel)
    {
      best_rel = rel;
      best_guid = spawn.guid;
    }
  }
  return best_guid;
}

void MapView::updateGameObjectSpawnHover(QPoint const& global_pos)
{
  if (terrainMode != editing_mode::gameobject || _dragging_gameobject_spawn || rightMouse)
  {
    setHoveredGameObjectSpawn(std::optional<std::uint32_t>());
    QToolTip::hideText();
    return;
  }

  static QElapsedTimer hover_throttle; // ~30 Hz re-pick, see the creature hover
  if (hover_throttle.isValid() && hover_throttle.elapsed() < 33)
  {
    return;
  }
  hover_throttle.restart();

  auto hovered_guid = findGameObjectSpawnAtCursor();
  setHoveredGameObjectSpawn(hovered_guid);

  if (!hovered_guid)
  {
    QToolTip::hideText();
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*hovered_guid);
  if (!spawn)
  {
    QToolTip::hideText();
    return;
  }

  QString name = QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name);
  QToolTip::showText(global_pos, QString("%1\nGUID: %2\nEntry: %3")
                               .arg(name)
                               .arg(spawn->guid)
                               .arg(spawn->entry), this);
}

bool MapView::tryStartGameObjectSpawnDrag()
{
  if (terrainMode != editing_mode::gameobject)
  {
    return false;
  }

  std::optional<std::uint32_t> best_guid = findGameObjectSpawnAtCursor();

  if (!best_guid)
  {
    return false;
  }

  auto const* clicked_spawn = _world->findGameObjectSpawn(*best_guid);
  if (!clicked_spawn)
  {
    return false;
  }

  if (!clicked_spawn->selected || selectedGameObjectSpawnCount() <= 1)
  {
    // Same as the creature path: skip the full browser-list rebuild on grab, it happens on release.
    setSelectedGameObjectSpawn(best_guid, /*update_browser*/ false);
  }

  _gameobject_drag_anchor_pos = _cursor_pos;
  _gameobject_drag_initial_positions.clear();
  // (struct init below carries orientation for the drag's undo op)
  for (auto const& spawn : _world->gameObjectSpawns())
  {
    if (spawn.selected)
    {
      _gameobject_drag_initial_positions.push_back({spawn.guid, spawn.pos, spawn.orientation});
    }
  }

  _dragging_gameobject_spawn = true;
  _main_window->statusBar()->showMessage(QString("Dragging %1 gameobject spawn(s). Release mouse, then use Export SQL.")
                                           .arg(_gameobject_drag_initial_positions.size()), 4000);
  return true;
}

void MapView::deleteSelectedGameObjectSpawns()
{
  std::vector<std::uint32_t> deleted;

  auto mark = [&](World::GameObjectSpawnOverlay& spawn)
  {
    if (spawn.pending_delete)
    {
      return;
    }
    spawn.pending_delete = true; // hidden from view/browser/picking; exported as DELETE
    spawn.dirty = true;          // count it as a pending change
    spawn.selected = false;
    spawn.hovered = false;
    deleted.push_back(spawn.guid);
  };

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (spawn.selected)
    {
      mark(spawn);
    }
  }

  if (deleted.empty() && _selected_gameobject_spawn_guid)
  {
    if (auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid))
    {
      mark(*spawn);
    }
  }

  if (deleted.empty())
  {
    return;
  }

  {
    SpawnUndoOp op;
    op.kind = SpawnUndoOp::Kind::Delete;
    op.guids = deleted;
    pushGameObjectUndoOp(std::move(op));
  }
  setSelectedGameObjectSpawn(std::nullopt, false);
  updateGameObjectBrowserStatus();
  scheduleGameObjectBrowserRebuild();
  refreshGameObjectEditorKnobs();
  _main_window->statusBar()->showMessage(
    QString("Marked %1 gameobject spawn(s) for deletion (Ctrl+Z to undo)").arg(deleted.size()), 5000);
}

bool MapView::undoGameObjectEdit()
{
  if (_gameobject_undo_ops.empty())
  {
    return false;
  }

  auto const op = _gameobject_undo_ops.back();
  _gameobject_undo_ops.pop_back();

  switch (op.kind)
  {
    case SpawnUndoOp::Kind::Move:
    {
      std::vector<std::uint32_t> touched;
      for (auto const& state : op.moves)
      {
        auto* spawn = _world->findGameObjectSpawn(state.guid);
        if (!spawn)
        {
          continue;
        }
        spawn->pos = state.pos;
        spawn->orientation = state.orientation;
        spawn->dirty = spawn->pending_create
                    || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                    || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;
        if (spawn->model_instance)
        {
          spawn->model_instance->pos = spawn->pos;
          spawn->model_instance->dir = glm::vec3(0.0f, spawn->orientation, 0.0f);
          spawn->model_instance->recalcExtents();
        }
        touched.push_back(state.guid);
      }
      updateGameObjectBrowserStatus();
      refreshGameObjectBrowserItems(touched);
      refreshGameObjectEditorKnobs();
      _needs_redraw = true;
      _main_window->statusBar()->showMessage(
        QString("Undid move of %1 gameobject spawn(s)").arg(touched.size()), 5000);
      return true;
    }

    case SpawnUndoOp::Kind::Delete:
    {
      for (auto guid : op.guids)
      {
        if (auto* spawn = _world->findGameObjectSpawn(guid))
        {
          spawn->pending_delete = false;
          // Keep it dirty only if it still has real edits (or is a never-saved spawn).
          spawn->dirty = spawn->pending_create
                      || glm::distance(spawn->pos, spawn->original_pos) > 0.01f
                      || std::abs(spawn->orientation - spawn->original_orientation) > 0.01f;
        }
      }
      updateGameObjectBrowserStatus();
      scheduleGameObjectBrowserRebuild();
      refreshGameObjectEditorKnobs();
      _main_window->statusBar()->showMessage(
        QString("Restored %1 deleted gameobject spawn(s)").arg(op.guids.size()), 5000);
      return true;
    }

    case SpawnUndoOp::Kind::Create:
    {
      std::size_t removed = 0;
      std::size_t remarked = 0;
      try
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());

        auto& spawns = _world->gameObjectSpawns();
        for (auto it = spawns.begin(); it != spawns.end(); )
        {
          if (std::find(op.guids.begin(), op.guids.end(), it->guid) == op.guids.end())
          {
            ++it;
            continue;
          }
          if (_selected_gameobject_spawn_guid && *_selected_gameobject_spawn_guid == it->guid)
          {
            _selected_gameobject_spawn_guid = std::nullopt;
          }
          if (_hovered_gameobject_spawn_guid && *_hovered_gameobject_spawn_guid == it->guid)
          {
            _hovered_gameobject_spawn_guid = std::nullopt;
          }
          if (it->pending_create)
          {
            it = spawns.erase(it);
            ++removed;
          }
          else
          {
            it->pending_delete = true;
            it->dirty = true;
            it->selected = false;
            ++remarked;
            ++it;
          }
        }
      }
      catch (...)
      {
      }
      updateGameObjectBrowserStatus();
      scheduleGameObjectBrowserRebuild();
      refreshGameObjectEditorKnobs();
      _needs_redraw = true;
      _main_window->statusBar()->showMessage(
        QString("Undid gameobject spawn addition (%1 removed%2)")
          .arg(removed)
          .arg(remarked ? QString(", %1 marked for deletion").arg(remarked) : QString()), 5000);
      return true;
    }
  }

  return false;
}

void MapView::updateSelectedGameObjectSpawnPosition(glm::vec3 const& pos)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto apply_position = [](World::GameObjectSpawnOverlay& spawn, glm::vec3 const& new_pos)
  {
    spawn.pos = new_pos;
    spawn.dirty = spawn.pending_create
               || glm::distance(spawn.pos, spawn.original_pos) > 0.01f
               || std::abs(spawn.orientation - spawn.original_orientation) > 0.01f;

    if (spawn.model_instance)
    {
      spawn.model_instance->pos = spawn.pos;
      spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
      spawn.model_instance->recalcExtents();
    }
  };

  if (_dragging_gameobject_spawn && _gameobject_drag_anchor_pos && !_gameobject_drag_initial_positions.empty())
  {
    if (_gameobject_drag_initial_positions.size() == 1)
    {
      // Single spawn: snap directly under the cursor's ground point.
      if (auto* spawn = _world->findGameObjectSpawn(_gameobject_drag_initial_positions[0].guid))
      {
        apply_position(*spawn, pos);
      }
    }
    else
    {
      glm::vec3 const delta = pos - *_gameobject_drag_anchor_pos;
      for (auto const& drag_state : _gameobject_drag_initial_positions)
      {
        if (auto* spawn = _world->findGameObjectSpawn(drag_state.guid))
        {
          apply_position(*spawn, drag_state.pos + delta);
        }
      }
    }
  }
  else
  {
    auto* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
    if (!spawn)
    {
      return;
    }

    apply_position(*spawn, pos);
  }

  // PERF: same as the creature path -- rebuildGameObjectBrowserList() repopulates a list widget over
  // every gameobject spawn on every mouse-move. Deferred to the drag end.
  if (_dragging_gameobject_spawn)
  {
    return;
  }

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
}

void MapView::showSelectedGameObjectSpawnMenu(QPoint const& global_pos)
{
  if (!_selected_gameobject_spawn_guid)
  {
    return;
  }

  auto const* spawn = _world->findGameObjectSpawn(*_selected_gameobject_spawn_guid);
  if (!spawn)
  {
    return;
  }

  QMenu menu(this);
  menu.addAction(QString("GameObject: %1").arg(QString::fromStdString(spawn->name.empty() ? std::string("<unnamed>") : spawn->name)))->setEnabled(false);
  menu.addAction(QString("Unique ID: %1").arg(spawn->guid))->setEnabled(false);
  menu.addAction(QString("Entry: %1").arg(spawn->entry))->setEnabled(false);
  menu.addAction(QString("Display ID: %1").arg(spawn->display_id))->setEnabled(false);
  menu.addAction(QString("Position: %1, %2, %3")
                   .arg(spawn->pos.x, 0, 'f', 2)
                   .arg(spawn->pos.y, 0, 'f', 2)
                   .arg(spawn->pos.z, 0, 'f', 2))->setEnabled(false);
  menu.addSeparator();
  auto* jump_action = menu.addAction("Center camera here");
  auto* save_action = menu.addAction("Save pending gameobject changes");
  auto* chosen = menu.exec(global_pos);

  if (chosen == jump_action)
  {
    focus_camera_on_target(spawn->pos);
  }
  else if (chosen == save_action)
  {
    saveDirtyGameObjectSpawns();
  }
}

void MapView::discardPendingGameObjectSpawns()
{
  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No gameobject spawn changes to discard", 4000);
    updateGameObjectBrowserStatus();
    return;
  }

  _selected_gameobject_spawn_guid = std::nullopt;
  _hovered_gameobject_spawn_guid = std::nullopt;
  _dragging_gameobject_spawn = false;
  _gameobject_drag_anchor_pos = std::nullopt;
  _gameobject_drag_initial_positions.clear();
  _gameobject_undo_ops.clear();

  std::size_t removed_new = 0;
  std::size_t reverted_existing = 0;

  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    auto& spawns = _world->gameObjectSpawns();
    for (auto& spawn : spawns)
    {
      spawn.selected = false;
      spawn.hovered = false;

      if (!spawn.dirty || spawn.pending_create)
      {
        continue;
      }

      spawn.pos = spawn.original_pos;
      spawn.orientation = spawn.original_orientation;
      spawn.dirty = false;
      spawn.pending_delete = false; // restore any spawn that was marked for deletion
      if (spawn.model_instance)
      {
        spawn.model_instance->pos = spawn.pos;
        spawn.model_instance->dir = glm::vec3(0.0f, spawn.orientation, 0.0f);
        spawn.model_instance->recalcExtents();
      }
      ++reverted_existing;
    }

    auto pending_begin = std::remove_if(spawns.begin(), spawns.end(),
      [&removed_new](World::GameObjectSpawnOverlay const& spawn)
      {
        if (!spawn.pending_create)
        {
          return false;
        }

        ++removed_new;
        return true;
      });
    spawns.erase(pending_begin, spawns.end());
  }
  catch (std::exception const& ex)
  {
    _main_window->statusBar()->showMessage(QString("Failed to discard gameobject spawn changes: %1").arg(ex.what()), 7000);
    updateGameObjectBrowserStatus();
    return;
  }
  catch (...)
  {
    _main_window->statusBar()->showMessage("Failed to discard gameobject spawn changes: unknown error", 7000);
    updateGameObjectBrowserStatus();
    return;
  }

  rebuildGameObjectBrowserList(false);
  refreshGameObjectEditorKnobs();
  updateGameObjectBrowserStatus();
  _needs_redraw = true;

  _main_window->statusBar()->showMessage(
    QString("Discarded %1 pending gameobject spawn(s), reverted %2 edited spawn(s)")
      .arg(removed_new)
      .arg(reverted_existing),
    5000);
}

QString MapView::buildDirtyGameObjectSpawnSql(bool rebase_state)
{
  QString sql;
  QTextStream stream(&sql);
  stream << "-- Noggit gameobject spawn update export\n";
  stream << "-- Map ID: " << _world->getMapID() << "\n";
  stream << "-- Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
  stream << "\n";

  for (auto& spawn : _world->gameObjectSpawns())
  {
    if (!spawn.dirty)
    {
      continue;
    }

    if (spawn.pending_delete)
    {
      // A spawn created this session and then deleted never reached the DB -> nothing to export.
      if (spawn.pending_create)
      {
        continue;
      }
      stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
             << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name) << "\n";
      stream << "DELETE FROM gameobject WHERE guid=" << spawn.guid << ";\n\n";
      continue;
    }

    auto server_pos = client_to_server_creature_position(spawn.pos, _world->mapIndex.hasAGlobalWMO());
    auto server_orientation = client_to_server_creature_orientation(spawn.orientation);
    stream << "-- GUID " << spawn.guid << " entry " << spawn.entry << " "
           << QString::fromStdString(spawn.name.empty() ? std::string("<unnamed>") : spawn.name)
           << "\n";

    if (spawn.pending_create)
    {
      stream << "-- Preview display ID: " << spawn.display_id << "\n";
      stream << "INSERT INTO gameobject (guid, id, map, position_x, position_y, position_z, orientation)\n"
             << "VALUES (" << spawn.guid << ", "
             << spawn.entry << ", "
             << _world->getMapID() << ", "
             << QString::number(server_pos.x, 'f', 6) << ", "
             << QString::number(server_pos.y, 'f', 6) << ", "
             << QString::number(server_pos.z, 'f', 6) << ", "
             << QString::number(server_orientation, 'f', 6) << ");\n\n";
    }
    else
    {
      stream << "UPDATE gameobject\n"
             << "SET position_x = " << QString::number(server_pos.x, 'f', 6) << ",\n"
             << "    position_y = " << QString::number(server_pos.y, 'f', 6) << ",\n"
             << "    position_z = " << QString::number(server_pos.z, 'f', 6) << ",\n"
             << "    orientation = " << QString::number(server_orientation, 'f', 6) << "\n"
             << "WHERE guid = " << spawn.guid << ";\n\n";
    }

    if (rebase_state)
    {
      spawn.original_pos = spawn.pos;
      spawn.original_orientation = spawn.orientation;
      spawn.pending_create = false;
      spawn.dirty = false;
    }
  }

  return sql;
}

void MapView::saveDirtyGameObjectSpawns()
{
  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No gameobject spawn changes to export", 4000);
    updateGameObjectBrowserStatus();
    return;
  }

  QDir project_dir(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));
  QString export_dir_path = project_dir.filePath("sql_exports/gameobject_spawns");
  QDir export_dir(export_dir_path);
  if (!export_dir.exists() && !project_dir.mkpath("sql_exports/gameobject_spawns"))
  {
    auto message = QString("Failed to create gameobject SQL export folder: %1").arg(export_dir_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateGameObjectBrowserStatus(message);
    return;
  }

  QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
  QString file_name = QString("gameobject_updates_map%1_%2.sql").arg(_world->getMapID()).arg(timestamp);
  QString file_path = export_dir.filePath(file_name);

  QFile output(file_path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    auto message = QString("Failed to write gameobject SQL export: %1").arg(file_path);
    _main_window->statusBar()->showMessage(message, 6000);
    updateGameObjectBrowserStatus(message);
    return;
  }

  QTextStream stream(&output);
  stream.setCodec("UTF-8");
  stream << buildDirtyGameObjectSpawnSql(/*rebase_state*/ true);
  output.close();

  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  _main_window->statusBar()->showMessage(QString("GameObject spawn SQL exported: %1").arg(file_path), 7000);
}

// ---------------------------------------------------------------------------
// SQL apply / reset tooling. Everything here targets the PROJECT's configured MySQL connection
// (Settings -> MySQL, per-project), so a Turtle project only ever pushes into its own tw_world-
// style schema and never into some other server's database.
// ---------------------------------------------------------------------------

#ifdef USE_MYSQL_UID_STORAGE
void MapView::applyDirtyCreatureSpawnsToDb()
{
  auto dirty_count = _world->dirtyCreatureSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No creature spawn changes to apply", 4000);
    return;
  }

  QString const sql = buildDirtyCreatureSpawnSql(/*rebase_state*/ false);
  if (!confirmSqlApply(this, "Apply creature changes",
                       QString("Apply %1 pending creature spawn change(s) directly to the database?\n"
                               "The same SQL is also written to sql_exports/ as a record.")
                         .arg(dirty_count),
                       sql))
  {
    return;
  }

  auto const result = mysql::executeSqlScript(sql.toStdString());
  reportSqlResult(this, result, "Creature spawn changes");
  if (!result.ok)
  {
    return;
  }

  // Keep a file record of exactly what was applied, and rebase the editor state (this also writes
  // the export file). Applied DELETEs are then dropped from the overlay outright -- they no longer
  // exist in the database.
  saveDirtyCreatureSpawns();
  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());
    auto& spawns = _world->creatureSpawns();
    spawns.erase(std::remove_if(spawns.begin(), spawns.end(),
                                [](World::CreatureSpawnOverlay const& s)
                                { return s.pending_delete && !s.pending_create; }),
                 spawns.end());
  }
  catch (...)
  {
  }
  updateDatabaseStatus();
  rebuildCreatureBrowserList(true);
  refreshCreatureEditorKnobs();
  _needs_redraw = true;
}

void MapView::applyDirtyGameObjectSpawnsToDb()
{
  auto dirty_count = _world->dirtyGameObjectSpawnCount();
  if (dirty_count == 0)
  {
    _main_window->statusBar()->showMessage("No gameobject spawn changes to apply", 4000);
    return;
  }

  QString const sql = buildDirtyGameObjectSpawnSql(/*rebase_state*/ false);
  if (!confirmSqlApply(this, "Apply gameobject changes",
                       QString("Apply %1 pending gameobject spawn change(s) directly to the database?\n"
                               "The same SQL is also written to sql_exports/ as a record.")
                         .arg(dirty_count),
                       sql))
  {
    return;
  }

  auto const result = mysql::executeSqlScript(sql.toStdString());
  reportSqlResult(this, result, "GameObject spawn changes");
  if (!result.ok)
  {
    return;
  }

  saveDirtyGameObjectSpawns();
  try
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());
    auto& spawns = _world->gameObjectSpawns();
    spawns.erase(std::remove_if(spawns.begin(), spawns.end(),
                                [](World::GameObjectSpawnOverlay const& s)
                                { return s.pending_delete && !s.pending_create; }),
                 spawns.end());
  }
  catch (...)
  {
  }
  updateGameObjectBrowserStatus();
  rebuildGameObjectBrowserList(true);
  refreshGameObjectEditorKnobs();
  _needs_redraw = true;
}

void MapView::applySqlFileToDb()
{
  QDir project_dir(QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));
  QString start_dir = project_dir.filePath("sql_exports");
  if (!QDir(start_dir).exists())
  {
    start_dir = project_dir.absolutePath();
  }

  QString const file_path = QFileDialog::getOpenFileName(this, "Apply SQL file to database",
                                                         start_dir, "SQL scripts (*.sql);;All files (*)");
  if (file_path.isEmpty())
  {
    return;
  }

  QFile file(file_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    QMessageBox::critical(this, "SQL apply failed", QString("Could not read %1").arg(file_path));
    return;
  }
  QString const sql = QTextStream(&file).readAll();
  file.close();

  if (sql.trimmed().isEmpty())
  {
    QMessageBox::information(this, "SQL apply", "The file contains no SQL.");
    return;
  }

  if (!confirmSqlApply(this, "Apply SQL file",
                       QString("Apply this SQL file to the database?\n%1").arg(QFileInfo(file_path).fileName()),
                       sql))
  {
    return;
  }

  auto const result = mysql::executeSqlScript(sql.toStdString());
  reportSqlResult(this, result, QFileInfo(file_path).fileName());
  if (result.ok)
  {
    // The DB just changed underneath the overlays -- resync them so the editor shows the result
    // (reloadCreatureSpawns re-reads gameobjects alongside).
    refreshCreatureSpawnOverlay(true);
    rebuildGameObjectBrowserList(true);
  }
}

void MapView::resetDatabaseFromSqlFolders()
{
  QSettings settings;

  QDialog dialog(this);
  dialog.setWindowTitle("Reset database from SQL folders");
  auto* layout = new QVBoxLayout(&dialog);

  auto* info = new QLabel(QString(
    "Rebuilds the project's database by running every .sql file (sorted by name) from the folders\n"
    "below through the MySQL client -- first the BASE folder, then the UPDATES folder. Point them\n"
    "at your server's sql/base and sql/database_updates directories.\n\n"
    "Target database: %1").arg(QString::fromStdString(mysql::connectionDescription())), &dialog);
  layout->addWidget(info);

  auto make_path_row = [&](QString const& label_text, QString const& settings_key, bool pick_file)
  {
    auto* row = new QHBoxLayout();
    auto* label = new QLabel(label_text, &dialog);
    label->setMinimumWidth(150);
    auto* edit = new QLineEdit(settings.value(Noggit::mysqlSettingKey(settings_key)).toString(), &dialog);
    auto* browse = new QPushButton("...", &dialog);
    browse->setMaximumWidth(30);
    QObject::connect(browse, &QPushButton::clicked, [&dialog, edit, pick_file]()
    {
      QString picked = pick_file
        ? QFileDialog::getOpenFileName(&dialog, "Select mysql client executable", edit->text(),
                                       "mysql client (mysql.exe mysql);;All files (*)")
        : QFileDialog::getExistingDirectory(&dialog, "Select SQL folder", edit->text());
      if (!picked.isEmpty())
      {
        edit->setText(picked);
      }
    });
    row->addWidget(label);
    row->addWidget(edit);
    row->addWidget(browse);
    layout->addLayout(row);
    return edit;
  };

  auto* mysql_client_edit = make_path_row("MySQL client (mysql.exe):", "reset_mysql_client", true);
  auto* base_edit = make_path_row("Base SQL folder:", "reset_base_dir", false);
  auto* updates_edit = make_path_row("Updates SQL folder (optional):", "reset_updates_dir", false);

  auto* confirm_label = new QLabel(QString("This OVERWRITES data in the target database. Type the schema name (%1) to confirm:")
                                     .arg(Noggit::mysqlSetting("db", "tw_world").toString()), &dialog);
  layout->addWidget(confirm_label);
  auto* confirm_edit = new QLineEdit(&dialog);
  layout->addWidget(confirm_edit);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText("Run reset");
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted)
  {
    return;
  }

  QString const schema = Noggit::mysqlSetting("db", "tw_world").toString();
  if (confirm_edit->text().trimmed() != schema)
  {
    QMessageBox::warning(this, "Reset cancelled",
                         QString("Confirmation text did not match the schema name (%1). Nothing was run.").arg(schema));
    return;
  }

  QString const client = mysql_client_edit->text().trimmed();
  if (client.isEmpty() || !QFileInfo::exists(client))
  {
    QMessageBox::critical(this, "Reset failed", "MySQL client executable not found. Point the first field at mysql.exe.");
    return;
  }

  // Remember the paths per project for next time.
  settings.setValue(Noggit::mysqlSettingKey("reset_mysql_client"), client);
  settings.setValue(Noggit::mysqlSettingKey("reset_base_dir"), base_edit->text().trimmed());
  settings.setValue(Noggit::mysqlSettingKey("reset_updates_dir"), updates_edit->text().trimmed());

  QStringList files;
  for (auto const& dir_path : {base_edit->text().trimmed(), updates_edit->text().trimmed()})
  {
    if (dir_path.isEmpty())
    {
      continue;
    }
    QDir dir(dir_path);
    if (!dir.exists())
    {
      QMessageBox::critical(this, "Reset failed", QString("Folder does not exist: %1").arg(dir_path));
      return;
    }
    for (auto const& entry : dir.entryList(QStringList() << "*.sql", QDir::Files, QDir::Name))
    {
      files << dir.filePath(entry);
    }
  }

  if (files.isEmpty())
  {
    QMessageBox::information(this, "Reset", "No .sql files found in the selected folder(s).");
    return;
  }

  // Stream each file into the mysql CLIENT (stdin), which handles arbitrarily large dumps and the
  // full statement syntax. Sequential, abort on the first failure.
  // Direct mode: the saved server/port. SSH tunnel mode: the tunnel's local 127.0.0.1 endpoint.
  std::string endpoint_host;
  unsigned int endpoint_port = 0;
  std::string endpoint_error;
  if (!mysql::resolveEndpoint(endpoint_host, endpoint_port, &endpoint_error))
  {
    QMessageBox::critical(this, "Reset failed", QString::fromStdString(endpoint_error));
    return;
  }
  QString const host = QString::fromStdString(endpoint_host);
  QString const user = Noggit::mysqlSetting("user", "root").toString();
  QString const pwd = Noggit::mysqlSetting("pwd", "mangos").toString();
  QString const port = QString::number(endpoint_port);

  QProgressDialog progress(QString("Running %1 SQL file(s) against %2...").arg(files.size()).arg(schema),
                           "Abort", 0, files.size(), this);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(0);

  int done = 0;
  for (auto const& sql_file : files)
  {
    if (progress.wasCanceled())
    {
      QMessageBox::warning(this, "Reset aborted",
                           QString("Aborted after %1 of %2 file(s). The database may be partially rebuilt.")
                             .arg(done).arg(files.size()));
      return;
    }
    progress.setValue(done);
    progress.setLabelText(QString("[%1/%2] %3").arg(done + 1).arg(files.size()).arg(QFileInfo(sql_file).fileName()));
    QCoreApplication::processEvents();

    QProcess proc;
    proc.setStandardInputFile(sql_file);
    proc.start(client, QStringList()
               << "-h" << host << "-P" << port << "-u" << user
               << (QString("-p") + pwd) << "--default-character-set=utf8" << schema);
    if (!proc.waitForStarted(10000))
    {
      QMessageBox::critical(this, "Reset failed", QString("Could not start the mysql client: %1").arg(client));
      return;
    }
    // Big base dumps take a while; no fixed timeout, the progress dialog's Abort stays responsive.
    while (!proc.waitForFinished(250))
    {
      QCoreApplication::processEvents();
      if (progress.wasCanceled())
      {
        proc.kill();
        proc.waitForFinished(5000);
        QMessageBox::warning(this, "Reset aborted",
                             QString("Aborted during %1. The database may be partially rebuilt.")
                               .arg(QFileInfo(sql_file).fileName()));
        return;
      }
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
      QString err = QString::fromLocal8Bit(proc.readAllStandardError());
      if (err.size() > 1500)
      {
        err = err.left(1500) + "\n[... truncated ...]";
      }
      QMessageBox::critical(this, "Reset failed",
                            QString("%1 failed (exit code %2). Later files were NOT run.\n\n%3")
                              .arg(QFileInfo(sql_file).fileName())
                              .arg(proc.exitCode())
                              .arg(err));
      return;
    }
    ++done;
  }
  progress.setValue(files.size());

  QMessageBox::information(this, "Reset complete",
                           QString("Ran %1 SQL file(s) against %2.").arg(files.size()).arg(schema));
  refreshCreatureSpawnOverlay(true);
}
#endif

void MapView::jumpToGameObjectListItem(QListWidgetItem* item)
{
  if (!item)
  {
    return;
  }

  auto guid = static_cast<std::uint32_t>(item->data(Qt::UserRole).toULongLong());
  auto is_current_map_item = item->data(Qt::UserRole + 6).toBool();

  if (is_current_map_item)
  {
    setSelectedGameObjectSpawn(guid, true);
    auto const* spawn = _world->findGameObjectSpawn(guid);
    if (spawn)
    {
      focus_camera_on_target(spawn->pos);
    }
    return;
  }
}

void MapView::setupHotkeys()
{

  addHotkey ( Qt::Key_F1
    , MOD_shift
    , [this]
              {
                if (alloff)
                {
                  alloff_models = _draw_models.get();
                  alloff_doodads = _draw_wmo_doodads.get();
                  alloff_contour = _draw_contour.get();
                  alloff_climb = _draw_climb.get();
                  alloff_vertex_color = _draw_vertex_color.get();
                  alloff_baked_shadows = _draw_baked_shadows.get();
                  alloff_wmo = _draw_wmo.get();
                  alloff_fog = _draw_fog.get();
                  alloff_terrain = _draw_terrain.get();

                  _draw_models.set (false);
                  _draw_wmo_doodads.set (false);
                  _draw_contour.set (true);
                  _draw_climb.set (false);
                  _draw_vertex_color.set(true);
                  _draw_baked_shadows.set(true);
                  _draw_wmo.set (false);
                  _draw_terrain.set (true);
                  _draw_fog.set (false);
                }
                else
                {
                  _draw_models.set (alloff_models);
                  _draw_wmo_doodads.set (alloff_doodads);
                  _draw_contour.set (alloff_contour);
                  _draw_climb.set(alloff_climb);
                  _draw_vertex_color.set(alloff_vertex_color);
                  _draw_baked_shadows.set(alloff_baked_shadows);
                  _draw_wmo.set (alloff_wmo);
                  _draw_terrain.set (alloff_terrain);
                  _draw_fog.set (alloff_fog);
                }
                alloff = !alloff;
              }
  );

  addHotkey ( Qt::Key_C
    , MOD_ctrl
    , [this]
              {
                objectEditor->copy_current_selection(_world.get());
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  /*
  addHotkey ( Qt::Key_C
    , MOD_none
    , [this]
              {
                objectEditor->copy_current_selection(_world.get());
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );*/

  addHotkey ( Qt::Key_V
    , MOD_ctrl
    ,
              [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                objectEditor->pasteObject (_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  /*
  addHotkey ( Qt::Key_V
    , MOD_none
    , [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                objectEditor->pasteObject (_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );*/
  addHotkey ( Qt::Key_V
    , MOD_shift
    , [this] { objectEditor->import_last_model_from_wmv(eMODEL); }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_V
    , MOD_alt
    , [this] { objectEditor->import_last_model_from_wmv(eWMO); }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_C
    , MOD_none
    , [this]
    {
      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eVERTEX_SELECTION);
      _world->clearVertexSelection();
      NOGGIT_ACTION_MGR->endAction();
    }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );

  addHotkey( Qt::Key_B
    , MOD_ctrl
    , [this]
             {
               NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
               objectEditor->copy_current_selection(_world.get());
               objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
               NOGGIT_ACTION_MGR->endAction();
             }
    , [this] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_Y
    , MOD_none
    , [this] { terrainTool->nextType(); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_Y
    , MOD_none
    , [this] { flattenTool->nextFlattenType(); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                flattenTool->toggleFlattenAngle();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_space
    , [&]
              {
                _left_sec_toolbar->nextFlattenMode(this);
                flattenTool->nextFlattenMode();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                texturingTool->toggle_tool();
              }
    , [&] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES);
                _world->setHoleADT (_camera.position, false);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&]
              {
                return terrainMode == editing_mode::holes && !NOGGIT_CUR_ACTION;
              }
  );

  addHotkey ( Qt::Key_T
    , MOD_alt
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES);
                _world->setHoleADT (_camera.position, true);
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&] { return terrainMode == editing_mode::holes && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                guiWater->toggle_angled_mode();
              }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_T
    , MOD_none
    , [&]
              {
                objectEditor->togglePasteMode();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );


  addHotkey ( Qt::Key_H
    , MOD_none
    , [&]
              {
                if (_world->has_selection())
                {
                  for (auto& selection : _world->current_selection())
                  {
                    if (selection.index() != eEntry_Object)
                      continue;

                    auto obj = std::get<selected_object_type>(selection);

                    if (obj->which() == eMODEL)
                    {
                      static_cast<ModelInstance*>(obj)->model->toggle_visibility();
                    }
                    else if (obj->which() == eWMO)
                    {
                      static_cast<WMOInstance*>(obj)->wmo->toggle_visibility();
                    }
                  }
                }
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_H
    , MOD_space
    , [&]
              {
                _draw_hidden_models.toggle();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey(Qt::Key_R
    , MOD_space
    , [&]
            {
              texturingTool->toggle_brush_level_min_max();
            }
    , [&] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_H
    , MOD_shift
    , [&]
              {
                ModelManager::clear_hidden_models();
                WMOManager::clear_hidden_wmos();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
                terrainTool->flattenVertices (_world.get());
                NOGGIT_ACTION_MGR->endAction();

              }
    , [&] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                flattenTool->toggleFlattenLock();
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_none
    , [&]
              {
                flattenTool->lockPos (_cursor_pos);
              }
    , [&] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_space
    , [&]
              {
                guiWater->toggle_lock();
              }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );
  addHotkey( Qt::Key_F
    , MOD_none
    , [&]
             {
               guiWater->lockPos(_cursor_pos);
             }
    , [&] { return terrainMode == editing_mode::water && !NOGGIT_CUR_ACTION; }
  );
  addHotkey ( Qt::Key_F
    , MOD_none
    , [&]
              {

                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                _world->set_selected_models_pos(_cursor_pos);
                _rotation_editor_need_update = true;
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_Plus, MOD_alt, [this] { terrainTool->changeRadius(0.01f); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; });

  addHotkey (Qt::Key_Plus, MOD_alt, [this] { flattenTool->changeRadius(0.01f); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; });

  addHotkey ( Qt::Key_Plus
    , MOD_alt
    , [&]
              {
                texturingTool->change_radius(0.1f);
              }
    , [this] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_Minus, MOD_alt, [this] { terrainTool->changeRadius(-0.01f); }
    , [this] { return terrainMode == editing_mode::ground && !NOGGIT_CUR_ACTION; });

  addHotkey (Qt::Key_Minus, MOD_alt, [this] { flattenTool->changeRadius(-0.01f); }
    , [this] { return terrainMode == editing_mode::flatten_blur && !NOGGIT_CUR_ACTION; });

  addHotkey ( Qt::Key_Minus
    , MOD_alt
    , [&]
              {
                texturingTool->change_radius(-0.1f);
              }
    , [this] { return terrainMode == editing_mode::paint && !NOGGIT_CUR_ACTION; }
  );

  addHotkey (Qt::Key_1, MOD_shift, [this] { _camera.move_speed = 15.0f; });
  addHotkey (Qt::Key_2, MOD_shift, [this] { _camera.move_speed = 50.0f; });
  addHotkey (Qt::Key_3, MOD_shift, [this] { _camera.move_speed = 200.0f; });
  addHotkey (Qt::Key_4, MOD_shift, [this] { _camera.move_speed = 800.0f; });
  addHotkey (Qt::Key_1, MOD_alt, [this] { texturingTool->set_brush_level(0.0f); });
  addHotkey (Qt::Key_2, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.25f); });
  addHotkey (Qt::Key_3, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.5f); });
  addHotkey (Qt::Key_4, MOD_alt, [this] { texturingTool->set_brush_level(255.0f* 0.75f); });
  addHotkey (Qt::Key_5, MOD_alt, [this] { texturingTool->set_brush_level(255.0f); });

  addHotkey(Qt::Key_1, MOD_none, [this] { set_editing_mode(editing_mode::ground); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_2, MOD_none, [this] { set_editing_mode (editing_mode::flatten_blur); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_3, MOD_none, [this] { set_editing_mode (editing_mode::paint); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_4, MOD_none, [this] { set_editing_mode (editing_mode::holes); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_5, MOD_none, [this] { set_editing_mode (editing_mode::areaid); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_6, MOD_none, [this] { set_editing_mode (editing_mode::flags); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_7, MOD_none, [this] { set_editing_mode (editing_mode::water); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_8, MOD_none, [this] { set_editing_mode (editing_mode::mccv); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_9, MOD_none, [this] { set_editing_mode (editing_mode::object); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });

  addHotkey(Qt::Key_0, MOD_ctrl, [this] { change_selected_wmo_doodadset(0); });
  addHotkey(Qt::Key_1, MOD_ctrl, [this] { change_selected_wmo_doodadset(1); });
  addHotkey(Qt::Key_2, MOD_ctrl, [this] { change_selected_wmo_doodadset(2); });
  addHotkey(Qt::Key_3, MOD_ctrl, [this] { change_selected_wmo_doodadset(3); });
  addHotkey(Qt::Key_4, MOD_ctrl, [this] { change_selected_wmo_doodadset(4); });
  addHotkey(Qt::Key_5, MOD_ctrl, [this] { change_selected_wmo_doodadset(5); });
  addHotkey(Qt::Key_6, MOD_ctrl, [this] { change_selected_wmo_doodadset(6); });
  addHotkey(Qt::Key_7, MOD_ctrl, [this] { change_selected_wmo_doodadset(7); });
  addHotkey(Qt::Key_8, MOD_ctrl, [this] { change_selected_wmo_doodadset(8); });
  addHotkey(Qt::Key_9, MOD_ctrl, [this] { change_selected_wmo_doodadset(9); });

  addHotkey(Qt::Key_Escape, MOD_none, [this] { _main_window->close(); });
}

void MapView::setupMinimap()
{
  _minimap = new Noggit::Ui::minimap_widget(this);
  _minimap_dock = new QDockWidget("Minimap", this);
  _minimap_dock->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  _minimap_dock->setFixedSize(_minimap->sizeHint());
  _minimap_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

  _minimap->world (_world.get());
  _minimap->camera (&_camera);
  _minimap->draw_boundaries (_show_minimap_borders.get());
  _minimap->draw_skies (_show_minimap_skies.get());
  _minimap->set_resizeable(true);

  connect ( _minimap, &Noggit::Ui::minimap_widget::map_clicked
    , [this] (glm::vec3 const& pos)
            {
              move_camera_with_auto_height (pos);
            }
  );

  _minimap_dock->setFeatures ( QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable
  );
  auto minimap_scroll_area = new QScrollArea(_minimap_dock);
  minimap_scroll_area->setWidget(_minimap);
  minimap_scroll_area->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  _minimap_dock->setWidget(minimap_scroll_area);
  _main_window->addDockWidget (Qt::LeftDockWidgetArea, _minimap_dock);
  _minimap_dock->setVisible (false);
  _minimap_dock->setFloating(true);
  _minimap_dock->move(_main_window->rect().center() - _minimap->rect().center());


  connect(this, &QObject::destroyed, _minimap_dock, &QObject::deleteLater);
  connect(this, &QObject::destroyed, _minimap, &QObject::deleteLater);

  connect ( &_show_minimap_window, &Noggit::BoolToggleProperty::changed
    , _minimap_dock, [this]
            {
              if (!ui_hidden)
                _minimap_dock->setVisible(_show_minimap_window.get());
            }
  );


  connect ( _minimap_dock, &QDockWidget::visibilityChanged
    , &_show_minimap_window, &Noggit::BoolToggleProperty::set
  );

  connect ( &_show_minimap_borders, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_boundaries(_show_minimap_borders.get());
            }
  );

  connect ( &_show_minimap_skies, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_skies(_show_minimap_skies.get());
            }
  );

}

void MapView::createGUI()
{
  LogDebug << "MapView::createGUI begin" << std::endl;
  // Combined dock
  _tool_panel_dock = new Noggit::Ui::Tools::ToolPanel(this);
  _tool_panel_dock->setFeatures(QDockWidget::DockWidgetMovable
                                | QDockWidget::DockWidgetFloatable);
  _tool_panel_dock->setAllowedAreas(Qt::RightDockWidgetArea);

  connect(this, &QObject::destroyed, _tool_panel_dock, &QObject::deleteLater);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _tool_panel_dock);

  // These calls need to be correctly ordered in order to work with the toolbar.
  // TODO: fix

  setupRaiseLowerUi();
  LogDebug << "MapView::createGUI setupRaiseLowerUi done" << std::endl;
  setupFlattenBlurUi();
  LogDebug << "MapView::createGUI setupFlattenBlurUi done" << std::endl;
  setupTexturePainterUi();
  LogDebug << "MapView::createGUI setupTexturePainterUi done" << std::endl;
  setupHoleCutterUi();
  LogDebug << "MapView::createGUI setupHoleCutterUi done" << std::endl;
  setupAreaDesignatorUi();
  LogDebug << "MapView::createGUI setupAreaDesignatorUi done" << std::endl;
  setupFlagUi();
  LogDebug << "MapView::createGUI setupFlagUi done" << std::endl;
  setupWaterEditorUi();
  LogDebug << "MapView::createGUI setupWaterEditorUi done" << std::endl;
  setupVertexPainterUi();
  LogDebug << "MapView::createGUI setupVertexPainterUi done" << std::endl;
  setupObjectEditorUi();
  LogDebug << "MapView::createGUI setupObjectEditorUi done" << std::endl;
  setupCreatureBrowserUi();
  LogDebug << "MapView::createGUI setupCreatureBrowserUi done" << std::endl;
  setupCreatureEditorUi();
  LogDebug << "MapView::createGUI setupCreatureEditorUi done" << std::endl;
  setupCreatureModelPickerUi();
  LogDebug << "MapView::createGUI setupCreatureModelPickerUi done" << std::endl;
  setupGameObjectEditorUi();
  LogDebug << "MapView::createGUI setupGameObjectEditorUi done" << std::endl;
  setupGameObjectBrowserUi();
  LogDebug << "MapView::createGUI setupGameObjectBrowserUi done" << std::endl;
  setupGameObjectModelPickerUi();
  LogDebug << "MapView::createGUI setupGameObjectModelPickerUi done" << std::endl;
  setupMinimapEditorUi();
  LogDebug << "MapView::createGUI setupMinimapEditorUi done" << std::endl;
  setupStampUi();
  LogDebug << "MapView::createGUI setupStampUi done" << std::endl;
  setupLightEditorUi();
  LogDebug << "MapView::createGUI setupLightEditorUi done" << std::endl;
  setupChunkManipulatorUi();
  LogDebug << "MapView::createGUI setupChunkManipulatorUi done" << std::endl;
  setupScriptingUi();
  LogDebug << "MapView::createGUI setupScriptingUi done" << std::endl;
  // End combined dock

  setupViewportOverlay();
  LogDebug << "MapView::createGUI setupViewportOverlay done" << std::endl;
  setupCreatureActionsUi();
  LogDebug << "MapView::createGUI setupCreatureActionsUi done" << std::endl;
  setupGameObjectActionsUi();
  LogDebug << "MapView::createGUI setupGameObjectActionsUi done" << std::endl;
  setupAssetBrowser();
  LogDebug << "MapView::createGUI setupAssetBrowser done" << std::endl;
  setupDetailInfos();
  LogDebug << "MapView::createGUI setupDetailInfos done" << std::endl;
  setupToolbars();
  LogDebug << "MapView::createGUI setupToolbars done" << std::endl;
  setupKeybindingsGui();
  LogDebug << "MapView::createGUI setupKeybindingsGui done" << std::endl;

  setupMinimap();
  LogDebug << "MapView::createGUI setupMinimap done" << std::endl;
  setupFileMenu();
  LogDebug << "MapView::createGUI setupFileMenu done" << std::endl;
  setupEditMenu();
  LogDebug << "MapView::createGUI setupEditMenu done" << std::endl;
  setupViewMenu();
  LogDebug << "MapView::createGUI setupViewMenu done" << std::endl;
  setupAssistMenu();
  LogDebug << "MapView::createGUI setupAssistMenu done" << std::endl;
  setupHelpMenu();
  LogDebug << "MapView::createGUI setupHelpMenu done" << std::endl;
  setupHotkeys();
  LogDebug << "MapView::createGUI setupHotkeys done" << std::endl;

  connect(_main_window, &Noggit::Ui::Windows::NoggitWindow::exitPromptOpened, this, &MapView::on_exit_prompt);

  set_editing_mode (editing_mode::ground);

  // [2026-09-06] Creature spawns are NEVER forced off. The old form gated the restore on an env
  // var and fell back to FALSE -- and because the toggle's change handler persists its value, a
  // forced-off start WROTE false back to the user's settings, so spawns came up off on every
  // later restart. Restore exactly what the user saved; default ON if the key was never written.
  LogError << "[SPAWN-TOGGLE] startup restore: saved=" << (_settings->value("view/creature_spawns", true).toBool() ? "true" : "false")
           << " current=" << (_draw_creature_spawns.get() ? "true" : "false") << std::endl;
  _draw_creature_spawns.set(_settings->value("view/creature_spawns", true).toBool());
  _settings->setValue("map_view/creature_browser", false);
  _settings->sync();
  _show_creature_browser.set(false);
  refreshCreatureSpawnOverlay(false);
  updateDatabaseStatus();
  LogDebug << "MapView::createGUI end" << std::endl;
}

void MapView::on_exit_prompt()
{
  // hide all popups
  _keybindings->hide();
  _minimap_dock->hide();
  _texture_palette_small->hide();
  _object_palette_dock->hide();
  objectEditor->helper_models_widget->hide();
  objectEditor->modelImport->hide();
  objectEditor->rotationEditor->hide();
  _detail_infos_dock->hide();
  if (_creature_actions_overlay) _creature_actions_overlay->hide();
  if (_creature_browser_dock) _creature_browser_dock->hide();
  if (_creature_editor_dock) _creature_editor_dock->hide();
  if (_creature_model_picker_dock) _creature_model_picker_dock->hide();
  if (_gameobject_browser_dock) _gameobject_browser_dock->hide();
  if (_gameobject_model_picker_dock) _gameobject_model_picker_dock->hide();
  _texture_picker_dock->hide();
  _texture_browser_dock->hide();
}

MapView::MapView( math::degrees camera_yaw0
                , math::degrees camera_pitch0
                , glm::vec3 camera_pos
                , Noggit::Ui::Windows::NoggitWindow* NoggitWindow
                , std::shared_ptr<Noggit::Project::NoggitProject> Project
                , std::unique_ptr<World> world
                , uid_fix_mode uid_fix
                , bool from_bookmark
                , bool capture_probe
                )
  : _camera (camera_pos, camera_yaw0, camera_pitch0)
  , mTimespeed(0.0f)
  , _uid_fix (uid_fix)
  , _from_bookmark (from_bookmark)
  , _settings (new QSettings (this))
  , cursor_color (1.f, 1.f, 1.f, 1.f)
  , _cursorType{CursorType::CIRCLE}
  , _main_window (NoggitWindow)
  , _world (std::move (world))
  , _status_position (new QLabel (this))
  , _status_selection (new QLabel (this))
  , _status_area (new QLabel (this))
  , _status_time (new QLabel (this))
  , _status_fps (new QLabel (this))
  , _status_culling (new QLabel (this))
  , _status_database(new QLabel(this))
  , _texBrush{new OpenGL::texture{}}
  , _transform_gizmo(Noggit::Ui::Tools::ViewportGizmo::GizmoContext::MAP_VIEW)
  , _tablet_manager(Noggit::TabletManager::instance()),
    _project(Project)
{
  LogDebug << "MapView::MapView begin" << std::endl;
  _capture_probe = capture_probe;
  setWindowTitle ("Noggit Studio Red - " STRPRODUCTVER);
  setFocusPolicy (Qt::StrongFocus);
  setMouseTracking (true);
  setMinimumHeight(200);
  setMaximumHeight(10000);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

  _world->LoadSavedSelectionGroups(); // not doing this in world constructor because noggit loads world twice

  _context = Noggit::NoggitRenderContext::MAP_VIEW;
  _transform_gizmo.setWorld(_world.get());

  _main_window->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  _main_window->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  _main_window->statusBar()->addWidget (_status_position);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_position); }
          );
  _main_window->statusBar()->addWidget (_status_selection);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_selection); }
          );
  _main_window->statusBar()->addWidget (_status_area);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_area); }
          );
  _main_window->statusBar()->addWidget (_status_time);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_time); }
          );
  _main_window->statusBar()->addWidget (_status_fps);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_fps); }
          );
  _main_window->statusBar()->addWidget (_status_culling);
  connect ( this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget (_status_culling); }
  );
  _main_window->statusBar()->addWidget(_status_database);
  connect(this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget(_status_database); }
  );

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
      this, SLOT(ShowContextMenu(const QPoint&)));

  moving = strafing = updown = lookat = turn = 0.0f;

  freelook = false;

  mousedir = -1.0f;

  look = false;
  _display_mode = display_mode::in_3D;

  _startup_time.start();

  int _fps_limit = _settings->value("fps_limit", 60).toInt();
  int _fps_calcul = (int)((1.f / (float)_fps_limit) * 1000.f);
  std::cout << "FPS limit is set to : " << _fps_limit << " (" << _fps_calcul << ")" << std::endl;

  if (!capture_probe)
  {
    // PreciseTimer: the default coarse timer allows ~5% slack, which beat against vsync and
    // made timer-paced frames land unevenly (part of the mouselook jitter)
    _update_every_event_loop.setTimerType(Qt::PreciseTimer);
    _update_every_event_loop.start (_fps_calcul);
    connect(&_update_every_event_loop, &QTimer::timeout,[=]{ _needs_redraw = true; update(); });
  }
  else
  {
    LogDebug << "MapView::MapView capture probe mode: redraw timer disabled" << std::endl;
  }
  LogDebug << "MapView::MapView before createGUI" << std::endl;
  createGUI();
  LogDebug << "MapView::MapView after createGUI" << std::endl;

  // Zone music: a hidden dropdown widget (shown from the toolbar music button) that plays the current
  // zone's background music, switching playlists as the camera crosses zone boundaries (see tick()).
  // Enable/disable lives in the dropdown's own checkbox.
  _zone_music_player = new Noggit::Ui::ZoneMusicPlayer(this);
  _zone_music_player->setVisible(false);
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::MapView end capture_probe=" << _capture_probe
             << " size=" << width() << "x" << height()
             << " needs_redraw=" << _needs_redraw
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
}

void MapView::tabletEvent(QTabletEvent* event)
{
  _tablet_manager->setPressure(event->pressure());
  _tablet_manager->setIsActive(true);
  event->ignore();
}

auto MapView::setBrushTexture(QImage const* img) -> void
{

  int const height{img->height()};
  int const width{img->width()};

  std::vector<std::uint32_t> tex(height * width);

  for(int i{}; i < height; ++i)
    for(int j{}; j < width; ++j)
      tex[i * width + j] = img->pixel(j, i);

  makeCurrent();
  OpenGL::context::scoped_setter const _{gl, context()};
  OpenGL::texture::set_active_texture(4);
  _texBrush->bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MapView::move_camera_with_auto_height (glm::vec3 const& pos)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  TileIndex tile_index = TileIndex(pos);
  if (_world->mapIndex.hasTile(tile_index))
  {
    _world->mapIndex.loadTile(pos)->wait_until_loaded();
  }

  _camera.position = pos;
  _camera.position.y = 0.0f;

  _world->GetVertex (pos.x, pos.z, &_camera.position);

  // min elevation according to https://wowdev.wiki/AreaTable.dbc
  //! \ todo use the current area's MinElevation
  if (_camera.position.y < -5000.0f)
  {
    //! \todo use the height of a model/wmo of the tile (or the map) ?
    _camera.position.y = 0.0f;
  }

  _camera.position.y += 50.0f;

  _camera_moved_since_last_draw = true;
}

void MapView::focus_camera_on_target (glm::vec3 const& target)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  TileIndex tile_index = TileIndex(target);
  if (_world->mapIndex.hasTile(tile_index))
  {
    _world->mapIndex.loadTile(target)->wait_until_loaded();
  }

  float const dist = 28.0f; // viewing distance from the target (tune)
  float const k = 0.70710678f; // cos/sin of 45 degrees

  // Approach from the current horizontal facing so the jump isn't jarring; back + up at 45 degrees.
  glm::vec3 dir = _camera.direction();
  glm::vec3 horiz (dir.x, 0.0f, dir.z);
  if (glm::length(horiz) < 0.001f)
  {
    horiz = glm::vec3(0.0f, 0.0f, 1.0f);
  }
  horiz = glm::normalize(horiz);

  glm::vec3 const eye = target - horiz * (dist * k) + glm::vec3(0.0f, dist * k, 0.0f);
  _camera.position = eye;

  // Aim at the target. direction() = (cos(pitch)*sin(yaw), -sin(pitch), cos(pitch)*cos(yaw)).
  glm::vec3 const d = glm::normalize(target - eye);
  _camera.yaw(math::degrees(glm::degrees(std::atan2(d.x, d.z))));
  _camera.pitch(math::degrees(glm::degrees(-std::asin(glm::clamp(d.y, -1.0f, 1.0f)))));

  _camera_moved_since_last_draw = true;
}

void MapView::on_uid_fix_fail()
{
  emit uid_fix_failed();

  _uid_fix_failed = true;
  deleteLater();
}

void MapView::initializeGL()
{
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL begin capture_probe=" << _capture_probe
             << " size=" << width() << "x" << height()
             << " pos=(" << _camera.position.x << ", " << _camera.position.y << ", " << _camera.position.z << ")"
             << std::endl;
  }

  bool uid_warning = false;

  // [VULKAN 2026-08-29] Settings > Graphics > "Graphics API" / "Vulkan parity check" -> backend gates
  // (read before the first draw; the draw_map statics latch these on the first frame).
  if (!vk_diff::forced()) // (--vk-parity-* runs pre-set these and must not be overridden by QSettings)
  {
    vk_diff::apiMode() = std::min(1, _settings->value("render/graphics_api", 0).toInt()); // UI: 0 GL / 1 VK (2 = env-only dev)
    // Dev override for the benchmark: it has to force GL and VK in turn without writing to (or
    // depending on) whatever the user has saved in Settings.
    if (char const* bench_api = std::getenv("NOGGIT_BENCH_API"))
      vk_diff::apiMode() = std::atoi(bench_api);
    vk_diff::parityCheck() = _settings->value("render/vk_parity_check", false).toBool();
    // The benchmark measures the REAL VK mode, where the ownership gates take work off the GL side.
    // Parity check forces GL to keep drawing everything (it is the reference image), so leaving it on
    // would benchmark the scene being drawn twice.
    if (std::getenv("NOGGIT_VK_BENCH"))
      vk_diff::parityCheck() = false;
    vk_diff::camsPath() = (QCoreApplication::applicationDirPath() + "/vk_diff_cams.txt").toStdString();
  }
  if (vk_diff::apiMode() != 0)
  {
    LogDebug << "[VK] graphics_api=" << vk_diff::apiMode() << " parity_check=" << vk_diff::parityCheck()
             << " cams=" << vk_diff::camsPath() << std::endl;
  }

  OpenGL::context::scoped_setter const _ (::gl, context());

  gl.viewport(0.0f, 0.0f, width(), height());

  gl.clearColor (0.0f, 0.0f, 0.0f, 1.0f);

  if (_uid_fix == uid_fix_mode::max_uid)
  {
    _world->mapIndex.searchMaxUID();
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fail_on_model_loading_error)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), true);

    if (result == uid_fix_status::failed)
    {
      on_uid_fix_fail();
      return;
    }
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fuckporting_edition)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), false);

    uid_warning = result == uid_fix_status::done_with_errors;
  }

  _uid_fix = uid_fix_mode::none;

  if (!_from_bookmark)
  {
    move_camera_with_auto_height (_camera.position);
  }

  if (uid_warning)
  {
    QMessageBox::warning
      ( nullptr
      , "UID Warning"
      , "Some models were missing or couldn't be loaded. "
        "This will lead to culling (visibility) errors in game\n"
        "It is recommended to fix those models (listed in the log file) and run the uid fix all again."
      , QMessageBox::Ok
      );
  }

  _imgui_context = QtImGui::initialize(this);

  emit resized();

  _last_opengl_context = context();

  _world->renderer()->upload();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL after renderer upload" << std::endl;
  }
  onSettingsSave();

  _buffers.upload();

  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[0], 4, nullptr, GL_DYNAMIC_READ);
  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[1], 4, nullptr, GL_DYNAMIC_READ);

  connect(context(), &QOpenGLContext::aboutToBeDestroyed, [this](){ emit aboutToLooseContext(); });

  _gl_initialized = true;
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::initializeGL end" << std::endl;
  }
}


void MapView::saveMinimap(MinimapRenderSettings* settings)
{

  OpenGL::context::scoped_setter const _ (::gl, context());

  bool mmap_render_success = false;

  static QProgressBar* progress;
  static QPushButton* cancel_btn;

  switch (settings->export_mode)
  {
    case MinimapGenMode::CURRENT_ADT:
    {
      TileIndex tile = TileIndex(_camera.position);

      if (_world->mapIndex.hasTile(tile))
      {
        mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);
      }

      if (mmap_render_success)
      {
        _world->mapIndex.saveMinimapMD5translate();
      }

      saving_minimap = false;

      break;
    }
    case MinimapGenMode::MAP:
    {

      // init progress
      if (!_mmap_async_index)
      {
        progress = new QProgressBar(nullptr);
        progress->setMinimum(0);
        progress->setMaximum(_world->mapIndex.getNumExistingTiles());
        _main_window->statusBar()->addPermanentWidget(progress);

        cancel_btn = new QPushButton(nullptr);
        cancel_btn->setText("Cancel");

        connect(cancel_btn, &QPushButton::clicked, 
          [=, this] 
          { 
            _mmap_async_index = 0; 
            _mmap_render_index = 0; 
            saving_minimap = false;
            progress->deleteLater(); 
            cancel_btn->deleteLater();
            _mmap_combined_image.reset();
          });

        _main_window->statusBar()->addPermanentWidget(cancel_btn);

        connect(this, &MapView::updateProgress,
                [=](int value)
                {

                  progress->setValue(value);
                });
      
        // setup combined image if necessary
        if (settings->combined_minimap)
        {
          _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
          _mmap_combined_image->fill(Qt::black);
        }
      
      }

      if (!saving_minimap)
        return;

      if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
      {
        TileIndex tile = TileIndex(_mmap_async_index / 64, _mmap_async_index % 64);

        if (_world->mapIndex.hasTile(tile))
        {
          OpenGL::context::scoped_setter const _(::gl, context());
          makeCurrent();
          mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);

          _mmap_render_index++;
          emit updateProgress(_mmap_render_index);

          if (!mmap_render_success)
          {
            LogError << "Minimap rendered incorrectly for tile: " << tile.x << "_" << tile.z << std::endl;
          }
        }

        _mmap_async_index++;
      }
      else
      {
        _mmap_async_index = 0;
        _mmap_render_index = 0;
        saving_minimap = false;
        progress->deleteLater();
        cancel_btn->deleteLater();
        _world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
          QString image_path = QString(std::string(_world->basename + "_combined_minimap.png").c_str());
          QSettings app_settings;
          QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());;
          if (!(str.endsWith('\\') || str.endsWith('/')))
          {
            str += "/";
          }

          QDir dir(str + "/textures/minimap/");
          if (!dir.exists())
            dir.mkpath(".");

          _mmap_combined_image->save(dir.filePath(image_path));
          _mmap_combined_image.reset();
        }
      
      }

      //_main_window->statusBar()->showMessage("Minimap rendering done.", 2000);
      break;
    } 
    case MinimapGenMode::SELECTED_ADTS:
    {
      auto selected_tiles = minimapTool->getSelectedTiles();

      // init progress
      if (!_mmap_async_index)
      {
        progress = new QProgressBar(nullptr);
        progress->setMinimum(0);

        unsigned n_selected_tiles = 0;

        for (int i = 0; i < 4096; ++i)
        {
          if (selected_tiles->at(i))
            n_selected_tiles++;
        }

        progress->setMaximum(n_selected_tiles);
        _main_window->statusBar()->addPermanentWidget(progress);

        cancel_btn = new QPushButton(nullptr);
        cancel_btn->setText("Cancel");

        connect(cancel_btn, &QPushButton::clicked,
          [=, this]
          {
            _mmap_async_index = 0;
            _mmap_render_index = 0;
            saving_minimap = false;
            progress->deleteLater();
            cancel_btn->deleteLater();
            _mmap_combined_image.reset();
          });

        _main_window->statusBar()->addPermanentWidget(cancel_btn);

        connect(this, &MapView::updateProgress,
                [=](int value)
                {
                  // This weirdness is required due to a bug on Linux when QT repaint crashes due to too many events
                  // being passed through. TODO: this potentially only masks the issue, which may reappear on faster
                  // hardware.
                  if (progress->value() != value)
                    progress->setValue(value);
                });

        // setup combined image if necessary
        if (settings->combined_minimap)
        {
          _mmap_combined_image.emplace(8192, 8192, QImage::Format_RGBA8888);
          _mmap_combined_image->fill(Qt::black);
        }
      
      }

      if (!saving_minimap)
        return;


      if (_mmap_async_index < 4096 && static_cast<int>(_mmap_render_index) < progress->maximum())
      {
        if (selected_tiles->at(_mmap_async_index))
        {
          TileIndex tile = TileIndex(_mmap_async_index / 64, _mmap_async_index % 64);

          if (_world->mapIndex.hasTile(tile))
          {
            mmap_render_success = _world->renderer()->saveMinimap(tile, settings, _mmap_combined_image);
            _mmap_render_index++;

            emit updateProgress(_mmap_render_index);


            if (!mmap_render_success)
            {
              LogError << "Minimap rendered incorrectly for tile: " << tile.x << "_" << tile.z << std::endl;
            }
          }
        }
        _mmap_async_index++;

      }
      else
      {
        _mmap_async_index = 0;
        _mmap_render_index = 0;
        saving_minimap = false;
        progress->deleteLater();
        cancel_btn->deleteLater();
        _world->mapIndex.saveMinimapMD5translate();

        // save combined minimap
        if (settings->combined_minimap)
        {
          QString image_path = QString(std::string(_world->basename + "_combined_minimap.png").c_str());
          QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
          if (!(str.endsWith('\\') || str.endsWith('/')))
          {
            str += "/";
          }

          QDir dir(str + "/textures/minimap/");
          if (!dir.exists())
            dir.mkpath(".");

          _mmap_combined_image->save(dir.filePath(image_path));
          _mmap_combined_image.reset();
        }
     
      }

      break;
     
    }
  }

  //minimapTool->progressUpdate(0);
}

void MapView::paintGL()
{
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL entered capture_probe=" << _capture_probe
             << " needs_redraw=" << _needs_redraw
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
  static bool lock = false;

  if (lock)
    return;

  // Live "Character models" fidelity (Settings): the spawns resolve their model + bake through the
  // value, so re-resolve them when it changes. Polled every 30 frames; the reload runs after this paint.
  {
    static int fidelity_seen = -1;
    static int fidelity_poll = 0;
    if (++fidelity_poll >= 30)
    {
      fidelity_poll = 0;
      int const fidelity = QSettings().value("render/character_model_fidelity", 0).toInt();
      if (fidelity_seen != -1 && fidelity != fidelity_seen && _world)
      {
        QTimer::singleShot(0, this, [this] { refreshCreatureSpawnOverlay(true); });
      }
      fidelity_seen = fidelity;
    }
  }

  if (!_needs_redraw)
    return;
  else
    _needs_redraw = false;

  // [GL-COST HUNT] paintGL split into [before draw_map] / [draw_map] / [after draw_map]; post is
  // derived from the FRAME SPLIT total. VK's paintGL costs ~3.9 ms more than GL's and only ~1 ms of
  // that is inside the instrumented VK block.
  auto const t_pg_start = std::chrono::steady_clock::now();

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL after redraw gate" << std::endl;
  }

  if (!_gl_initialized)
  {
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before initializeGL" << std::endl;
    }
    initializeGL();
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after initializeGL gl_initialized=" << _gl_initialized << std::endl;
    }
  }

  if (_last_opengl_context != context())
  {
    _gl_initialized = false;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL context changed, skipping frame" << std::endl;
    }
    return;
  }

  const qreal now(_startup_time.elapsed() / 1000.0);

  _last_frame_durations.emplace_back (now - _last_update);

  // minimap rendering
  if (saving_minimap)
  {
    OpenGL::context::scoped_setter const _(::gl, context());
    makeCurrent();
    _camera_moved_since_last_draw = true;
    lock = true;
    saveMinimap(minimapTool->getMinimapRenderSettings());
    lock = false;
    return;
  }

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before context setter" << std::endl;
  }
  OpenGL::context::scoped_setter const _(::gl, context());
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before makeCurrent" << std::endl;
  }
  makeCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::paintGL before clear" << std::endl;
  }

  // [game mode] clamp the camera boom against THIS frame's look direction before any matrix is
  // built -- fast mouse swings between game ticks otherwise render 1-2 frames inside geometry
  clampGameBoomPreDraw();
  // [both modes] the camera never sits INSIDE the water-surface band: snap out/in so an edge-on
  // surface view (above-water frame with a submerged half and no effect) cannot exist.
  snapCameraOffWaterSurface();

  // [unaccounted bisect 2026-08-07] PaintBody = this makeCurrent..paintGL-return span (function scope, RAII
  // handles early returns). Frame is start..start; Frame-PaintBody = time Qt spends BETWEEN paintGL calls
  // (buffer swap, QOpenGLWidget compositor blit, event-loop processing, any loader/GPU-upload callback).
  // The spike line prints betweenFrame vs inPaintUnprof so a 500ms hitch is localized to one side in one run.
  noggit::perf::Scoped _prof_paint_body(noggit::perf::Phase::PaintBody);

  gl.clear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  {
    // [perf] frame-to-frame total + throttled per-phase report (NOGGIT_FRAME_PROFILE=1). No-op when off.
    static std::chrono::steady_clock::time_point s_prof_last;
    static bool s_prof_have = false;
    auto& _prof = noggit::perf::FrameProfiler::get();
    auto const _prof_now = std::chrono::steady_clock::now();
    if (s_prof_have)
      _prof.add(noggit::perf::Phase::Frame,
                std::chrono::duration<double, std::milli>(_prof_now - s_prof_last).count());
    s_prof_last = _prof_now;
    s_prof_have = true;
    _prof.end_frame();

    // [mem-diag 2026-08-04] Once/sec, log process working set + loaded-asset counts so a memory climb can
    // be attributed: models/textures/wmos growing = assets not freeing; working set climbing while those
    // stay flat = a GPU/loader-side leak. NOGGIT_FRAME_PROFILE gate (already on in the launcher).
    if (_prof.on)
    {
      static std::chrono::steady_clock::time_point s_mem_last;
      static bool s_mem_have = false;
      if (!s_mem_have || std::chrono::duration<double, std::milli>(_prof_now - s_mem_last).count() > 1000.0)
      {
        s_mem_last = _prof_now;
        s_mem_have = true;
        std::size_t ws_mb = 0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) // kernel32-exported, no psapi.lib
          ws_mb = pmc.WorkingSetSize / (1024u * 1024u);
#endif
        // VRAM readout (NVIDIA GL_NVX_gpu_memory_info / AMD GL_ATI_meminfo). Vendor-gated so we never issue
        // an unsupported enum (that would trip the once-per-frame GL error check). used=dedicated-available.
        // evict/s (NVIDIA) = the driver paging VRAM<->RAM over PCIe -- the direct signal for a VRAM-pressure
        // loading STUTTER: if it climbs while flying into new tiles, we're VRAM-capacity-bound.
        static int s_vram_vendor = -2; // -2 uninit, -1 none/unknown, 0 nvidia, 1 amd
        if (s_vram_vendor == -2)
        {
          s_vram_vendor = -1;
          if (GLubyte const* v = gl.getString(GL_VENDOR))
          {
            std::string vs(reinterpret_cast<char const*>(v));
            std::transform(vs.begin(), vs.end(), vs.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            if (vs.find("nvidia") != std::string::npos) s_vram_vendor = 0;
            else if (vs.find("ati") != std::string::npos || vs.find("amd") != std::string::npos
                     || vs.find("radeon") != std::string::npos) s_vram_vendor = 1;
          }
        }
        std::string vram;
        if (s_vram_vendor == 0)
        {
          constexpr GLenum NVX_DEDICATED = 0x9047, NVX_AVAILABLE = 0x9049, NVX_EVICTION_COUNT = 0x904A, NVX_EVICTED = 0x904B;
          GLint ded = 0, avail = 0, evc = 0, evm = 0;
          gl.getIntegerv(NVX_DEDICATED, &ded);
          gl.getIntegerv(NVX_AVAILABLE, &avail);
          gl.getIntegerv(NVX_EVICTION_COUNT, &evc);
          gl.getIntegerv(NVX_EVICTED, &evm);
          static GLint s_prev_evc = 0;
          GLint const evc_delta = (s_prev_evc == 0) ? 0 : (evc - s_prev_evc); // per-report evictions = stutter rate
          s_prev_evc = evc;
          std::ostringstream os;
          os << " vram=" << ((ded - avail) / 1024) << "/" << (ded / 1024) << "MB"
             << " evict/s=" << evc_delta << " evictedNow=" << (evm / 1024) << "MB";
          vram = os.str();
        }
        else if (s_vram_vendor == 1)
        {
          constexpr GLenum ATI_TEXTURE_FREE_MEMORY = 0x87FC;
          GLint info[4] = {0, 0, 0, 0}; // [total_free, largest_free_block, total_aux_free, largest_aux_free] KB
          gl.getIntegerv(ATI_TEXTURE_FREE_MEMORY, info);
          std::ostringstream os;
          os << " vramFree=" << (info[0] / 1024) << "MB";
          vram = os.str();
        }
        LogError << "[MEM] workingSet=" << ws_mb << "MB"
                 << " models=" << ModelManager::loaded_count()
                 << " textures=" << TextureManager::loaded_count()
                 << " wmos=" << WMOManager::loaded_count()
                 << " tiles=" << (_world ? _world->mapIndex.getNLoadedTiles() : 0u)
                 << vram
                 << std::endl;
      }
    }
  }

  if (!saving_minimap)
  {
    lock = true;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before draw_map" << std::endl;
    }
    auto const t_dm0 = std::chrono::steady_clock::now();
    vk_stat_pg_pre_ms() += std::chrono::duration<double, std::milli>(t_dm0 - t_pg_start).count();
    draw_map();
    vk_stat_pg_map_ms() += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t_dm0).count();
    {
      // [perf] GPU-boundedness probe (NOGGIT_FRAME_PROFILE only): time a glFinish right after the render.
      // GpuWait ~= how long the CPU must WAIT for the GPU to finish the frame's draws beyond what already
      // overlapped the CPU submit. Large GpuWait => GPU-BOUND (GPU render time ~= WorldDraw + GpuWait);
      // ~0 => CPU-bound. Intrusive (it serializes CPU<->GPU), so it only runs while profiling.
      auto& _prof_gpu = noggit::perf::FrameProfiler::get();
      if (_prof_gpu.on)
      {
        auto const _gt0 = std::chrono::steady_clock::now();
        gl.finish();
        _prof_gpu.add(noggit::perf::Phase::GpuWait,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _gt0).count());
      }
    }
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after draw_map" << std::endl;
    }
    lock = false;
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL before tick" << std::endl;
    }
    {
      // [perf] time tick() into the (otherwise-unused) Overlays bucket so the frame-to-frame gap can be
      // split: Frame - WorldDraw - Overlays = pure Qt event-loop/compositor cost outside our draw code.
      noggit::perf::Scoped _prof_tick(noggit::perf::Phase::Overlays);
      tick (now - _last_update);
    }
    if (capture_debug_enabled())
    {
      LogDebug << "MapView::paintGL after tick" << std::endl;
    }
  }

  _last_update = now;


  if (_gizmo_on.get() && _world->has_selection())
  {
    ImGui::SetCurrentContext(_imgui_context);
    QtImGui::newFrame();

    static bool is_open = false;
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowPos(ImVec2(-100.f, -100.f));
    ImGui::Begin("Gizmo", &is_open, ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar
                                                | ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground);

    auto mv = model_view();
    auto proj = projection();

    _transform_gizmo.setCurrentGizmoOperation(_gizmo_operation);
    _transform_gizmo.setCurrentGizmoMode(_gizmo_mode);
    _transform_gizmo.setUseMultiselectionPivot(_use_median_pivot_point.get());

    auto pivot = _world->multi_select_pivot().has_value() ?
        _world->multi_select_pivot().value() : glm::vec3(0.f, 0.f, 0.f);

    _transform_gizmo.setMultiselectionPivot(pivot);

    _transform_gizmo.handleTransformGizmo(this, _world->current_selection(), mv, proj);

    _world->update_selection_pivot();

    ImGui::End();

    /* Example
    std::string sText;

    if(ImGui::IsMouseClicked( 1 ) )
    {
      ImGui::OpenPopup( "PieMenu" );
    }

    if( BeginPiePopup( "PieMenu", 1 ) )
    {
      if( PieMenuItem( "Test1" ) ) sText = "Test1";
      if( PieMenuItem( "Test2" ) )
      {
        sText = "Test2";
      }
      if( PieMenuItem( "Test3", false ) ) sText = "Test3";
      if( BeginPieMenu( "Sub" ) )
      {
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }
      if( BeginPieMenu( "Sub2" ) )
      {
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }

      EndPiePopup();
    }

   */

    //ImGui::ShowDemoWindow();
    //ImGui::ShowStyleEditor();

    ImGui::Render();

  }

  if (!saving_minimap && _world->uid_duplicates_found() && !_uid_duplicate_warning_shown)
  {
    _uid_duplicate_warning_shown = true;
    LogError << "Duplicate object UIDs were found while loading this map. "
             << "Noggit reassigned duplicates for this editor session; run a max UID check before saving shared edits."
             << std::endl;
  }

  // [harness] Capture HERE, in the same scope that drew the frame -- see _pending_screenshot.
  if (!_pending_screenshot.empty())
  {
    std::string const path = _pending_screenshot;
    _pending_screenshot.clear();
    captureFrameNow(path);
  }

  FrameMark
}

void MapView::resizeGL (int width, int height)
{
  OpenGL::context::scoped_setter const _ (::gl, context());
  gl.viewport(0.0f, 0.0f, width, height);
  emit resized();
#ifdef _WIN32
  // [VULKAN NATIVE PRESENT] keep the present child window glued to the viewport
  if (_vk_present_container)
  {
    _vk_present_container->setGeometry(rect());
    raiseViewportOverlayWidgets();   // panels built lazily after the surface must stay on top
  }
  if (g_vk_capture_backend)
    g_vk_capture_backend->notifyPresentResize();
#endif
  _camera_moved_since_last_draw = true;
  _needs_redraw = true;
}


MapView::~MapView()
{
  makeCurrent();

  _destroying = true;

#ifdef _WIN32
  // [VULKAN NATIVE PRESENT] the HWND dies with this widget; a recycled handle value must never
  // stay hit-test-transparent for some future window
  if (_vk_present_window)
    vkPresentHitFilter().hwnds.erase(reinterpret_cast<void*>(_vk_present_window->winId()));
#endif

  // AUDIO TEARDOWN (user 2026-08-27: "water sound gets stuck playing even after changing map").
  // ZoneMusicPlayer is a child widget and stops itself in its destructor, but WaterSoundPlayer is
  // a process-wide singleton fed only from this view's tick -- once the view dies nothing updates
  // or silences it, so its loop channels kept playing into the next map / the menu.
  Noggit::Ui::WaterSoundPlayer::instance().stop_all();

  // Make teardown single-threaded BEFORE destroying the world. The async loader threads finish
  // model/WMO loads in the background, and completing a load can still feed SceneObject instances into
  // MapTile::object_instances (under the tile mutex). World destruction (~World -> ~MapTile) iterates
  // object_instances WITHOUT that lock and calls instance->derefTile(); if a loader thread mutates the
  // tile mid-iteration the instance pointer is garbage -> access violation in SceneObject::derefTile.
  // This is exactly the crash when leaving a still-streaming zone like Ironforge. Waiting until the
  // loader is idle quiesces the background so the destruction below runs with no concurrent mutation.
  // (finishLoading is CPU-side file parsing; GL upload is deferred to first draw, so this can't deadlock
  // on the now-stopped render loop.)
  AsyncLoader::instance().wait_until_idle();

  OpenGL::context::scoped_setter const _ (::gl, context());
  // Force the GPU fully idle BEFORE deleting any GL resource below (and in ~World via _world.reset()).
  // Returning to menu / switching maps crashed inside the NVIDIA GL driver (nvoglv64.dll, __fastfail):
  // the last rendered frame's draws were still in flight referencing textures/buffers that teardown then
  // deleted, so the driver touched freed objects. glFinish blocks until all queued GL commands complete,
  // so nothing the deletes free is still in use. (The per-call glGetError sync used to hide this; this is
  // the explicit, sync-setting-independent fix.)
  gl.finish();
  delete _texBrush;
  delete _viewport_overlay_ui;

  // when the uid fix fail the UI isn't created
  if (!_uid_fix_failed)
  {
    delete TexturePicker; // explicitly delete this here to avoid opengl context related crash
    delete objectEditor;
    delete texturingTool;
  }
  
  if (_force_uid_check)
  {
    uid_storage::remove_uid_for_map(_world->getMapID());
  }

  _world.reset();

  AsyncLoader::instance().reset_object_fail();

  Noggit::Ui::selected_texture::texture.reset();

  ModelManager::report();
  TextureManager::report();
  WMOManager::report();

  NOGGIT_ACTION_MGR->disconnect();

  _buffers.unload();

}

void MapView::tick (float dt)
{
	_mod_shift_down = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
	_mod_ctrl_down = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
	_mod_alt_down = QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
	_mod_num_down = QApplication::keyboardModifiers().testFlag(Qt::KeypadModifier);

	unsigned action_modality = 0;
	if (_mod_shift_down)
    action_modality |= Noggit::ActionModalityControllers::eSHIFT;
	if (_mod_ctrl_down)
    action_modality |= Noggit::ActionModalityControllers::eCTRL;
  if (_mod_alt_down)
    action_modality |= Noggit::ActionModalityControllers::eALT;
  if (_mod_num_down)
    action_modality |= Noggit::ActionModalityControllers::eNUM;
  if (_mod_space_down)
    action_modality |= Noggit::ActionModalityControllers::eSPACE;
  if (leftMouse)
    action_modality |= Noggit::ActionModalityControllers::eLMB;
  if (rightMouse)
    action_modality |= Noggit::ActionModalityControllers::eRMB;
  if (MoveObj)
    action_modality |= Noggit::ActionModalityControllers::eMMB;

  NOGGIT_ACTION_MGR->endActionOnModalityMismatch(action_modality);

  // start unloading tiles
  // [perf 2026-08-05] Pinpoint the tick (Overlays) stall: time enterTile (loadTile ctor+queue) vs
  // unloadTiles (tile free -> ~MapTile -> ensure_deletable waits on in-flight loads) separately.
  if (noggit::perf::FrameProfiler::get().on)
  {
    auto const t0 = std::chrono::steady_clock::now();
    _world->mapIndex.enterTile (TileIndex (_camera.position));
    auto const t1 = std::chrono::steady_clock::now();
    _world->mapIndex.unloadTiles (TileIndex (_camera.position));
    auto const t2 = std::chrono::steady_clock::now();
    double const enter_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double const unload_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    if (enter_ms > 15.0 || unload_ms > 15.0)
    {
      LogError << "[TICK-STALL] enterTile=" << enter_ms << "ms unloadTiles=" << unload_ms << "ms" << std::endl;
    }
  }
  else
  {
    _world->mapIndex.enterTile (TileIndex (_camera.position));
    _world->mapIndex.unloadTiles (TileIndex (_camera.position));
  }

  dt = std::min(dt, 1.0f);

  auto cur_action = NOGGIT_CUR_ACTION;

  if ((cur_action && !cur_action->getBlockCursor()) || !cur_action)
  {
    if (_locked_cursor_mode.get())
    {
      switch (terrainMode)
      {
        case editing_mode::areaid:
        case editing_mode::flags:
        case editing_mode::holes:
        case editing_mode::object:
          update_cursor_pos();
          break;
        default:
          break;
      }
    }
    else
    {
      update_cursor_pos();
    }
  }

  math::degrees yaw (-_camera.yaw()._);

  glm::vec3 dir(1.0f, 0.0f, 0.0f);
  glm::vec3 dirUp(1.0f, 0.0f, 0.0f);
  glm::vec3 dirRight(0.0f, 0.0f, 1.0f);
  math::rotate(0.0f, 0.0f, &dir.x, &dir.y, _camera.pitch());
  math::rotate(0.0f, 0.0f, &dir.x, &dir.z, yaw);

  if (_mod_ctrl_down)
  {
    dirUp.x = 0.0f;
    dirUp.y = 1.0f;
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z,yaw);
  }
  else if(!_mod_shift_down)
  {
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z, yaw);
  }

  auto currentSelection = _world->current_selection();
  if (_world->has_selection())
  {
    // update rotation editor if the selection has changed
    if (lastSelected != currentSelection)
    {
      _rotation_editor_need_update = true;
    }

    if (terrainMode == editing_mode::object)
    {
      // reset numpad_moveratio when no numpad key is pressed
      if (!(keyx != 0 || keyy != 0 || keyz != 0 || keyr != 0 || keys != 0))
      {
        numpad_moveratio = 0.5f;
      }
      else // Set move scale and rotate for numpad keys
      {
        if (_mod_ctrl_down && _mod_shift_down)
        {
          numpad_moveratio += 0.5f;
        }
        else if (_mod_shift_down)
        {
          numpad_moveratio += 0.05f;
        }
        else if (_mod_ctrl_down)
        {
          numpad_moveratio += 0.005f;
        }
      }

      if (keys != 0.f)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
        _world->scale_selected_models(keys*numpad_moveratio / 50.f, World::m2_scaling_type::add);
        // NOGGIT_ACTION_MGR->endAction();
        _rotation_editor_need_update = true;
      }
      if (keyr != 0.f)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
        _world->rotate_selected_models( math::degrees(0.f)
                                      , math::degrees(keyr * numpad_moveratio * 5.f)
                                      , math::degrees(0.f)
                                      , _use_median_pivot_point.get()
                                      );
        _rotation_editor_need_update = true;
      }

      if (MoveObj)
      {
        if (_mod_alt_down)
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eALT
                                                         | Noggit::ActionModalityControllers::eMMB );
          _world->scale_selected_models(std::pow(2.f, mv*4.f), World::m2_scaling_type::mult);
        }
        else if (_mod_shift_down)
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eSHIFT
                                                         | Noggit::ActionModalityControllers::eMMB );
          _world->move_selected_models(0.f, mv*80.f, 0.f);
        }
        else if (_mod_ctrl_down)
        {
            // do nothing
        }
        else
        {
          bool snapped = false;
          bool snapped_to_object = false;
          if (_world->has_multiple_model_selected())
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                                 Noggit::ActionModalityControllers::eMMB );
            _world->set_selected_models_pos(_cursor_pos, false);

            if (_snap_multi_selection_to_ground.get())
            {
              snap_selected_models_to_the_ground();
              snapped = true;
            }
          }
          else
          {
            if (!_move_model_to_cursor_position.get())
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                                   Noggit::ActionModalityControllers::eMMB );

              if ((mh <= 0.01f && mh >= -0.01f) && (mv <= 0.01f && mv >= -0.01f))
              {
                  glm::vec3 _vec = (mh * dirUp + mv * dirRight);
                  _world->move_selected_models(_vec * 500.f);
              }
            }
            else
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                             Noggit::ActionModalityControllers::eMMB );

              if (_move_model_to_cursor_position.get() || _move_model_snap_to_objects.get())
              {
                selection_result results(intersect_result(false));

                if (!results.empty())
                {
                    for (auto result = results.begin(); result != results.end(); result++)
                    {
                        auto const& hit(result->second);
                        bool is_selected_model = false;

                        // if a terrain is found first use that (terrain cursor pos position updated on move already)
                        if (hit.index() == eEntry_MapChunk && _move_model_to_cursor_position.get())
                        {
                            break;
                        }

                        if (hit.index() == eEntry_Object && _move_model_snap_to_objects.get())
                        {
                            auto obj_hit = std::get<selected_object_type>(hit);
                            auto obj_hit_type = obj_hit->which();

                            // don't snap to animated models
                            if (obj_hit_type == eMODEL)
                            {
                                auto m2_model_hit = static_cast<ModelInstance*>(obj_hit);
                                if (m2_model_hit->model->animated_mesh())
                                    continue;
                            }

                            // find and ignore current object/selected models or it will keep snaping to itself
                            for (auto& entry : _world->current_selection())
                            {
                                auto type = entry.index();
                                if (type == eEntry_Object)
                                {
                                    auto& selection_obj = std::get<selected_object_type>(entry);
                                    if (selection_obj->uid == obj_hit->uid)
                                    {
                                        is_selected_model = true;
                                        break;
                                    }
                                }
                            }
                            if (is_selected_model)
                                continue;
                            auto hit_pos = intersect_ray().position(result->first);
                            _cursor_pos = hit_pos;
                            snapped_to_object = true;
                            // TODO : rotate objects to objects normal
                            // if (_rotate_doodads_along_doodads.get())
                            //    _world->rotate_selected_models_to_object_normal(_rotate_along_ground_smooth.get(), obj_hit, hit_pos, glm::transpose(model_view()), _rotate_doodads_along_wmos.get());
                            break;
                        }
                    }
                }
                _world->set_selected_models_pos(_cursor_pos, false);
                snapped = true;
              }
            }
          }

          if (snapped && _rotate_along_ground.get())
          {
            if (!snapped_to_object)
              _world->rotate_selected_models_to_ground_normal(_rotate_along_ground_smooth.get());

            if (_rotate_along_ground_random.get())
            {
              float minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;

              if (_settings->value("model/random_rotation", false).toBool())
              {
                minY = _object_paste_params.minRotation;
                maxY = _object_paste_params.maxRotation;
              }

              if (_settings->value("model/random_tilt", false).toBool())
              {
                minX = _object_paste_params.minTilt;
                maxX = _object_paste_params.maxTilt;
                minZ = minX;
                maxZ = maxX;
              }

              _world->rotate_selected_models_randomly(
                  minX,
                  maxX,
                  minY,
                  maxY,
                  minZ,
                  maxZ);

              if (_settings->value("model/random_size", false).toBool())
              {
                float min = _object_paste_params.minScale;
                float max = _object_paste_params.maxScale;

                _world->scale_selected_models(misc::randfloat(min, max), World::m2_scaling_type::set);
              }
            }
          }


        }

        _rotation_editor_need_update = true;
      }

      /* TODO: Numpad for action system
      if (keyx != 0.f || keyy != 0.f || keyz != 0.f)
      {
        _world->move_selected_models(keyx * numpad_moveratio, keyy * numpad_moveratio, keyz * numpad_moveratio);
        _rotation_editor_need_update = true;
      }
       */

      if (look)
      {
        if (_mod_ctrl_down) // X
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eCTRL
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(rh + rv)
                                        , math::degrees(0.f)
                                        , math::degrees(0.f)
                                        , _use_median_pivot_point.get()
                                        );
        }
        if (_mod_shift_down) // Y
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eSHIFT
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(0.f)
                                        , math::degrees(rh + rv)
                                        , math::degrees(0.f)
                                        , _use_median_pivot_point.get()
                                        );
        }
        if (_mod_alt_down) // Z
        {
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED,
                                                         Noggit::ActionModalityControllers::eALT
                                                         | Noggit::ActionModalityControllers::eRMB );
          _world->rotate_selected_models( math::degrees(0.f)
                                        , math::degrees(0.f)
                                        , math::degrees(rh + rv)
                                        , _use_median_pivot_point.get()
                                        );
        }

        _rotation_editor_need_update = true;
      }
    }

    for (auto& selection : currentSelection)
    {
      if (selection.index() == eEntry_MapChunk && terrainMode == editing_mode::scripting)
      {
        scriptingTool->sendBrushEvent(_cursor_pos, 7.5f * dt);
      }

      if (leftMouse && selection.index() == eEntry_MapChunk)
      {
        bool underMap = _world->isUnderMap(_cursor_pos);
        auto cur_action = NOGGIT_CUR_ACTION;

        switch (terrainMode)
        {
        case editing_mode::ground:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            auto mask_selector = terrainTool->getImageMaskSelector();

            if (_mod_shift_down && (!mask_selector->isEnabled() || mask_selector->getBrushMode()))
            {
              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTerrainRotation);

              terrainTool->changeTerrain(_world.get(), _cursor_pos, 7.5f * dt);
            }
            else if (_mod_ctrl_down && (!mask_selector->isEnabled() || mask_selector->getBrushMode()))
            {
              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTerrainRotation);

              terrainTool->changeTerrain(_world.get(), _cursor_pos, -7.5f * dt);
            }
          }
          break;
        case editing_mode::flatten_blur:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              flattenTool->flatten(_world.get(), _cursor_pos, dt);
            }
            else if (_mod_ctrl_down)
            {

              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              flattenTool->blur(_world.get(), _cursor_pos, dt);
            }
          }
          break;
        case editing_mode::paint:
          if (_mod_shift_down && _mod_ctrl_down && _mod_alt_down)
          {
            // clear chunk texture
            if (!underMap)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eALT
                                                             | Noggit::ActionModalityControllers::eLMB);

              _world->eraseTextures(_cursor_pos);
            }
          }
          else if (_mod_ctrl_down && !ui_hidden)
          {
            // Pick texture
            _texture_picker_dock->setVisible(true);
            TexturePicker->setMainTexture(texturingTool->_current_texture);
            TexturePicker->getTextures(selection);
          }
          else  if (_mod_shift_down && !!Noggit::Ui::selected_texture::get())
          {
            if ((_display_mode == display_mode::in_3D && !underMap) || _display_mode == display_mode::in_2D)
            {
              auto image_mask_selector = texturingTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
              && texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
              && image_mask_selector->isEnabled()
              && !image_mask_selector->getBrushMode())
                break;

              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE,
                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeTexturingRotation);

              if (texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              texturingTool->paint(_world.get(), _cursor_pos, dt, *Noggit::Ui::selected_texture::get());
            }
          }
          break;

        case editing_mode::holes:
          // no undermap check here, else it's impossible to remove holes
          if (_mod_shift_down)
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES,
                                                           Noggit::ActionModalityControllers::eSHIFT
                                                           | Noggit::ActionModalityControllers::eLMB);
            _world->setHole(_cursor_pos, holeTool->brushRadius(),_mod_alt_down, false);
          }
          else if (_mod_ctrl_down && !underMap)
          {
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_HOLES,
                                                           Noggit::ActionModalityControllers::eCTRL
                                                           | Noggit::ActionModalityControllers::eLMB);
            _world->setHole(_cursor_pos, holeTool->brushRadius(), _mod_alt_down, true);
          }
          break;
        case editing_mode::areaid:
          if (!underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_AREAID,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              // draw the selected AreaId on current selected chunk
              _world->setAreaID(_cursor_pos, _selected_area_id, false, ZoneIDBrowser->brushRadius());
            }
            else if (_mod_ctrl_down)
            {
              // pick areaID from chunk
              MapChunk* chnk(std::get<selected_chunk_type>(selection).chunk);
              int newID = chnk->getAreaID();
              _selected_area_id = newID;
              ZoneIDBrowser->setZoneID(newID);
            }
          }
          break;
        case editing_mode::flags:
          if (!underMap)
          {
            // todo: replace this
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_FLAGS,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              _world->mapIndex.setFlag(true, _cursor_pos, 0x2);
            }
            else if (_mod_ctrl_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_FLAGS,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              _world->mapIndex.setFlag(false, _cursor_pos, 0x2);
            }
          }
          break;
        case editing_mode::water:
          if (_display_mode == display_mode::in_3D && !underMap)
          {
            if (_mod_shift_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);
              guiWater->paintLiquid(_world.get(), _cursor_pos, true);
            }
            else if (_mod_ctrl_down)
            {
              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);
              guiWater->paintLiquid(_world.get(), _cursor_pos, false);
            }
          }
          break;
        case editing_mode::stamp:
          if (_display_mode == display_mode::in_3D && (_mod_shift_down || _mod_ctrl_down || _mod_alt_down) && stampTool->getBrushMode())
          {
            auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eNO_FLAG,
                                                           Noggit::ActionModalityControllers::eSHIFT
                                                           | Noggit::ActionModalityControllers::eLMB);

            if (!stampTool->getBrushMode())
              action->setBlockCursor(true);

            stampTool->execute(_cursor_pos, _world.get(), dt, _mod_shift_down, _mod_alt_down, _mod_ctrl_down, underMap);
          }
          break;
        case editing_mode::mccv:
          if (!underMap)
          {
            if (_mod_shift_down)
            {

              auto image_mask_selector = shaderTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                break;

              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR,
                                                             Noggit::ActionModalityControllers::eSHIFT
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeShaderRotation);

              if (image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              shaderTool->changeShader(_world.get(), _cursor_pos, dt, true);
            }
            if (_mod_ctrl_down)
            {

              auto image_mask_selector = shaderTool->getImageMaskSelector();

              if (NOGGIT_CUR_ACTION
                  && image_mask_selector->isEnabled()
                  && !image_mask_selector->getBrushMode())
                break;


              auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR,
                                                             Noggit::ActionModalityControllers::eCTRL
                                                             | Noggit::ActionModalityControllers::eLMB);

              action->setPostCallback(&MapView::randomizeShaderRotation);

              if (image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
                action->setBlockCursor(true);

              shaderTool->changeShader(_world.get(), _cursor_pos, dt, false);
            }
          }
          break;
          default:
            break;
        }
      }
    }
  }

  mh = 0;
  mv = 0;
  rh = 0;
  rv = 0;

  if (_display_mode != display_mode::in_2D)
  {
    if (turn)
    {
      _camera.add_to_yaw(math::degrees(turn));
      _camera_moved_since_last_draw = true;
    }
    if (lookat)
    {
      _camera.add_to_pitch(math::degrees(lookat));
      _camera_moved_since_last_draw = true;
    }

    if (_game_mode_camera.get())
    {
      // [game mode 2026-08-10] client-faithful walking: run on the HORIZONTAL plane at the client's
      // 7 yd/s regardless of camera pitch, real gravity + spacebar jumping with the client's movement
      // constants, and the ground follow includes WMO surfaces (roofs, bridges, interior floors) via
      // the same distance-sorted picking the spawn tool uses. The old block ran along the full look
      // vector and hard-snapped to TERRAIN height only, so every WMO was walked straight through.
      float const     run_speed   = _game_mode_speed; // yd/s -- speed panel (client run 7 by default)
      float constexpr gravity     = 19.2911f;    // yd/s^2 -- client gravity constant
      float constexpr jump_speed  = 7.9557519f;  // yd/s -- client jump takeoff velocity
      float constexpr eye_height  = 2.0f;        // camera above the feet (the old game-mode offset)
      float constexpr step_height = 1.6f;        // tallest ledge stepped up while grounded (stairs)
      float constexpr drop_follow = 1.0f;        // deepest drop followed while grounded (down-stairs);
                                                 // anything deeper is a real edge -> fall

      glm::vec3 feet(_camera.position.x, _camera.position.y - eye_height, _camera.position.z);
      glm::vec3 const feet_before(feet);

      // all game-mode rays go through the probe cache: only nearby collidables get tested
      // (walking the whole instance storage per ray was the "touch the camera and it lags" chop)
      _world->ensureProbeCache(feet);

      // WATER (client state machine, RE'd 3.3.5a FUN_00730d10): depth = waterLevel - feet.
      // Enter swimming at depth > 0.75 * collisionHeight, exit below (enter - 1/36 yd) -- a
      // 2.8cm hysteresis, so the surface settle is a micro-bob, not a threshold flap. A jump's
      // RISE never re-enters swim (client blocks entering until the jump apex, FUN_00986ea0:
      // fallTime < v0/g), which is what lets the water-jump clear the pool.
      // PER-MODEL collision height (user 2026-08-26 "dwarf never sticks head out"): Turtle
      // CreatureModelData f15 x display scale, cached by setGameCharacterDisplayId. The old
      // fixed 2.0894661 was the WOTLK HumanMale value applied to every race. All four 335a
      // constants dumped: enter 0.75x (0x9e9ee4), hysteresis 1/36 (0xa3f854), breach window
      // 1/3 (0xa12098); no hard clamp exists -- the surface float IS this hysteresis bob.
      float const collision_height = _world->gameCharacterCollisionHeight();
      float const swim_enter_depth = 0.75f * collision_height;      // client 0.75 factor
      float const swim_exit_depth = swim_enter_depth - 0.0277778f;  // client 1/36 hysteresis
      float const swim_jump_window = swim_enter_depth + 0.3333333f; // ascend this close to the
                                                                        // surface = jump out (1/3)
      std::optional<float> const water_level = _world->getLiquidHeightAt(feet);
      float const water_depth = water_level ? (*water_level - feet.y) : -999.0f;
      if (water_level)
      {
        _game_last_water_y = water_level; // remembered for the swim-exit splash (see below)
      }
      bool const jump_rising = _game_airborne_from_jump && _game_vertical_speed > 0.0f;
      if (!_game_swimming && water_level && water_depth > swim_enter_depth && !jump_rising)
      {
        // DIVE detection BEFORE the state resets below: the entry splash (visual + sound)
        // belongs to an AIRBORNE arrival (jump/fall into water). The surface float's
        // enter/exit micro-bob (the 1/36 hysteresis) also re-enters swim every bob -- that
        // re-entry must NOT splash (user 2026-08-27: "continuously plays the splash sound").
        bool const dive_entry = !_game_grounded
                             && (_game_airborne_from_jump || _game_vertical_speed < -1.0f);
        _game_swimming = true; // the water kills the fall outright (swim has no vertical memory)
        _game_breaching = false;
        _game_grounded = false;
        _game_airborne_from_jump = false;
        _game_vertical_speed = 0.0f;
        _game_air_time = 0.0f;
        // ENTRY SPLASH here, on the swim-ENTER event itself (user 2026-08-26: "dolphin jumping on
        // top of the water, the ripple won't happen") -- the old was-swimming transition flag only
        // updated inside the swim branch, so it stayed stale-true across a hop and splash-down saw
        // no transition. The enter event fires on EVERY entry: wading in, falling in, each hop.
        {
          World::WaterRippleSpawn splash;
          splash.pos = glm::vec3(feet.x, *water_level, feet.z);
          splash.rot = misc::frand() * 6.2831853f;
          splash.size0 = 0.65f;
          splash.growth = 1.6666666f;
          splash.lifetime_s = 1.0f;
          splash.alpha_peak = 1.0f;
          splash.kind = 1;
          // DIVE entries only + cooldown: the surface bob's re-entries splash neither visually
          // nor audibly (user 2026-08-27 "continuously plays the splash sound").
          if (dive_entry && _world->animtime >= _game_splash_cd_ms)
          {
            _game_splash_cd_ms = static_cast<float>(_world->animtime) + 600.0f;
            _world->pending_ripples.push_back(splash);
            // BODY splash (SoundType 21 CharacterSplashSound, doc 38 round 18): the body
            // entering/leaving the water. Separate lane from the type-20 footstep wading splash
            // below, which is why the water transitions sounded wrong without it.
            int const body_se = _world->characterSplashSoundEntry();
            if (body_se > 0)
            {
              Noggit::Ui::SfxPlayer::instance().play(body_se);
            }
            // ENTER-WATER wading/footstep splash lane (FootstepTerrainLookup SPLASH column).
            int const se = _world->footstepSoundEntry(
              _world->gameCharacterFootstepId(),
              _world->groundTerrainTypeAt(feet, glm::transpose(model_view())), true);
            if (se > 0)
            {
              Noggit::Ui::SfxPlayer::instance().play(se);
            }
          }
        }
      }
      else if (_game_swimming && (!water_level || water_depth < swim_exit_depth))
      {
        // rising through the surface (or wading out / liquid ended): air physics take over.
        // The client fall restarts FROM REST -- no upward velocity carries out of the water --
        // and the horizontal keeps going (_game_air_velocity was set by the swim tick).
        _game_swimming = false;
        _game_grounded = false;
        _game_airborne_from_jump = false;
        _game_vertical_speed = 0.0f;
        _game_air_time = 0.0f;
        _game_breaching = water_level.has_value(); // still over water: hold the swim pose
        _game_air_steer_allowed = !(moving || strafing);
        _game_air_steer_used = false;
        _game_air_input_moving = moving;
        _game_air_input_strafing = strafing;
        // EXIT-WATER SPLASH (user 2026-08-27 "not playing the coming-out-of-water sound", still
        // missing 2026-08-28 "when jumping out of water"). The gate used to be `_game_breaching`,
        // i.e. water_level.has_value() on the very frame swimming ends -- but the two ways out of
        // water differ: surfacing in place still reports liquid (sound played), while JUMPING OUT
        // at a shoreline lands the exit frame PAST the liquid edge, where the query returns
        // nothing, so breaching was false and the splash was skipped entirely. Leaving the water
        // is leaving the water either way: the SOUND now fires on every swim exit (cooldown still
        // suppresses the surface bob). The visual ripple still needs a real surface, so it uses
        // the live level when there is one and otherwise the last one seen while swimming, and is
        // skipped if neither exists.
        if (_world->animtime >= _game_splash_cd_ms)
        {
          _game_splash_cd_ms = static_cast<float>(_world->animtime) + 600.0f;
          auto const splash_y = water_level ? water_level : _game_last_water_y;
          if (splash_y)
          {
            World::WaterRippleSpawn splash;
            splash.pos = glm::vec3(feet.x, *splash_y, feet.z);
            splash.rot = misc::frand() * 6.2831853f;
            splash.size0 = 0.65f;
            splash.growth = 1.6666666f;
            splash.lifetime_s = 1.0f;
            splash.alpha_peak = 1.0f;
            splash.kind = 1;
            _world->pending_ripples.push_back(splash);
          }
          // BODY splash (SoundType 21 CharacterSplashSound) -- the body leaving the water; the
          // footstep splash below is the separate type-20 wading lane (doc 38 round 21).
          int const body_se = _world->characterSplashSoundEntry();
          if (body_se > 0)
          {
            Noggit::Ui::SfxPlayer::instance().play(body_se);
          }
          int const se = _world->footstepSoundEntry(
            _world->gameCharacterFootstepId(),
            _world->groundTerrainTypeAt(feet, glm::transpose(model_view())), true);
          if (se > 0)
          {
            Noggit::Ui::SfxPlayer::instance().play(se);
          }
        }
      }

      // wanted horizontal velocity: WASD while grounded. Mid-air the client is BALLISTIC -- the
      // takeoff velocity is locked when the feet leave the ground and keyboard cannot steer the
      // jump/fall -- so airborne we replay _game_air_velocity and ignore input entirely.
      glm::vec3 horiz(0.0f);
      if (_game_grounded && (moving || strafing))
      {
        glm::vec3 const look(_camera.direction());
        glm::vec3 forward(look.x, 0.0f, look.z);
        float const flen = glm::length(forward);
        // looking straight up/down leaves no horizontal component; fall back to yaw via the camera's
        // right vector so movement never dead-zones
        glm::vec3 const right(glm::normalize(glm::cross(flen > 0.0001f ? forward / flen
                                                                       : glm::vec3(0.f, 0.f, 1.f)
                                                       , glm::vec3(0.f, 1.f, 0.f))));
        forward = flen > 0.0001f ? forward / flen : glm::cross(glm::vec3(0.f, 1.f, 0.f), right);
        glm::vec3 velocity(forward * moving + right * strafing);
        float const vlen = glm::length(velocity);
        if (vlen > 0.0001f)
        {
          // client backpedal: walking backwards is capped at 4.5 yd/s no matter the run speed
          float const speed = (moving < 0.0f) ? std::min(run_speed, 4.5f) : run_speed;
          horiz = velocity * (speed / vlen); // diagonal (forward+strafe) still moves at full speed, like the client
        }
      }
      if (_game_grounded)
      {
        _game_air_velocity = horiz; // momentum carried into the next jump / edge-drop
      }
      else
      {
        // ONE-TIME AIR STEER (client, RE'd FUN_00988a20/FUN_00988b00): the FIRST movement input
        // during a fall that took off with NO movement keys redirects the horizontal velocity
        // once, at WALK speed (client speed getter with the air-move flag = min(walk, run));
        // afterwards -- and for any takeoff that had keys held -- input pends until landing.
        if (_game_air_steer_allowed && !_game_air_steer_used && !_game_swimming
            && (moving || strafing))
        {
          glm::vec3 const look(_camera.direction());
          glm::vec3 flat(look.x, 0.0f, look.z);
          float const flen = glm::length(flat);
          glm::vec3 const ff(flen > 0.0001f ? flat / flen : glm::vec3(0.f, 0.f, 1.f));
          glm::vec3 const right(glm::normalize(glm::cross(ff, glm::vec3(0.f, 1.f, 0.f))));
          glm::vec3 const dir(ff * moving + right * strafing);
          float const dlen = glm::length(dir);
          if (dlen > 0.0001f)
          {
            _game_air_velocity = dir * (std::min(2.5f, run_speed) / dlen);
            _game_air_steer_used = true;
            _game_air_input_moving = moving;   // the frozen moveflags now carry the steer input:
            _game_air_input_strafing = strafing; // the strafe-twist posture follows it (client)
          }
        }
        horiz = _game_air_velocity; // ballistic: follow the takeoff trajectory
      }

      // COLLISION. NOTE (honest, 2026-08-13): the exact 3.3.5a local-player wall-clip function was
      // NOT isolated in the binary despite exhaustive RE (every CWorld::Intersect caller + every
      // geometry query decompiled -- all are camera/LOS/missile or VERTICAL ground-follow; the
      // client's ground query FUN_007a0b00 chain is floor-height only). So this is a BEST-EFFORT
      // model, not a proven 1:1 port. The prior all-floor-query attempt OVER-BLOCKED: probing the
      // topmost surface 2yd above the feet treated every tree canopy / bridge / eave / doorway
      // header as a wall. This version separates the two real cases:
      //   (1) FORWARD wall detection at TORSO heights (walls, fences, trunks, crate sides) -- rays
      //       in the move direction, reach = |delta| + radius. NO shin ray (that stuck players on
      //       stair risers). Lateral whiskers give the body its width so thin posts between rays
      //       still block.
      //   (2) LOW-obstacle step rule via a ground probe from a LOW origin (<= waist) so it catches
      //       curbs/crates near foot level but CANNOT see overhangs above: floor rises <= step ->
      //       glide up (stairs/curb); > step and below torso -> block (low crate/rim).
      // STEP ALLOWANCE. The client's ground query has NO step limit (FUN_007c8360 = a per-group
      // bilinear FLOOR HEIGHTFIELD whose accept test is `entityZ < floorZ + 0.01`, unbounded
      // above; the terrain pass FUN_007ad3b0 has no test at all; the relink FUN_007a1bc0 only
      // decides GROUNDED from `groundZ <= entity+0x5c`). But the client's floor comes from a
      // HEIGHTFIELD over walkable surfaces, while ours is a DOWN-RAY that happily returns a wall
      // cap, a crate lid or a table top -- so an unbounded allowance let the player walk up things
      // that are not walkable in game ("too generous, I can overclimb shit I can't in game",
      // 2026-08-28). Back to a curb-height allowance; the STAIRS fix is the sweep base below, not
      // this number. DEVIATION, labelled.
      // STEP ALLOWANCE round 3 (user 2026-08-28: "struggling to walk up bzb_greysky_city's interior
      // staircase -- threshold too strict"). The CLIENT has NO step limit at all (FUN_007c8360
      // accepts any floor with entityZ < floorZ + 0.01; FUN_007ad3b0 has no test; doc 39 sec 1) --
      // 0.55 was our number, not the client's, and real WMO risers exceed it. The cap exists ONLY
      // because our floor probe is a down-ray that can return unwalkable tops; the probe fires from
      // low_probe_top (1.0yd, waist) so nothing higher than 1.0 above the feet can be seen anyway.
      // Setting the allowance to that same geometric bound = the least-strict value our probe
      // permits, i.e. as close to the client's no-limit as this model can express. (The round-2
      // "overclimb" that briefly reverted this was the LANDING probe bug, fixed separately.)
      float constexpr step_limit   = 1.0f;  // = low_probe_top; client-side there is no limit
      float constexpr body_radius  = 0.5f;  // ~HumanMale collision radius (CreatureModelData order)
      float constexpr low_probe_top = 1.0f; // ground-climb probe origin: waist-high, never sees ceilings
      float const climb_limit = std::max(step_limit, 1.2f * run_speed * dt);
      // base_y = the height the torso ray ladder is measured from. Normally the feet; when the
      // move is a STEP-UP it is the stepped-to floor, so an ascending staircase is not swept as
      // if it were a wall in front of the player's chest (see try_move).
      auto const wall_ahead = [&](glm::vec3 const& delta, float base_y) -> bool
      {
        float const len = glm::length(delta);
        if (len < 1e-4f) { return false; }
        glm::vec3 const dir(delta / len);
        glm::vec3 side(0.0f);
        {
          glm::vec3 const h(-dir.z, 0.0f, dir.x);
          float const hl = glm::length(h);
          if (hl > 1e-3f) { side = h / hl * (body_radius * 0.8f); }
        }
        // DENSE height ladder, all ABOVE the step limit (0.55) so stair risers/curbs are NOT read
        // as walls (they go through the ground-climb rule) but a THIN fence rail can't slip through:
        // the old 0.9/1.4/1.9 left a 0.5yd gap and a rail at ~1.2 passed clean between two rays
        // (the user's "walk into the side of a fence at certain mesh points"). Max gap now 0.3yd.
        for (float ph : { 0.7f, 1.0f, 1.3f, 1.6f, 1.9f, 2.2f })
        {
          for (float lat : { 0.0f, -1.0f, 1.0f })
          {
            glm::vec3 const from(feet.x + side.x * lat, base_y + ph, feet.z + side.z * lat);
            math::ray const ray(from, dir);
            selection_result const hits
              (_world->intersectProbe(glm::transpose(model_view()), ray, len + body_radius + 0.1f));
            for (auto const& hit : hits)
            {
              if (hit.first <= len + body_radius) { return true; }
            }
          }
        }
        return false;
      };
      auto const try_move = [&](glm::vec3 const& delta) -> bool
      {
        // AIRBORNE. Fully free movement (what the ground-follow RE literally implies) let the
        // player walk INTO solids mid-jump -- stormwindfountain01, stair flights -- and then fall
        // through the world (user 2026-08-28). But the old torso-ray veto killed real jumps: the
        // ladder reaches ~0.7yd, so brushing a ledge FACE while still RISING zeroed the horizontal
        // velocity and dropped the player straight down. Neither is right, and the client's actual
        // wall clip is still not located (docs/client_re/39 sec 4), so the rule here is the
        // PHYSICAL one: an obstacle blocks only if this jump cannot get on top of it.
        //   top <= feet          -> already above it, keep going (you land on it)
        //   top - feet <= rise   -> the remaining rise still clears it, keep going
        //   otherwise            -> that is a wall to you right now, block
        // rise = v^2/2g, the height left in the arc (zero once falling), so a descending player is
        // stopped by anything above the feet and a rising one may still mount a reachable ledge.
        if (!_game_grounded)
        {
          float const dlen_air = glm::length(delta);
          glm::vec3 const ahead_air =
            dlen_air > 1e-5f ? delta * (body_radius / dlen_air) : glm::vec3(0.0f);
          glm::vec3 const target_air(feet.x + delta.x + ahead_air.x, feet.y,
                                     feet.z + delta.z + ahead_air.z);
          // probe origin above the head so a whole wall face registers, not just a low lip
          auto const top = game_mode_ground_height(target_air, 2.5f, 2.5f + 8.0f);
          if (!top || *top <= feet.y + 0.1f)
          {
            return true;
          }
          float const rise_left = (_game_vertical_speed > 0.0f)
            ? (_game_vertical_speed * _game_vertical_speed) / (2.0f * gravity)
            : 0.0f;
          return (*top - feet.y) <= rise_left + 0.1f;
        }

        // GROUNDED: probe the floor a body-radius ahead from a WAIST-high origin (never sees
        // overhangs).
        float const dlen = glm::length(delta);
        glm::vec3 const ahead = dlen > 1e-5f ? delta * (body_radius / dlen) : glm::vec3(0.0f);
        glm::vec3 const target(feet.x + delta.x + ahead.x, feet.y, feet.z + delta.z + ahead.z);
        auto const g = game_mode_ground_height(target, low_probe_top, low_probe_top + step_height + 8.0f);

        // A walkable rise is a STEP-UP, not a wall: sweep the torso rays from the STEPPED-TO
        // height. Without this the 0.7-2.2yd ladder hits the NEXT RISERS of an ascending
        // staircase (they sit right in front of the chest) and the player sticks on the stairs
        // -- the user's long-standing "I get stuck walking up stairs" (2026-08-28).
        float ray_base = feet.y;
        if (g && *g > feet.y && *g - feet.y <= climb_limit)
        {
          ray_base = *g + 0.02f;
        }
        if (wall_ahead(delta, ray_base))
        {
          return false; // torso/head geometry: walls, fences, trunks, crate sides
        }
        // LOW-obstacle rule: rises > climb (low crate/rim/fountain edge the torso rays pass over)
        // -> block; <= climb (curb/stair/slope) -> allow, the ground-follow lifts the feet.
        if (g && *g - feet.y > climb_limit)
        {
          return false;
        }
        return true;
      };

      bool moved_h = false;
      if (!_game_swimming)
      {
      if (glm::length(horiz) > 0.0001f)
      {
        glm::vec3 const step(horiz.x * dt, 0.0f, horiz.z * dt);
        if (try_move(step))
        {
          feet.x += step.x;
          feet.z += step.z;
        }
        else
        {
          glm::vec3 const step_x(step.x, 0.0f, 0.0f);
          if (step.x != 0.0f && try_move(step_x))
          {
            feet.x += step.x;
          }
          else
          {
            _game_air_velocity.x = 0.0f;
          }
          glm::vec3 const step_z(0.0f, 0.0f, step.z);
          if (step.z != 0.0f && try_move(step_z))
          {
            feet.z += step.z;
          }
          else
          {
            _game_air_velocity.z = 0.0f;
          }
        }
      }

      moved_h = (feet.x != feet_before.x) || (feet.z != feet_before.z);

      // CLIENT JUMP TRIGGER (RE'd FUN_0072eb80 -> AUCPlayerMoveEvent type 10): EDGE-triggered,
      // one jump per physical space press -- holding space does NOT hop again on landing, and a
      // press while airborne is eaten (the queued event can't execute while falling; there is NO
      // buffering and NO cooldown in the client -- the re-press cadence itself is what gives the
      // landing anim time to play).
      if (_game_grounded && _game_jump_pressed)
      {
        _game_vertical_speed = jump_speed;
        _game_grounded = false;
        _game_fall_peak_y = feet.y;
        _game_airborne_from_jump = true;
        _game_anim_restart_pending = true; // re-launching JumpStart must restart it (held-space hops)
        _game_air_time = 0.0f;
        _game_air_steer_allowed = !(moving || strafing); // stationary jump: one air steer left
        _game_air_steer_used = false;
        _game_air_input_moving = moving;   // client: moveflags freeze for the whole fall
        _game_air_input_strafing = strafing;
      }

      // ground under the (possibly moved) feet -- probe from step_height above so stairs count as
      // ground. SKIPPED when standing still on the ground: static geometry can't move, and the idle
      // probe cost is real framerate. Reach scales with fall speed so a fast fall (or a hitchy
      // frame's big dt) still sees the floor coming instead of tunnelling through it.
      std::optional<float> ground;
      if (!_game_grounded || moved_h)
      {
        float const probe_reach =
          step_height + std::max(8.0f, -_game_vertical_speed * dt * 2.0f + 4.0f);
        ground = game_mode_ground_height(feet, step_height, probe_reach);
      }

      if (_game_grounded)
      {
        // climb_limit shared with try_move's veto above: anything higher than the allowance
        // (crate/fence tops) already rejected the move there; this branch just follows the ground.
        if (ground && *ground >= feet.y - drop_follow && *ground - feet.y <= climb_limit)
        {
          feet.y = *ground; // walkable: climb the step / follow the slope down
        }
        else if (ground && *ground > feet.y + climb_limit)
        {
          // ledge top above the climb allowance (box/fence): stay grounded at current height
        }
        else if (ground)
        {
          _game_grounded = false; // walked off an edge -> start falling
          _game_vertical_speed = 0.0f;
          _game_fall_peak_y = feet.y;
          _game_airborne_from_jump = false; // Fall anim, not Jump
          _game_air_time = 0.0f;
          _game_air_steer_allowed = !(moving || strafing);
          _game_air_steer_used = false;
          _game_air_input_moving = moving;
          _game_air_input_strafing = strafing;
        }
        // no ground at all (void / unloaded): hold height rather than falling forever
      }

      if (!_game_grounded)
      {
        _game_air_time += dt;
        float const air_prev_y = feet.y;
        _game_vertical_speed -= gravity * dt;
        feet.y += _game_vertical_speed * dt;
        _game_fall_peak_y = std::max(_game_fall_peak_y, feet.y);
        // LANDING PROBE. `ground` above starts step_height (1.6yd) ABOVE the feet and returns the
        // TOPMOST surface from there down, which while airborne is often something OVER the head
        // (a stair riser ahead, a fountain rim) -- testing the landing against that either
        // teleported the player up onto it (the "overclimb" regression) or, once guarded by a
        // reach, NEVER matched, so the player fell straight through the WMO and out of the world
        // (user 2026-08-28 "i can clip into stairs and fall thru wmo to the ground"). The landing
        // question is only ever about the floor the feet CROSSED this frame, so probe from where
        // the feet were BEFORE this step: every hit it can return is at or below that point.
        std::optional<float> land_ground;
        if (_game_vertical_speed <= 0.0f)
        {
          float const drop = std::max(0.0f, air_prev_y - feet.y);
          land_ground = game_mode_ground_height(feet, drop + 0.05f,
                                                drop + 0.05f + std::max(8.0f, drop * 2.0f));
        }
        if (!ground && feet.y < -2000.0f)
        {
          // in-place Game View entry over void/unloaded tiles: nothing below to land on, so
          // stop the runaway fall below any real map height and stand on an invisible floor
          feet.y = -2000.0f;
          _game_vertical_speed = 0.0f;
          _game_grounded = true;
          _game_airborne_from_jump = false;
        }
        if (_game_breaching && _game_air_time > 0.5f)
        {
          _game_breaching = false; // way past a surface dip: this is a real fall (Fall anim)
        }
        // TOUCHDOWN = the feet crossed `land_ground` (the floor under the pre-step position).
        if (_game_vertical_speed <= 0.0f && land_ground && feet.y <= *land_ground)
        {
          feet.y = *land_ground; // touchdown
          _game_vertical_speed = 0.0f;
          _game_grounded = true;
          bool const was_jump_or_real_fall = _game_airborne_from_jump || _game_air_time >= 0.25f;
          float const fall_drop = std::max(0.0f, _game_fall_peak_y - feet.y);
          _game_airborne_from_jump = false;

          // GROUND CONTACT + FALL DAMAGE VOCAL (user 2026-08-28: "we are missing for game view
          // mode falling and landing and hitting ground ... should do the taking damage yell too
          // like in game when falling and taking damage").
          //   * The thud is the FootstepTerrainLookup lane again -- in the client the landing
          //     contact sound is a footfall of the terrain under you (the $FSD chain, doc 38), so
          //     it is stone in Stormwind, snow in Dun Morogh, and the SPLASH column when the feet
          //     land in liquid. Micro-air (stepping over props, collision jitter) stays silent,
          //     same gate the landing ANIM uses.
          //   * The client has no continuous "falling" sound: the only fall-specific client logic
          //     is the network fall event -- FUN_0073a890 flips move opcode 0x2d/0x2e once the
          //     feet are more than DAT_00a41b18 = 1.5yd above the ground -- which carries no audio.
          //   * Fall damage is SERVER business, so its threshold is cited from this project's own
          //     core: tortoise-wow src/game/Objects/Player.cpp HandleFall, `z_diff >= 14.57f`
          //     (the height at which 0.018*z - 0.2426 crosses zero). Past it the controlled
          //     display's CreatureSoundData WOUND line plays -- the yell you hear in game.
          if (was_jump_or_real_fall && !_game_swimming)
          {
            int const step_se = _world->footstepSoundEntry(
              _world->gameCharacterFootstepId(),
              _world->groundTerrainTypeAt(feet, glm::transpose(model_view())),
              water_level.has_value() && water_depth > 0.0f); // feet in liquid -> splash column
            if (step_se > 0)
            {
              Noggit::Ui::SfxPlayer::instance().play(step_se);
            }
            if (fall_drop >= 14.57f)
            {
              int const hurt_se =
                static_cast<int>(_world->gameCharacterSoundEntry(CreatureSoundDataDB::Wound));
              if (hurt_se > 0)
              {
                Noggit::Ui::SfxPlayer::instance().play(hurt_se);
              }
            }
          }
          _game_fall_peak_y = feet.y;
          if (_game_breaching)
          {
            // wading out through the surface dip: the client never treats this as a fall
            // (the exit-depth walk-out goes straight to the ground anims, no JumpEnd flash)
            _game_breaching = false;
          }
          else
          {
            // CLIENT LANDING (RE'd FUN_0073d2b0 + the one-shot chain FUN_0073ac30/FUN_0071e0d0/
            // FUN_0073bff0): the landing anim is chosen ONCE from the input AT TOUCHDOWN --
            // standing -> JumpEnd (39); moving forward/strafing at run speed -> JumpLandRun
            // (187); backpedal or walk speed -> NO landing anim. It then plays as a one-shot to
            // completion: the per-frame selector keeps returning the one-shot's own id, so input
            // changes during it neither re-pick nor restart it. (Re-evaluating from live input
            // every tick made tap-tap-tap W flip 39<->187, restarting the blend on each tap --
            // the "movement pauses the fall animation" flap.)
            bool const any_input = (moving != 0.0f || strafing != 0.0f);
            int land_anim = 0;
            // micro-air (stepping up/over props, collision jitter) never queues a landing
            // one-shot -- only jumps and real falls do (matches the Fall-anim grace above)
            if (!was_jump_or_real_fall)
            {
              // no landing anim
            }
            else if (!any_input)
            {
              land_anim = 39;
            }
            else if (moving >= 0.0f && run_speed > 5.0f)
            {
              // CLIENT-EXACT threshold (wow335a.exe FUN_00716710->FUN_00716fa0 @0x716fa0):
              // JumpLandRun (187) is suppressed when curSpeed <= 2*walkSpeed. walkSpeed = 2.5
              // (CMovement+0x90) -> 2x = 5.0. The old 3.5 let a jog land in the run-land anim
              // where the client plays JumpEnd. Backward (moving<0) always -> no 187 (client:
              // FUN_0073d2b0 clears the one-shot for the backward flag).
              land_anim = (_world->gameCharacterAnimLengthMs(187) > 0) ? 187 : 39;
            }
            _game_land_anim = land_anim;
            if (land_anim != 0)
            {
              int const land_ms = _world->gameCharacterAnimLengthMs(land_anim);
              // 187 plays at the run-speed playback scale (client moveSpeed scaling) -- shrink
              // the wall-clock window with it or the held last frame gaps the handoff to Run
              float scale = 1.0f;
              if (land_anim == 187)
              {
                float const authored = _world->gameCharacterAnimMoveSpeed(187);
                if (authored > 0.1f)
                {
                  scale = std::clamp(run_speed / authored, 0.25f, 4.0f);
                }
              }
              _game_land_anim_timer =
                land_ms > 0 ? static_cast<float>(land_ms) / 1000.0f / scale : 0.35f;
            }
            else
            {
              _game_land_anim_timer = 0.0f;
            }
          }
        }
      }
      }
      else
      {
        // SWIMMING (client displacement law, RE'd FUN_00987b50 / FUN_00987700 / FUN_009876b0
        // + direction builder FUN_00987e30):
        //  - forward/back swim along the PITCHED 3D forward (z = sin(look pitch)); STRAFE is
        //    FLAT; a diagonal is (pitched-forward +- flat-right)/sqrt2 -- full speed with the
        //    dive rate halved. Backpedal speed 2.5 wins on any back combo (flag semantics).
        //  - SPACE (ascend) alone: straight up at full swim speed. With movement keys the
        //    motion becomes flat-input * speed/sqrt2 plus speed/sqrt2 UP -- exactly 45 deg,
        //    look pitch IGNORED (that is why the client pitches the body to 45 while rising).
        //  - SPACE within 1/3 yd above the enter depth = the client water JUMP (FUN_0072eb80):
        //    a standard jump out of the pool. THAT is the dolphin leap / hop onto the shore --
        //    there is no velocity-threshold breach in the client.
        // No surface position cap: the exit-depth check in the state machine handles the
        // waterline, so swimming up just crests into the micro-dip and settles.
        float constexpr swim_speed = 4.722222f;
        float constexpr swim_back = 2.5f;
        float constexpr inv_sqrt2 = 0.70710678f;
        glm::vec3 const look(_camera.direction());
        glm::vec3 flat(look.x, 0.0f, look.z);
        float const flen = glm::length(flat);
        glm::vec3 const fflat(flen > 0.0001f ? flat / flen : glm::vec3(0.f, 0.f, 1.f));
        glm::vec3 const right(glm::normalize(glm::cross(fflat, glm::vec3(0.f, 1.f, 0.f))));
        float const mv = (moving > 0.0f) ? 1.0f : (moving < 0.0f) ? -1.0f : 0.0f;
        float const st = (strafing > 0.0f) ? 1.0f : (strafing < 0.0f) ? -1.0f : 0.0f;
        float const speed = (mv < 0.0f) ? swim_back : swim_speed;
        if (_mod_space_down && water_depth < swim_jump_window)
        {
          // WATER JUMP: ascend close under the surface converts into a real jump (JumpStart,
          // client jump velocity); landing back in deep water re-enters swim past the apex.
          _game_swimming = false;
          _game_breaching = false;
          _game_grounded = false;
          _game_airborne_from_jump = true;
          _game_anim_restart_pending = true; // water dolphin-hop: restart JumpStart every launch
          _game_air_time = 0.0f;
          _game_vertical_speed = jump_speed;
          _game_air_steer_allowed = !(moving || strafing); // space-only water jump: steerable once
          _game_air_steer_used = false;
          _game_air_input_moving = moving;
          _game_air_input_strafing = strafing;
          glm::vec3 const flat_dir(fflat * mv + right * st);
          float const dlen = glm::length(flat_dir);
          _game_air_velocity = dlen > 0.0001f ? flat_dir * (speed * inv_sqrt2 / dlen)
                                              : glm::vec3(0.0f);

          // EXIT-WATER SPLASH for the WATER JUMP (user 2026-08-28 "when jumping out of water its
          // not playing the coming out of water sound"). THIS is the path a space-press out of
          // water actually takes -- it clears _game_swimming/_game_breaching right here and never
          // reached the exit-branch splash below, so the dolphin hop left the water in silence.
          // Same lane as the dive/exit splash (FootstepTerrainLookup splash column, doc 38) and
          // the same 600 ms cooldown so a bob at the surface cannot machine-gun it.
          if (_world->animtime >= _game_splash_cd_ms)
          {
            _game_splash_cd_ms = static_cast<float>(_world->animtime) + 600.0f;
            auto const splash_y = water_level ? water_level : _game_last_water_y;
            if (splash_y)
            {
              World::WaterRippleSpawn splash;
              splash.pos = glm::vec3(feet.x, *splash_y, feet.z);
              splash.rot = misc::frand() * 6.2831853f;
              splash.size0 = 0.65f;
              splash.growth = 1.6666666f;
              splash.lifetime_s = 1.0f;
              splash.alpha_peak = 1.0f;
              splash.kind = 1;
              _world->pending_ripples.push_back(splash);
            }
            // BODY splash (SoundType 21 CharacterSplashSound, doc 38 round 18): the body
            // entering/leaving the water. Separate lane from the type-20 footstep wading splash
            // below, which is why the water transitions sounded wrong without it.
            int const body_se = _world->characterSplashSoundEntry();
            if (body_se > 0)
            {
              Noggit::Ui::SfxPlayer::instance().play(body_se);
            }
            int const se = _world->footstepSoundEntry(
              _world->gameCharacterFootstepId(),
              _world->groundTerrainTypeAt(feet, glm::transpose(model_view())), true);
            if (se > 0)
            {
              Noggit::Ui::SfxPlayer::instance().play(se);
            }
          }
        }
        else
        {
          glm::vec3 vel(0.0f);
          if (_mod_space_down)
          {
            glm::vec3 const flat_dir(fflat * mv + right * st);
            float const dlen = glm::length(flat_dir);
            if (dlen > 0.0001f)
            {
              vel = flat_dir * (speed * inv_sqrt2 / dlen); // 45 deg law: flat at speed/sqrt2...
              vel.y = speed * inv_sqrt2;                   // ...plus speed/sqrt2 up
            }
            else
            {
              vel.y = speed; // ascend alone: straight up at full speed
            }
          }
          else if (mv != 0.0f || st != 0.0f)
          {
            // CLIENT PITCH SOURCE (decompiled 2026-08-26, doc 37 -- the drift/staircase fix): the
            // unit's swim pitch is only ever SET during mouselook steering (RMB); keyboard-only
            // swimming is LEVEL. The direction builder FUN_00987e30's "threshold" _DAT_009f1224 =
            // 9.5e-7 -- an epsilon, not a dead zone. Our old always-follow fed the default
            // 3rd-person down-tilt into the vector = the slow submerge.
            // this->look = the RMB mouselook flag (the local `look` above is the view vector)
            float const pitch_up = this->look ? -glm::radians(_camera.pitch()._) : 0.0f; // camera pitch is +down
            glm::vec3 const fwd3(std::cos(pitch_up) * fflat.x, std::sin(pitch_up),
                                 std::cos(pitch_up) * fflat.z);
            glm::vec3 dir3(fwd3 * mv + right * st);
            dir3 /= std::sqrt(std::abs(mv) + std::abs(st)); // client renormalize: /1 or /sqrt2
            vel = dir3 * speed;
          }
          glm::vec3 const step(vel.x * dt, 0.0f, vel.z * dt);
          if (glm::length(step) > 0.0001f && try_move(step))
          {
            feet.x += step.x;
            feet.z += step.z;
          }
          feet.y += vel.y * dt;
          // SURFACE CEILING (2026-08-26): swimming up cannot carry the character out of the
          // water -- ascent clamps just under the exit depth, so a steep-up steer glides along
          // the surface instead of the exit->fall->re-enter staircase. Breaching stays the
          // water JUMP's job (space near the surface), like the client.
          if (water_level)
          {
            float const surface_cap = *water_level - (swim_exit_depth + 0.02f);
            if (vel.y > 0.0f && feet.y > surface_cap)
            {
              feet.y = surface_cap;
            }
          }
          std::optional<float> const bottom =
            game_mode_ground_height(feet, step_height, step_height + 8.0f);
          if (bottom && feet.y < *bottom)
          {
            feet.y = *bottom; // lake floor
          }
          _game_vertical_speed = 0.0f;
          // the flat components carry into the surface-exit fall (the crest keeps gliding)
          _game_air_velocity = glm::vec3(vel.x, 0.0f, vel.z);
        }
        moved_h = (feet.x != feet_before.x) || (feet.z != feet_before.z) || (feet.y != feet_before.y);
      }

      // one jump per press: the edge is consumed every game tick whether it fired (grounded),
      // was eaten mid-air (client: no buffering), or was ignored while swimming
      _game_jump_pressed = false;

      // Did the camera turn since the last game tick? Idle gating for the probe blocks below --
      // standing still without turning runs ZERO raycasts (the idle game-mode framerate cost).
      float const cam_yaw = _camera.yaw()._;
      // turn detection uses the VIEW angles (facing + LMB orbit) so orbiting re-runs the camera
      // probes even though the character facing didn't change
      float const view_yaw = cam_yaw + _game_orbit_yaw;
      float const view_pitch = _camera.pitch()._ + _game_orbit_pitch;
      bool const cam_turned = (view_yaw != _game_prev_cam_yaw) || (view_pitch != _game_prev_cam_pitch);
      _game_prev_cam_yaw = view_yaw;
      _game_prev_cam_pitch = view_pitch;

      // CAMERA DEPENETRATION: the movement rays only test the travel direction, so an oblique
      // wall-slide or a corner can still leave the eye close enough for the near plane to clip in
      // -- and turning the head next to a fence moves the frustum without moving the feet at all.
      // Client-style fix: treat the eye as a small sphere and push the body away from any surface
      // closer than cam_radius, testing the look direction plus 8 compass directions. Runs EVERY
      // tick (probe-cache cheap): gating it on moved/turned made turn frames cost more than idle
      // frames -- the "steppy" camera.
      {
        float constexpr cam_radius = 0.5f;
        glm::vec3 push(0.0f);
        glm::vec3 dirs[9];
        int ndirs = 0;
        dirs[ndirs++] = game_camera_look_direction(); // the frustum points this way (incl. LMB orbit)
        // full compass sphere only when the BODY moved; a turn-in-place frame only needs the look
        // ray (the frustum is what could clip), keeping mouselook cheap and smooth
        if (moved_h || !_game_grounded)
        {
          for (int i = 0; i < 8; ++i)
          {
            float const a = float(i) * 0.7853981634f; // pi/4
            dirs[ndirs++] = glm::vec3(std::cos(a), 0.0f, std::sin(a));
          }
        }
        for (int i = 0; i < ndirs; ++i)
        {
          glm::vec3 const eye(feet.x + push.x, feet.y + eye_height, feet.z + push.z);
          math::ray const ray(eye, dirs[i]);
          selection_result const hits
            (_world->intersectProbe(glm::transpose(model_view()), ray, cam_radius + 0.1f));
          float nearest = cam_radius;
          for (auto const& hit : hits)
          {
            nearest = std::min(nearest, hit.first);
          }
          if (nearest < cam_radius)
          {
            push.x -= dirs[i].x * (cam_radius - nearest);
            push.z -= dirs[i].z * (cam_radius - nearest);
          }
        }
        // horizontal only -- never shove the body through the floor or into the air
        feet.x += push.x;
        feet.z += push.z;
      }

      // 3RD-PERSON CAMERA BOOM: pull the wheel distance in to the first surface behind the head
      // (client-style -- the orbit camera never enters geometry). Recomputed when the player moved,
      // the camera turned, or the wheel changed the wanted distance; ray reach = the boom itself.
      if (_game_third_person_distance > 0.01f)
      {
        // recompute the sweep EVERY tick (probe-cache cheap) -- uniform frame cost, no stepping
        {
          // CLIENT-STYLE camera sweep: not one thin ray but the near-plane VOLUME -- rays from the
          // head to the desired camera position AND its four near-plane corners. A single center
          // ray slid past terrain and wall edges just off-axis, letting the frustum corners clip
          // inside; the shortest corner fraction pulls the whole boom in instead.
          glm::vec3 const head(feet.x, feet.y + eye_height, feet.z);
          glm::vec3 const look(game_camera_look_direction());
          glm::vec3 const cam_des(head - look * _game_third_person_distance);
          float const near_d = 0.25f; // MUST match the game-mode near plane in projection()
          float const half_h = near_d * std::tan(_camera.fov()._ * 0.5f);
          float const half_w = half_h * aspect_ratio();
          glm::vec3 const right(glm::normalize(glm::cross(look, glm::vec3(0.f, 1.f, 0.f))));
          glm::vec3 const up(glm::cross(right, look));
          glm::vec3 const corners[5] = {
            glm::vec3(0.0f),
            right * half_w + up * half_h,
            right * half_w - up * half_h,
            -right * half_w + up * half_h,
            -right * half_w - up * half_h,
          };
          float limit = _game_third_person_distance;
          for (auto const& corner : corners)
          {
            glm::vec3 const to((cam_des + corner) - head);
            float const len = glm::length(to);
            if (len < 0.001f)
            {
              continue;
            }
            math::ray const sweep(head, to / len);
            selection_result const hits
              (_world->intersectProbe(glm::transpose(model_view()), sweep, len + 0.5f));
            for (auto const& hit : hits)
            {
              if (hit.first < len)
              {
                limit = std::min(limit, std::max(0.0f, (hit.first / len) * _game_third_person_distance - 0.1f));
              }
            }
          }
          // (round 26) The boom-DISTANCE water wall is retired -- it clamped how far the boom
          // travels but never lifted the eye off the surface plane, and its sticky-side logic
          // fought the eye push. The render-eye Y push (snapCameraOffWaterSurface) replaces it.
          _game_boom_limit = limit;
        }
        // VALIDATED snap-in / glide-out (the "vibrating at the edge" fix). The old flow glided
        // toward the CORNER-RAY limit and then let the clearance sphere pull the camera back --
        // two different collision oracles disagreeing every frame = a one-step-forward,
        // one-step-back oscillation at the boundary. Now ONE validator owns clearance:
        //   validate_boom(d): from distance d, iteratively pull in until a 26-direction
        //   proximity sphere (6 axes + 12 edge diagonals + 8 corner diagonals -- the edge set
        //   is what catches walls lying obliquely between axis and corner samples) AND a floor
        //   probe (camera >= cam_clear above whatever is straight below -- hillsides) are both
        //   satisfied; returns the largest clear distance <= d.
        // The glide only ADVANCES to a candidate that validates farther out than where it is;
        // when the next step would clip, the camera holds still at the last safe distance.
        {
          // covers the REAL near-plane half-diagonal (~0.24 at the game-mode 0.25 near plane)
          // with margin; the old 0.5 was sized for a near plane the projection didn't have
          float constexpr cam_clear = 0.35f;
          glm::vec3 const vlook(game_camera_look_direction());
          glm::vec3 const head_pos(feet.x, feet.y + eye_height, feet.z);
          float constexpr c = 0.57735027f; // 1/sqrt3
          float constexpr e = 0.70710678f; // 1/sqrt2
          static glm::vec3 const clear_dirs[26] = {
            {1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
            {0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f},
            { e,  e, 0.f}, {-e,  e, 0.f}, { e, -e, 0.f}, {-e, -e, 0.f},
            { e, 0.f,  e}, {-e, 0.f,  e}, { e, 0.f, -e}, {-e, 0.f, -e},
            {0.f,  e,  e}, {0.f, -e,  e}, {0.f,  e, -e}, {0.f, -e, -e},
            { c,  c,  c}, {-c,  c,  c}, { c,  c, -c}, {-c,  c, -c},
            { c, -c,  c}, {-c, -c,  c}, { c, -c, -c}, {-c, -c, -c},
          };
          auto const validate_boom = [&](float dist) -> float
          {
            for (int pass = 0; pass < 8 && dist > 0.05f; ++pass)
            {
              glm::vec3 const cam_pos(head_pos - vlook * dist);
              float pull = 0.0f;
              for (auto const& cd : clear_dirs)
              {
                math::ray const cr(cam_pos, cd);
                selection_result const chits
                  (_world->intersectProbe(glm::transpose(model_view()), cr, cam_clear + 0.05f));
                for (auto const& ch : chits)
                {
                  if (ch.first < cam_clear)
                  {
                    pull = std::max(pull, cam_clear - ch.first);
                  }
                }
              }
              {
                math::ray const down(cam_pos, glm::vec3(0.f, -1.f, 0.f));
                float const reach = cam_clear + 0.6f;
                selection_result const dhits
                  (_world->intersectProbe(glm::transpose(model_view()), down, reach));
                float below = reach;
                for (auto const& dh : dhits)
                {
                  below = std::min(below, dh.first);
                }
                if (below < cam_clear)
                {
                  // the raw deficit, floored so shallow grazes still converge and CAPPED so the
                  // correction never over-pulls past the safe point (the old fixed 0.3 yank
                  // overshot, the glide re-advanced, and that loop was the residual bouncing)
                  pull = std::max(pull, std::clamp(cam_clear - below, 0.05f, 0.25f));
                }
              }
              if (pull <= 0.0f)
              {
                return dist; // clear here
              }
              dist = std::max(0.0f, dist - pull);
              // [perf 2026-08-16] Once the per-pass correction is sub-2cm the boom has effectively
              // converged; the remaining passes only refine by an invisible amount. Stopping here cuts
              // the 8-pass x 27-ray clearance-probe storm in the dense-city case (where every pass hits
              // the same wall and grinds all 8) to ~2-4 passes, with a <=0.02yd difference in the final
              // camera distance -- behaviour-preserving. Collision probing is the measured game-view
              // bottleneck (~13-21ms/frame); this is the biggest single ray consumer.
              if (pull < 0.02f)
              {
                break;
              }
            }
            return dist;
          };

          float const prev_actual = _game_camera_actual_distance;
          if (_game_boom_limit < _game_camera_actual_distance)
          {
            // geometry moved in behind us: SNAP in (never clip), validated at the new spot
            _game_camera_actual_distance = validate_boom(_game_boom_limit);
          }
          else
          {
            // the world (or the character) may have shifted into the current position
            float const cur = validate_boom(_game_camera_actual_distance);
            if (cur < _game_camera_actual_distance - 0.001f)
            {
              _game_camera_actual_distance = cur;
            }
            else if (_game_camera_actual_distance < _game_boom_limit - 0.01f)
            {
              // client glide-out (cameraDistanceSmoothSpeed 8.33): advance only as far as
              // validation allows; a blocked step means HOLD, not advance-and-bounce
              float const candidate =
                std::min(_game_boom_limit, _game_camera_actual_distance + dt * 8.33f);
              float const safe = validate_boom(candidate);
              if (safe > _game_camera_actual_distance + 0.05f) // real gains only: no micro creep
              {
                _game_camera_actual_distance = safe;
              }
            }
          }
          if (_game_camera_actual_distance != prev_actual)
          {
            _camera_moved_since_last_draw = true; // keep redrawing while the boom is in motion
          }
        }
      }
      else
      {
        _game_camera_actual_distance = 0.0f;
        _game_boom_limit = 0.0f;
      }
      _game_prev_boom_dist = _game_third_person_distance;

      // PLAYER CHARACTER: visible only in 3rd person, feet on the ground, facing the camera yaw
      // (the client's mouselook-follow; model dir.y and camera yaw share the same convention).
      // Animation: Stand idle / Walk / Run / Walkbackwards by input+speed; JumpStart while rising
      // fast, then Jump until landing; Fall when the ground dropped away without a jump.
      {
        // client-like: when the boom is pinched nearly into the head (tight interior), drop back to
        // first person -- don't draw the character filling the whole screen
        bool const third_person = _game_camera_actual_distance > 1.2f;
        _world->setGameCharacterVisible(third_person);
        if (third_person)
        {
          _world->setGameCharacterDisplayId(static_cast<std::uint32_t>(_game_character_display_id));
          int anim = -1; // -1 = the model's normal idle-variation Stand
          // INPUT-based like the client (movement flags), NOT displacement-based: a zero-dt tick or
          // a fully-blocked step made displacement read "stopped" for one frame -- the anim flapped
          // Run->Stand->Run 40ms apart (see the char-anim trace), each flap restarting a fresh
          // cross-fade mid-fade = the limb jerk/spin on every start/stop. Running against a wall
          // plays Run in place, exactly like the game.
          bool const moving_now = _game_grounded && (moving != 0.0f || strafing != 0.0f);
          // STRAFE = LOWER-BODY TWIST (client look): the facing stays camera-bound, but the legs
          // and hips rotate toward the strafe direction -- the client's offset table (RE'd
          // FUN_0073dab0: pure strafe pi/2, strafe+fwd/back pi/4, sign mirrored while
          // backpedaling) smoothed by its critically-damped spring (RE'd FUN_00719660: rate 20/s,
          // k-polynomial 1/(1 + x + 0.48x^2 + 0.235x^3), clamped to +-pi/2). The twist rotates
          // only the pelvis/leg bones; the SpineLow subtree counter-rotates to keep the torso
          // facing forward (Model::_lower_body_twist).
          auto const norm180 = [](float a)
          {
            while (a >= 180.0f) { a -= 360.0f; }
            while (a < -180.0f) { a += 360.0f; }
            return a;
          };
          float const spring_rate = 20.0f;
          float const spring_x = dt * spring_rate;
          float const spring_k =
            1.0f / (1.0f + spring_x + spring_x * spring_x * 0.48f + spring_x * spring_x * spring_x * 0.235f);

          // strafe twist: the client suppresses the offset/twist only while swimming or flying
          // (FUN_0073dab0 gate moveflags & 0x2200000 -> twist slots cleared); it is NOT gated on
          // falling -- the machinery keeps running off the moveflags, which FREEZE during a fall
          // (presses/releases only pend) except for the one air steer. So a running strafe-jump
          // holds its lean through the arc, and a stationary-jump air steer turns the body into
          // the same strafe posture mid-air, springing at the ground rate.
          if (_game_swimming || _game_breaching)
          {
            _game_twist = 0.0f;
            _game_twist_vel = 0.0f;
          }
          else
          {
            float const eff_moving = _game_grounded ? moving : _game_air_input_moving;
            float const eff_strafing = _game_grounded ? strafing : _game_air_input_strafing;
            float target = 0.0f;
            if (eff_strafing != 0.0f)
            {
              float const mag = (eff_moving != 0.0f) ? 0.7853982f  // pi/4 diagonal
                                                     : 1.5707964f; // pi/2 pure strafe
              float sign = (eff_strafing > 0.0f) ? -1.0f : 1.0f; // + = left; strafe right twists right
              if (eff_moving < 0.0f)
              {
                sign = -sign; // backpedal mirror
              }
              target = sign * mag;
            }
            float const delta = _game_twist - target;
            float const tmp = (delta * spring_rate + _game_twist_vel) * dt;
            _game_twist = (delta + tmp) * spring_k + target;
            _game_twist_vel = (_game_twist_vel - tmp * spring_rate) * spring_k;
            _game_twist = std::clamp(_game_twist, -1.5704823f, 1.5704823f); // client cap
          }

          // render yaw: mouselook = the body IS the facing -- it turns WITH the camera instantly
          // (client RMB behaviour), INCLUDING while airborne: the client turns freely mid-air,
          // only the trajectory is locked, never the facing. While SWIMMING the client adds one
          // wrinkle (FUN_00719b80 [0x237]): a facing flip of >90 deg in a SINGLE tick makes the
          // body SWING round at clamp01(dt*4pi) instead of teleporting; smaller turns instant.
          float const swim_dt = std::clamp(dt, 1.0f / 90.0f, 0.05f); // client smoother dt clamp
          if (_game_swimming)
          {
            auto const wrap_deg = norm180;
            float const facing_delta = wrap_deg(cam_yaw - _game_swim_prev_facing);
            if (std::abs(facing_delta) > 90.0f)
            {
              _game_swim_yaw_chasing = true;
            }
            if (_game_swim_yaw_chasing)
            {
              float const k = std::min(1.0f, swim_dt * 12.566371f); // 4pi/s
              _game_render_yaw =
                wrap_deg(_game_render_yaw + wrap_deg(cam_yaw - _game_render_yaw) * k);
              if (std::abs(wrap_deg(cam_yaw - _game_render_yaw)) < 0.6f) // ~0.01 rad snap
              {
                _game_swim_yaw_chasing = false;
                _game_render_yaw = cam_yaw;
              }
            }
            else
            {
              _game_render_yaw = cam_yaw;
            }
          }
          else
          {
            _game_render_yaw = cam_yaw; // grounded AND airborne: the body follows the facing
            _game_swim_yaw_chasing = false;
          }
          _game_swim_prev_facing = cam_yaw;

          if (_game_swimming || _game_breaching)
          {
            // client swim anim selector (RE'd FUN_00717050): STRAFE WINS -- diagonals play the
            // side strokes too (43 SwimLeft / 44 SwimRight), then backpedal 45, forward 42;
            // no direction keys (incl. ascend-only rising) = 41 SwimIdle (the idle selector,
            // FUN_0071e0d0). The breach micro-dip keeps running the same selection.
            anim = (strafing < 0.0f) ? 43
                 : (strafing > 0.0f) ? 44
                 : (moving < 0.0f) ? 45
                 : (moving > 0.0f) ? 42
                 : 41;
          }
          else if (!_game_grounded && (_game_airborne_from_jump || _game_air_time >= 0.25f))
          {
            // MICRO-AIR GRACE (non-jump): walking up/over objects or collision jitter produces
            // brief !grounded flickers; without the grace the Fall flail (40) stomped the ground
            // anims while moving over props ("fall animation plays over the walking animation").
            // A real fall exceeds the grace and takes this branch; a jump takes it immediately.
            // JumpStart plays to its FULL authored length (queried from the M2), then the Jump
            // loop holds until landing -- switching by velocity cut it off mid-pose.
            int jumpstart_ms = _world->gameCharacterAnimLengthMs(37);
            if (jumpstart_ms <= 0)
            {
              jumpstart_ms = 450;
            }
            // a jump that keeps falling past its own arc (down faster than the takeoff speed =
            // below the takeoff height) becomes a real fall -> the Fall flail takes over
            bool const still_jump_arc = _game_vertical_speed >= -(jump_speed + 0.5f);
            anim = (_game_airborne_from_jump && still_jump_arc)
                 ? (_game_air_time * 1000.0f < static_cast<float>(jumpstart_ms) ? 37 : 38)
                 : 40;                                     // Fall
          }
          else if (_game_land_anim_timer > 0.0f && _game_land_anim != 0)
          {
            // one-shot landing (client): chosen once at touchdown and NOT re-picked per tick
            // (re-picking caused the tap-W 39<->187 restart flap). But a CHANGE of movement
            // state CANCELS it PERMANENTLY (user-verified 335a): JumpEnd(39, chosen standing)
            // cancels straight into Run the moment input appears; JumpLandRun(187, chosen
            // moving) cancels to Stand if input stops. Zeroing the timer makes the cancel
            // one-way (no resume if the input flips back), and this tick already falls
            // through to the normal ground selector below.
            if ((_game_land_anim == 39 && moving_now) || (_game_land_anim == 187 && !moving_now))
            {
              _game_land_anim_timer = 0.0f;
              _game_land_anim = 0;
              anim = moving_now
                   ? ((moving < 0.0f) ? 13
                      : (run_speed > 11.0f && _world->gameCharacterAnimLengthMs(143) > 0) ? 143
                      : (run_speed > 5.0f) ? 5
                      : 4)
                   : anim; // idle keeps whatever the idle selector picked upstream
            }
            else
            {
              anim = _game_land_anim;
              _game_land_anim_timer -= dt;
            }
          }
          else if (moving_now)
          {
            // client ground selector (RE'd FUN_00717050): backpedal -> 13; current speed above
            // the FIXED sprint threshold 11.0 yd/s (DAT_00a34b20; ~1.57x the base run 7) ->
            // 143 Sprint (falls back to Run when the model lacks it, like the AnimationData
            // fallback); speed > 2 x walkSpeed (2 x 2.5, CMovement+0x90) -> 5 Run; else 4 Walk.
            // The moveSpeed playback scaling applies on top of whichever anim plays.
            anim = (moving < 0.0f) ? 13                    // Walkbackwards (client: strafe = Run + body yaw)
                 : (run_speed > 11.0f && _world->gameCharacterAnimLengthMs(143) > 0) ? 143
                 : (run_speed > 5.0f) ? 5
                 : 4;
          }

          // client "sprint": no dedicated anim -- movement anims play at unit_speed / authored
          // sequence moveSpeed, so 14 yd/s mounted-speed running doubles the Run playback rate
          float anim_scale = 1.0f;
          if (anim == 4 || anim == 5 || anim == 13 || anim == 143 || anim == 187
              || (anim >= 42 && anim <= 45))
          {
            float const authored = _world->gameCharacterAnimMoveSpeed(anim);
            float const current = (_game_swimming || _game_breaching)
                                ? ((moving < 0.0f) ? 2.5f : 4.722222f)
                                : (moving < 0.0f) ? std::min(run_speed, 4.5f) : run_speed;
            if (authored > 0.1f)
            {
              anim_scale = std::clamp(current / authored, 0.25f, 4.0f);
            }
          }
          // swim body pitch (client FUN_00719b80, exact): the target is the LOOK pitch only
          // while swimming forward/back (nose-up-positive; the camera pitch value is +down);
          // ASCEND overrides to +45 deg (the motion really is 45: speed/sqrt2 flat + up), a
          // backpedal-ascend mirrors it to -45; ANY strafe key FLATTENS the body to 0 -- the
          // side strokes swim level, even on diagonals. Idle floats level. Smoothed toward the
          // wrapped target at clamp01(dt*4pi) (dt clamped [1/90,1/20]), snapping inside 0.01
          // rad. The turn-LEAN in the same client function is FLYING-only (the caller passes
          // moveflags & 0x2000000), so swimming has NO roll. The breach dip holds the pose;
          // leaving the water otherwise drops it instantly (the client swaps transform paths).
          if (_game_swimming)
          {
            float target = 0.0f;
            if (moving != 0.0f)
            {
              // same steering gate as the movement pitch: keyboard swim is level, so the body
              // floats level too instead of nosing down with the default camera tilt
              target = _mod_space_down ? 0.7853982f
                     : look ? -glm::radians(_camera.pitch()._) : 0.0f;
              if (moving < 0.0f && _mod_space_down)
              {
                target = -target; // backpedal mirror applies to the ascend/descend override
              }
            }
            if (strafing != 0.0f)
            {
              target = 0.0f; // any strafe flattens the body
            }
            float const delta = std::remainder(target - _game_swim_pitch, 6.2831853f);
            if (std::abs(delta) < 0.01f)
            {
              _game_swim_pitch = target;
            }
            else
            {
              _game_swim_pitch += delta * std::min(1.0f, swim_dt * 12.566371f); // 4pi/s chase
            }
          }
          else if (!_game_breaching)
          {
            _game_swim_pitch = 0.0f; // out of the water the pitched transform is simply dropped
          }
          float const body_pitch_up = (_game_swimming || _game_breaching)
                                    ? glm::degrees(_game_swim_pitch) : 0.0f;
          _world->updateGameCharacter(feet, _game_render_yaw, anim, _game_twist, anim_scale, body_pitch_up,
                                      _game_anim_restart_pending);
          // ===== FOOTSTEPS (doc 38, client law): steps fire on the M2 $FSD ANIM EVENTS of the
          // playing cycle (CGUnit_C::HandleAnimEvent), never on a timer -- an anim without
          // authored events plays no steps. Resolve per fire: CreatureFootstepID x the
          // TerrainType row under the feet x wet state -> FootstepTerrainLookup normal/splash
          // SoundEntries -> SfxPlayer. Splash note: the client keys splash on liquid status
          // > 2; the observable equivalent here is standing in liquid (wading) -- the exact
          // status-tier writer is still queued for decomp.
          {
            long long const fs_now = static_cast<long long>(_world->animtime);
            if (anim != _game_fs_anim || _game_anim_restart_pending)
            {
              _game_fs_anim = anim;
              _game_fs_start_ms = fs_now;
              _game_fs_prev_t = 0;
              _game_fs_len = (anim >= 0) ? _world->gameCharacterAnimLengthMs(anim) : 0;
              _game_fs_events = (anim >= 0)
                ? _world->gameCharacterAnimEventTimes(0x44534624u /* '$FSD' */, anim)
                : std::vector<int>{};
              // FS-STATE diag (one-shot x8): the 11:20 session logged ZERO footstep fires --
              // this names whether the cycle length or the parsed $FSD events are the dead link.
              {
                static std::atomic<int> s_fs_state_left{8};
                if (anim >= 0
                    && s_fs_state_left.load(std::memory_order_relaxed) > 0
                    && s_fs_state_left.fetch_sub(1, std::memory_order_relaxed) > 0)
                {
                  LogError << "FS-STATE anim=" << anim << " len=" << _game_fs_len
                           << " events=" << _game_fs_events.size() << std::endl;
                }
              }
            }
            // GROUNDED ONLY (user-corrected 2026-08-27 + the client's own gate): $FSD footsteps
            // fire only when the unit is ON the ground (FUN_0060a740's map-height tolerance
            // check in the dispatcher) -- SWIMMING PLAYS NO PER-STROKE SOUND in the client.
            // The splash column serves WADING steps (walking in shallow water); the continuous
            // in-water sound is the WATER'S OWN loop (emitter/ambience lane, doc 38).
            if (_game_grounded && !_game_swimming && !_game_breaching
                && _game_fs_len > 0 && !_game_fs_events.empty())
            {
              int const fs_t = static_cast<int>((fs_now - _game_fs_start_ms) % _game_fs_len);
              auto const fire_between = [&](int a, int b)
              {
                for (int const et : _game_fs_events)
                {
                  if (et > a && et <= b)
                  {
                    bool const wet = water_level && water_depth > 0.0f; // wading step -> splash column
                    int const terrain_row =
                      _world->groundTerrainTypeAt(feet, glm::transpose(model_view()));
                    int const se = _world->footstepSoundEntry(_world->gameCharacterFootstepId(),
                                                              terrain_row, wet);
                    if (se > 0)
                    {
                      Noggit::Ui::SfxPlayer::instance().play(se);
                    }
                  }
                }
              };
              if (fs_t >= _game_fs_prev_t)
              {
                fire_between(_game_fs_prev_t, fs_t);
              }
              else // cycle wrapped since the last tick
              {
                fire_between(_game_fs_prev_t, _game_fs_len);
                fire_between(-1, fs_t);
              }
              _game_fs_prev_t = fs_t;
            }
            else
            {
              _game_fs_prev_t = 0;
              _game_fs_start_ms = fs_now;
            }
          }
          _game_anim_restart_pending = false;
          // breath bubbles while the head is under the surface (client hardcoded kit)
          _world->setGameCharacterBubbles(_game_swimming && water_level
                                          && (*water_level - feet.y) > 1.7f,
                                          water_level ? *water_level : 1.0e30f);
          // Camera-vs-water sticky side (see waterBoomLimit): flip BELOW only when the head is
          // clearly under (>0.4yd), back ABOVE only when it has clearly surfaced -- the ~0.35yd
          // hysteresis band absorbs the surface bob ("stepped enough into either side").
          if (water_level)
          {
            float const head_y = feet.y + eye_height;
            if (head_y < *water_level - 0.4f)
            {
              _game_cam_water_side = -1;
            }
            else if (head_y > *water_level - 0.05f)
            {
              _game_cam_water_side = 1;
            }
            else if (_game_cam_water_side == 0)
            {
              _game_cam_water_side = 1;
            }
          }
          else
          {
            _game_cam_water_side = 0;
          }
          // SURFACE RIPPLES (client Water0Ripple port, RE doc 36 -- laws extracted, cadence and
          // magnitudes approximated): WAKE rings while swimming near the surface, an entry SPLASH
          // on the walk->swim edge. Client laws kept: linear growth, alpha peak at 40% of life,
          // per-entry rotation, additive white.
          {
            bool const near_surface = water_level && (*water_level - feet.y) < 2.0f;
            // CLIENT-EXACT ripple law (2026-08-26, FUN_005fa760 fully decompiled + all constants
            // dumped -- doc 37): size0 = rand[0.60, 0.70]; growth = clamp(speed/3 x rand[0.9,1.1],
            // 1/3, 5/3) yd/s; lifetime = rand[3.0,4.5]/3 x (speed/2.5) s; alpha = FULL at the
            // surface (the 1/6 magnitude x6 trigger round-trip), depth-faded only when submerged.
            // MOVING (mode byte table 0x860a4c: only directional movement is facing-aligned):
            // cadence = 400ms x 2.5/speed; rotation = facing +- strafe offsets (fwd 0, fwd-diag
            // pi/4, strafe pi/2, back-diag 3pi/4, back pi). IDLE floats emit small random-rotation
            // pops instead: cadence 400-800ms, x0.6 growth, x0.25 lifetime, x0.8 alpha.
            float const swim_dx = feet.x - feet_before.x;
            float const swim_dz = feet.z - feet_before.z;
            float const swim_h_speed = (swim_dt > 1.0e-4f)
              ? std::sqrt(swim_dx * swim_dx + swim_dz * swim_dz) / swim_dt : 0.0f;
            bool const swim_moving = swim_h_speed > 0.1f;
            if (_game_swimming && near_surface && _world->animtime >= _game_next_wake_ms)
            {
              World::WaterRippleSpawn wake;
              wake.pos = glm::vec3(feet.x, *water_level, feet.z);
              wake.size0 = 0.60f + 0.10f * misc::frand();
              if (swim_moving)
              {
                float const spd = std::min(swim_h_speed, 20.0f);
                _game_next_wake_ms = _world->animtime
                  + std::clamp(400.0f * 2.5f / spd, 150.0f, 800.0f);
                // facing +- the client strafe-combo offset
                float rot_off = 0.0f;
                if (moving > 0.0f)      { rot_off = (strafing != 0.0f) ? 0.7853982f : 0.0f; }
                else if (moving < 0.0f) { rot_off = (strafing != 0.0f) ? 2.3561945f : 3.1415927f; }
                else if (strafing != 0.0f) { rot_off = 1.5707964f; }
                if (strafing < 0.0f) { rot_off = -rot_off; }
                // BASE ANGLE FIX (user 2026-08-26 "pointed south west instead of forwards"):
                // facing forward is (sin yaw, 0, cos yaw) but the ripple quad's U axis is
                // (cos rot, 0, sin rot) -- rot = yaw was off by (pi/2 - 2*yaw), a facing-
                // DEPENDENT skew. Base = pi/2 - yaw; the strafe mirror flips with the reversed
                // angle direction (all 8 movement combos re-derived: each wake now points along
                // the actual travel direction).
                // round 55 fixed the facing-DEPENDENT skew (base pi/2 - yaw), leaving a constant
                // quarter-turn ("90 degrees LEFT"). Round 55's -pi/2 correction went the wrong
                // way (user: "playing 180 backwards") -- the right turn is +pi/2: base = pi - yaw.
                // (The texture's directional feature runs along -V.)
                wake.rot = 3.1415927f - glm::radians(_game_render_yaw) + rot_off;
                wake.growth = std::clamp(spd / 3.0f * (0.9f + 0.2f * misc::frand()),
                                         0.33333334f, 1.6666666f);
                wake.lifetime_s = (3.0f + 1.5f * misc::frand()) / 3.0f * (spd / 2.5f);
                wake.alpha_peak = 1.0f;
              }
              else
              {
                _game_next_wake_ms = _world->animtime + 400.0f + 400.0f * misc::frand();
                wake.rot = misc::frand() * 6.2831855f;      // idle: random rotation
                wake.growth = 0.2f;                          // clamp-floor 1/3 x idle 0.6
                wake.lifetime_s = 0.25f + 0.125f * misc::frand(); // (1.0..1.5)/3 x idle 0.25... x1
                wake.alpha_peak = 0.8f;                      // idle 0.8 multiplier
              }
              wake.kind = 0;
              _world->pending_ripples.push_back(wake);
              // RIPPLE-DIAG (one-shot, always-on): proves the SPAWN stage fires.
              static bool s_ripple_spawn_diag = false;
              if (!s_ripple_spawn_diag)
              {
                s_ripple_spawn_diag = true;
                LogError << "RIPPLE-DIAG first wake spawned at (" << wake.pos.x << ","
                         << wake.pos.y << "," << wake.pos.z << ")" << std::endl;
              }
            }
            // (entry splash moved to the swim-ENTER event in the physics block above -- the old
            // was-swimming transition flag went stale across dolphin hops and ate the splash)
          }
        }
      }

      if (feet != feet_before)
      {
        _camera.position = glm::vec3(feet.x, feet.y + eye_height, feet.z);
        _camera_moved_since_last_draw = true;
      }
    }
    else
    {
      // leaving game mode: hide the player character and collapse the 3rd-person boom
      _world->setGameCharacterVisible(false);
      _game_camera_actual_distance = 0.0f;

      if (moving)
      {
        _camera.move_forward(moving, dt);
        _camera_moved_since_last_draw = true;
      }
      if (strafing)
      {
        _camera.move_horizontal(strafing, dt);
        _camera_moved_since_last_draw = true;
      }
      if (updown)
      {
        _camera.move_vertical(updown, dt);
        _camera_moved_since_last_draw = true;
      }
      // camera collision to ground
      /*
      auto ground_height = _world.get()->get_ground_height(_camera.position).y;
      if (_camera.position.y < ground_height)
      {
          _camera.position.y = ground_height + 3;
      }
      */

    }
  }
  else
  {
    //! \todo this is total bullshit. there should be a seperate view and camera class for tilemode
    if (moving)
    {
      _camera.position.z -= dt * _camera.move_speed * moving;
      _camera_moved_since_last_draw = true;
    }
    if (strafing)
    {
      _camera.position.x += dt * _camera.move_speed * strafing;
      _camera_moved_since_last_draw = true;
    }
    if (updown)
    {
      _2d_zoom *= pow(2.0f, dt * updown * 4.0f);
      _2d_zoom = std::max(0.01f, _2d_zoom);
      _camera_moved_since_last_draw = true;
    }
  }

  // _minimap->update(); // causes massive performance issues

  _world->time += this->mTimespeed * dt;
  // animtime must advance every frame (matches reference noggit3). It drives liquid
  // texture-frame cycling (lava/water churn) which must animate regardless of the model
  // animation toggle. Gating it here is what froze all lava.
  _world->animtime += dt * 1000.0f;

  lightEditor->UpdateWorldTime();

  if (_draw_model_animations.get())
  {
    // Advance the model animation clock (bones/particles) only while enabled, so toggling it off
    // pauses models in place rather than letting per-instance models keep animating.
    _world->model_animtime += dt * 1000.0f;
    _world->update_models_emitters(dt);
  }

  if (_world->has_selection())
  {
    lastSelected = currentSelection;
  }

  if (_rotation_editor_need_update)
  {
    objectEditor->rotationEditor->updateValues(_world.get());
    _rotation_editor_need_update = false;
  }

  // [PERF 2026-07-25] Throttle the status-bar + area / zone-music / detail-widget / db-status display refresh
  // to ~10Hz. All of it is human-readable status (coords, area name, FPS, loaded/rendered counts, zone-music
  // polling, the selection detail widget, water UI) that nobody perceives faster than that -- yet it ran EVERY
  // frame and cost ~4ms of the tick in Stormwind (QLabel setText churn + getAreaID / getZoneMusic DBC lookups
  // + updateDetailInfos). Camera movement and edit actions are handled ABOVE this gate, so they're unaffected.
  static double s_status_accum = 0.0;
  s_status_accum += dt;
  if (s_status_accum >= 0.1)
  {
    s_status_accum = 0.0;

  QString status;
  status += ( QString ("tile: %1 %2")
            . arg (std::floor (_camera.position.x / TILESIZE))
            . arg (std::floor (_camera.position.z / TILESIZE))
            );
  status += ( QString ("; coordinates client: (%1, %2, %3), server: (%4, %5, %6)")
            . arg (_camera.position.x)
            . arg (_camera.position.z)
            . arg (_camera.position.y)
            . arg (ZEROPOINT - _camera.position.z)
            . arg (ZEROPOINT - _camera.position.x)
            . arg (_camera.position.y)
            );

  _status_position->setText (status);

  if (currentSelection.size() > 0)
  {
    _status_selection->setText ("");
  }
  else if (currentSelection.size() == 1)
  {
    switch (currentSelection.begin()->index())
    {
    case eEntry_Object:
      {
        auto obj = std::get<selected_object_type>(*currentSelection.begin());

        if (obj->which() == eMODEL)
        {
          auto instance(static_cast<ModelInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->model->file_key().stringRepr()))
              );
        }
        else if (obj->which() == eWMO)
        {
          auto instance(static_cast<WMOInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->wmo->file_key().stringRepr()))
              );
        }

        break;
      }
    case eEntry_MapChunk:
      {
      auto chunk(std::get<selected_chunk_type>(*currentSelection.begin()).chunk);
        _status_selection->setText
          (QString ("%1, %2").arg (chunk->px).arg (chunk->py));
        break;
      }
    }
  }

  updateDetailInfos();

  unsigned int const current_area_id = _world->getAreaID (_camera.position);
  _status_area->setText
    (QString::fromStdString (gAreaDB.getAreaName (current_area_id)));

  // Drive zone music from the current area only. Only while enabled (the dropdown checkbox), so nothing
  // music-related (incl. the QtMultimedia backend) runs when off.
  // NOTE: only the MUSIC lane honours the "Enable zone music" checkbox. Ambience, the water
  // loops and the submerged duck are separate channels with their own volume sliders (the
  // client keeps EnableMusic / EnableAmbience / SoundVolume apart), so they must keep ticking
  // when music is off -- otherwise they freeze at whatever they were last set to.
  if (_zone_music_player)
  {
    bool const music_enabled = _zone_music_player->enabled();
    // Music deliberately does NOT follow the day/night cycle -- it used to swap tracks at every dawn/dusk
    // as time advanced. Always request the zone's DAY music so it stays put regardless of time of day.
    bool const is_day = true;
    // World resolves WMOAreaTable first (cities/dungeons/caves -- Ironforge, Caverns of Time), then the
    // AreaTable parent chain, for BOTH the looping ZoneMusic and the one-shot ZoneIntroMusic.
    if (music_enabled)
    {
      _zone_music_player->update_zone(_world->getZoneMusic(_camera.position),
                                      _world->getZoneIntroMusic(_camera.position), is_day);
    }
    // AMBIENCE channel (doc 38): the zone SoundAmbience day/night loop, 5.0 s crossfades on
    // change, with the client's "Underwater (DONOTRENAME)" override while the camera is
    // submerged. Same frozen-day convention as the music above.
    bool const listener_submerged = _world->renderer()->camera_underwater();
    if (!Noggit::Rendering::g_noggit_harness_silent) _zone_music_player->update_ambience(_world->getZoneAmbience(_camera.position), is_day,
                                        listener_submerged);
    // Submerged-listener duck (doc 38): the client's underwater SoundProviderPreferences EAX
    // muffle, approximated (labeled) as a volume duck on music + one-shots.
    _zone_music_player->set_submerged(listener_submerged);
    Noggit::Ui::SfxPlayer::instance().set_submerged(listener_submerged);

    // WATER LOOPS (doc 38, client FUN_00462b50): the ambient sound of nearby liquid --
    // RiverStill/Ocean/LavaPool/SlimeLoop via SoundWaterType.dbc, <=2 classes at once, 5 s
    // fades, distance-attenuated, silenced while the listener is submerged. Sampled at ~10 Hz
    // (the probe ring costs a handful of liquid lookups).
    {
      static float s_water_loop_next_ms = 0.0f;
      if (_world->animtime >= s_water_loop_next_ms)
      {
        s_water_loop_next_ms = static_cast<float>(_world->animtime) + 100.0f;
        std::array<World::WaterLoopSample, 4> near_liquid;
        _world->sampleWaterLoopSources(_camera.position, 90.0f, near_liquid);
        std::array<Noggit::Ui::WaterSoundPlayer::Source, 4> sources;
        int count = 0;
        for (int cls = 0; cls < 4; ++cls)
        {
          if (near_liquid[cls].found)
          {
            // FLOW SPEED: the client reads a per-liquid-TILE nibble (class + speed*4) from the
            // MCLQ tile flags (FUN_0068b0d0), which noggit's liquid model does not preserve.
            // On 3.3.5a data that costs nothing -- the flow variant lives in the liquid ID
            // itself (5/9 = Slow/Fast Water ...) and WaterSoundPlayer reads the row's loop
            // column. On 1.12 data (ids 1-4/21 only) speed stays STILL: exact for ocean, lava
            // pools, slime and still lakes; a flowing river sounds calm (labeled, doc 38).
            sources[count].liquid_class = cls;
            sources[count].speed = 0;
            sources[count].distance = near_liquid[cls].distance;
            sources[count].liquid_id = near_liquid[cls].liquid_id;
            ++count;
          }
        }
        if (!Noggit::Rendering::g_noggit_harness_silent) Noggit::Ui::WaterSoundPlayer::instance().update(
          sources, count, listener_submerged, _zone_music_player->ambience_volume(), true);
      }
    }
  }

  {
    int time ((static_cast<int>(_world->time) % 2880) / 2);
    std::stringstream timestrs;
    timestrs << "Time: " << (time / 60) << ":" << std::setfill ('0')
             << std::setw (2) << (time % 60);


    timestrs << ", Pres: " << _tablet_manager->pressure();

    _status_time->setText (QString::fromStdString (timestrs.str()));
  }

  _last_fps_update += dt;

  // update fps every sec
  if (_last_fps_update > 1.f && !_last_frame_durations.empty())
  {
    auto avg_frame_duration
      ( std::accumulate ( _last_frame_durations.begin()
                        , _last_frame_durations.end()
                        , 0.
                        )
      / qreal (_last_frame_durations.size())
      );
    _status_fps->setText ( "FPS: " + QString::number (int (1. / avg_frame_duration)) 
                         + " - Average frame time: " + QString::number(avg_frame_duration*1000.0) + "ms"
                         );

    _last_frame_durations.clear();
    _last_fps_update = 0.f;
  }

  _status_culling->setText ( "Loaded tiles: " + QString::number(_world->getNumLoadedTiles())
                         + " Rendered tiles: " + QString::number(_world->getNumRenderedTiles())
                         + " Loaded objects: " + QString::number(_world->getModelInstanceStorage().getTotalModelsCount())
                         + " Rendered objects: " + QString::number(_world->getNumRenderedObjects())
  );

  updateDatabaseStatus();

  guiWater->updatePos (_camera.position);
  } // end ~10Hz status-display throttle
}

glm::vec4 MapView::normalized_device_coords (int x, int y) const
{
  return {2.0f * x / width() - 1.0f, 1.0f - 2.0f * y / height(), 0.0f, 1.0f};
}

float MapView::aspect_ratio() const
{
  return float (width()) / float (height());
}

math::ray MapView::intersect_ray() const
{
  float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

  if (_display_mode == display_mode::in_3D)
  {
    // during rendering we multiply perspective * view
    // so we need the same order here and then invert.
      glm::mat4x4 const invertedViewMatrix = glm::inverse(projection() * model_view());
      auto normalisedView = invertedViewMatrix * normalized_device_coords(mx, mz);

      auto pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);

    return { _camera.position, pos - _camera.position };
  }
  else
  {
    glm::vec3 const pos
    ( _camera.position.x - (width() * 0.5f - mx) * _2d_zoom
    , _camera.position.y
    , _camera.position.z - (height() * 0.5f - mz) * _2d_zoom
    );
    
    return { pos, glm::vec3(0.f, -1.f, 0.f) };
  }
}

selection_result MapView::intersect_result(bool terrain_only, bool force_objects)
{
  selection_result results
  ( _world->intersect
    ( glm::transpose(model_view())
    , intersect_ray()
    , terrain_only
    , force_objects || terrainMode == editing_mode::object || terrainMode == editing_mode::minimap
    , _draw_terrain.get()
    , _draw_wmo.get()
    , _draw_models.get()
    , _draw_hidden_models.get()
    , _draw_wmo_exterior.get()
    )
  );

  std::sort ( results.begin()
            , results.end()
            , [](selection_entry const& lhs, selection_entry const& rhs)
              {
                return lhs.first < rhs.first;
              }
            );

  return std::move(results);
}

void MapView::enterGameModeInPlace()
{
  // Game View entry: switch WHERE THE CAMERA IS and let the tick physics take over -- gravity
  // drops the character onto whatever is below (Fall anim, real landing), and if the camera is
  // underwater the swim depth check catches it immediately instead of sinking to the lake
  // floor. The old entry snapped the camera to the nearest surface below, which teleported the
  // view down whenever Game View was toggled from a fly-height editor camera. No GL context
  // needed anymore: nothing probes here.
  _game_grounded = false;
  _game_vertical_speed = 0.0f;
  _game_airborne_from_jump = false; // falling, not jumping
  _game_air_time = 0.0f;
  _game_air_velocity = glm::vec3(0.0f); // drop straight down, no horizontal drift
  _game_air_steer_allowed = true; // no-input fall: the client's one air steer is available
  _game_air_steer_used = false;
  _game_air_input_moving = 0.0f;
  _game_air_input_strafing = 0.0f;
  _game_jump_pressed = false; // stale presses from editor mode never fire a jump
  _game_swimming = false;
  _game_breaching = false;
  // start in 3rd person at HALF the wheel range (wheel: 0..25 yd) -- the boom glides out to it
  // at the client's cameraDistanceSmoothSpeed, collision-checked, instead of opening first-person
  _game_third_person_distance = 12.5f;
  _camera_moved_since_last_draw = true;
}

std::optional<float> MapView::game_mode_ground_height(glm::vec3 const& feet, float probe_up, float max_dist)
{
  // Downward pick from probe_up above the feet: terrain + WMO + M2 doodads (M2 picking is
  // per-triangle -- ModelInstance::intersect -- so standing on crates/planks lands on real geometry,
  // not bounding boxes). do_objects/force = true so objects are pickable outside object mode; hidden
  // models excluded; interior AND exterior WMO faces count (bridges, building floors). Results carry
  // the ray distance in .first -> surface y = origin.y - t.
  glm::vec3 const origin(feet.x, feet.y + probe_up, feet.z);
  math::ray const ray(origin, glm::vec3(0.f, -1.f, 0.f));
  // reach-limited calls come from the game tick (probe cache active); unlimited = the entry snap,
  // which may probe far outside the cache radius -> full walk
  selection_result const results
    (max_dist > 0.0f
      ? _world->intersectProbe(glm::transpose(model_view()), ray, max_dist)
      : _world->intersect(glm::transpose(model_view()), ray, false, true, true, true, true, false, true, true));
  if (results.empty())
  {
    return std::nullopt;
  }
  float nearest = std::numeric_limits<float>::max();
  for (auto const& hit : results)
  {
    nearest = std::min(nearest, hit.first);
  }
  return origin.y - nearest;
}

std::optional<glm::vec3> MapView::surface_pos_under_cursor()
{
  // _cursor_pos comes from a TERRAIN-ONLY raycast, which is right for the sculpt/paint brushes but
  // wrong for placing a spawn: dragging an NPC over a building dropped it through the roof onto the
  // ground underneath. Pick against terrain + WMOs + M2s instead and take the FIRST hit -- results are
  // already sorted by ray distance, so front() is the nearest surface the cursor is actually over.
  // Any hit type works because the entry's .first is the distance along the ray; a WMO/M2 entry
  // carries no hit position of its own, so we evaluate the ray at that distance.
  math::ray const ray(intersect_ray());
  selection_result const results(intersect_result(false, true));

  if (results.empty())
  {
    return std::nullopt;
  }

  return ray.position(results.front().first);
}

void MapView::doSelection (bool selectTerrainOnly, bool mouseMove)
{
  if (_world->get_selected_model_count() && _gizmo_on.get() && (_transform_gizmo.isUsing() || _transform_gizmo.isOver()))
    return;

  selection_result results(intersect_result(selectTerrainOnly));

  if (results.empty())
  {
    _world->reset_selection();
  }
  else
  {
    auto const& hit (results.front().second);

    if (terrainMode == editing_mode::object || terrainMode == editing_mode::minimap)
    {
      float radius = 0.0f;
      switch (terrainMode)
      {
        case editing_mode::object:
         radius = objectEditor->brushRadius();
         break;

        case editing_mode::minimap:
          radius = minimapTool->brushRadius();
          break;

        default:
          break;
      }

      if (_mod_shift_down)
      {
        if (hit.index() == eEntry_Object)
        {
          if (!_world->is_selected(hit))
          {
            _world->add_to_selection(hit);
          }
          else if (!mouseMove)
          {
            _world->remove_from_selection(hit);
          }
        }
        else if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, false);
        }
      }
      else if (_mod_ctrl_down)
      {
        if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, true);
        }
      }
      else if (!_mod_space_down && !_mod_alt_down && !_mod_ctrl_down)
      {
        // objectEditor->update_selection(_world.get());
        _world->reset_selection();
        _world->add_to_selection(hit);
      }
    }
    else if (hit.index() == eEntry_MapChunk && !mouseMove)
    {
      _world->reset_selection();
      _world->add_to_selection(hit);
    }

    auto action = NOGGIT_CUR_ACTION;

    if (!action || (!action->getBlockCursor()) || !_locked_cursor_mode.get())
    {
      _cursor_pos = hit.index() == eEntry_Object ? std::get<selected_object_type>(hit)->pos
                                                 : hit.index() == eEntry_MapChunk ? std::get<selected_chunk_type>(hit).position
                                                                                  : throw std::logic_error("bad variant");
    }

  }

  _rotation_editor_need_update = true;
  objectEditor->update_selection_ui(_world.get()); 
}

void MapView::update_cursor_pos()
{
  // PERF (2026-07-21): this does a full ray-vs-terrain pick (World::intersect) EVERY frame to place the 3D
  // brush cursor -- ~7ms, measured in the tick() phase (invisible to WorldDraw but real frame cost). The
  // cursor only matters when the camera is SETTLED for editing; while you fly / look around it's wasted.
  // Skip the pick on any frame the view is still changing -- it re-picks the instant you stop, and painting
  // (camera still, mouse dragging) still picks every frame. Exact == is correct here: an unchanged camera
  // keeps identical float bits, a moving one does not.
  {
    static glm::vec3 s_last_cam_pos(std::numeric_limits<float>::max());
    static float s_last_yaw = std::numeric_limits<float>::max();
    static float s_last_pitch = std::numeric_limits<float>::max();
    static float s_last_mx = std::numeric_limits<float>::max();
    static float s_last_mz = std::numeric_limits<float>::max();
    bool const cam_still = s_last_cam_pos == _camera.position
                        && s_last_yaw == _camera.yaw()._ && s_last_pitch == _camera.pitch()._;
    s_last_cam_pos = _camera.position;
    s_last_yaw = _camera.yaw()._;
    s_last_pitch = _camera.pitch()._;
    if (!cam_still)
    {
      s_last_mx = std::numeric_limits<float>::max(); // force a re-pick the instant the camera settles
      return;
    }
    // PERF (2026-07-24): camera settled. While idle-hovering with no mouse button held, a still mouse cannot
    // move the cursor, so the World::intersect pick is redundant -- skip it. This reclaims the same ~4-7ms
    // (tick phase) when paused over a dense scene, on top of the fly-time skip above. A held button
    // (painting/sculpting) always picks, and ANY mouse move re-picks on the next frame.
    float const cur_mx = static_cast<float>(_last_mouse_pos.x());
    float const cur_mz = static_cast<float>(_last_mouse_pos.y());
    bool const mouse_still = cur_mx == s_last_mx && cur_mz == s_last_mz;
    s_last_mx = cur_mx;
    s_last_mz = cur_mz;
    if (mouse_still && !leftMouse && !rightMouse)
      return;
  }

  static bool buffer_switch = false;

  if (false && terrainMode != editing_mode::holes) // figure out why this does not work on every hardware.
  {
    float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

    //gl.readBuffer(GL_FRONT);
    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(buffer_switch)]);

    gl.readPixels(mx, height() - mz - 1, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0);

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(!buffer_switch)]);
    GLushort* ptr = static_cast<GLushort*>(gl.mapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));

    buffer_switch = !buffer_switch;

    if(ptr)
    {
      glm::vec4 viewport = glm::vec4(0, 0, width(), height());
      glm::vec3 wincoord = glm::vec3(mx, height() - mz - 1, static_cast<float>(*ptr) / std::numeric_limits<unsigned short>::max());

      glm::mat4x4 model_view_ = model_view();
      glm::mat4x4 projection_ = projection();

      glm::vec3 objcoord = glm::unProject(wincoord, model_view_,projection_, viewport);


      TileIndex tile({objcoord.x, objcoord.y, objcoord.z});

      if (!_world->mapIndex.tileLoaded(tile))
      {
        gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
        gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
      }

      _cursor_pos = {objcoord.x, objcoord.y, objcoord.z};

      gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return;
  }

  // Spawn tools aim at a SURFACE, not at the ground: the cursor must sit on a WMO floor/roof or a
  // doodad when you hover one, so the aim circle is visible there and a click places the spawn there.
  // The brush tools below stay terrain-only -- sculpting/painting acts on chunks, so a WMO hit would
  // be meaningless for them.
  if (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
  {
    if (auto const surface = surface_pos_under_cursor())
    {
      _cursor_pos = *surface;
    }
    return;
  }

  // use raycasting for holes

  selection_result results (intersect_result (true));

  if (!results.empty())
  {
    auto const& hit(results.front().second);
    // hit cannot be something else than a chunk
    auto const& chunkHit = std::get<selected_chunk_type>(hit);
    _cursor_pos = chunkHit.position;

  }
}

glm::vec3 MapView::game_camera_look_direction() const
{
  // The VIEW direction in game mode: character facing (_camera yaw/pitch) plus the LMB orbit
  // offsets. Built exactly like Camera::direction() so the two agree when the orbit is zero.
  float const yaw = _camera.yaw()._ + _game_orbit_yaw;
  float const pitch = std::clamp(_camera.pitch()._ + _game_orbit_pitch, -80.0f, 80.0f);
  glm::vec4 const forward(0.0f, 0.0f, 1.0f, 0.0f);
  auto const rotation = glm::quat(glm::vec3(glm::radians(pitch), glm::radians(yaw), 0.0f));
  return glm::normalize(glm::vec3(glm::mat4_cast(rotation) * forward));
}

void MapView::clampGameBoomPreDraw()
{
  if (!_game_mode_camera.get() || _game_camera_actual_distance <= 0.01f || !_world)
  {
    return;
  }
  // same near-plane-volume sweep as the tick (head -> desired camera + 4 near-plane corners),
  // but with THIS frame's look direction; clamp is one-way (never extends -- the tick's
  // validated glide owns growth)
  glm::vec3 const head(_camera.position);
  glm::vec3 const look(game_camera_look_direction());
  float const dist = _game_camera_actual_distance;
  glm::vec3 const cam_des(head - look * dist);
  float const near_d = 0.25f;
  float const half_h = near_d * std::tan(_camera.fov()._ * 0.5f);
  float const half_w = half_h * aspect_ratio();
  glm::vec3 const right(glm::normalize(glm::cross(look, glm::vec3(0.f, 1.f, 0.f))));
  glm::vec3 const up(glm::cross(right, look));
  glm::vec3 const corners[5] = {
    glm::vec3(0.0f),
    right * half_w + up * half_h,
    right * half_w - up * half_h,
    -right * half_w + up * half_h,
    -right * half_w - up * half_h,
  };
  float limit = dist;
  for (auto const& corner : corners)
  {
    glm::vec3 const to((cam_des + corner) - head);
    float const len = glm::length(to);
    if (len < 0.001f)
    {
      continue;
    }
    math::ray const sweep(head, to / len);
    selection_result const hits(_world->intersectProbe(glm::transpose(model_view()), sweep, len + 0.5f));
    for (auto const& hit : hits)
    {
      if (hit.first < len)
      {
        limit = std::min(limit, std::max(0.0f, (hit.first / len) * dist - 0.1f));
      }
    }
  }
  // (round 26) boom-distance water wall retired -- see the tick site; the render-eye Y push
  // (snapCameraOffWaterSurface) is the surface-vs-camera mechanism now.

  if (limit < _game_camera_actual_distance)
  {
    _game_camera_actual_distance = limit;
  }
}

namespace
{
  // Liquid surface near a camera position: the camera's own XZ column is not enough -- hanging
  // over a dry walkway edge at canal-surface height, the column probe returns nothing while the
  // water plane still crosses the frame (user round 23). Sample a small neighbourhood and take
  // the surface closest to the camera height.
  std::optional<float> nearby_liquid_surface(World* world, glm::vec3 const& p)
  {
    // RING SAMPLING (round 24: ±2yd was far too small -- the plane renders edge-on across the
    // whole frame from many yards beyond the nearest wet column): centre + 8 directions at radii
    // 2/6/12/20yd. Take the surface closest to the camera height.
    std::optional<float> best;
    auto consider = [&](float x, float z)
    {
      auto const lvl = world->getLiquidHeightAt(glm::vec3(x, p.y, z));
      if (lvl && (!best || std::abs(*lvl - p.y) < std::abs(*best - p.y)))
      {
        best = lvl;
      }
    };
    consider(p.x, p.z);
    static float const radii[4] = {2.0f, 6.0f, 12.0f, 20.0f};
    for (float r : radii)
    {
      for (int d = 0; d < 8; ++d)
      {
        float const a = static_cast<float>(d) * 0.785398f; // 45 deg steps
        consider(p.x + r * std::cos(a), p.z + r * std::sin(a));
      }
    }
    return best;
  }
}

// WATER SURFACE = CAMERA WALL (client behaviour, user rounds 17-18): the boom may not cross the
// liquid surface. The allowed side is the STICKY _game_cam_water_side (hysteresis on the
// character's submersion -- flipping per-frame with the head bob caused the "camera keeps
// bouncing" fight between the tick's boom glide and the pre-draw clamp; the sticky side +
// applying this limit in BOTH places stops it). The margin includes the near-plane half-height so
// the frustum corners can't clip the plane even when the boom centre clears it.
// RETIRED round 26 (kept for reference; no longer called -- the render-eye Y push replaced it).
[[maybe_unused]] float MapView::waterBoomLimit(glm::vec3 const& head, glm::vec3 const& look, float dist) const
{
  if (!_world || dist <= 0.0f)
  {
    return dist;
  }
  glm::vec3 const cam_end(head - look * dist);
  auto lvl = nearby_liquid_surface(_world.get(), cam_end);
  if (!lvl)
  {
    lvl = nearby_liquid_surface(_world.get(), head);
  }
  if (!lvl)
  {
    return dist;
  }
  // Round 24: side==0 used to DISARM the wall entirely -- exactly the case of the character on
  // dry ground with the boom swinging over the canal (the surviving grazing view). With no sticky
  // swim side, the side is simply where the HEAD is relative to the found surface.
  int const side = (_game_cam_water_side != 0) ? _game_cam_water_side
                                               : (head.y >= *lvl ? 1 : -1);
  float const near_d = 0.25f;
  float const near_half_h = near_d * std::tan(_camera.fov()._ * 0.5f);
  float const margin = 0.1f + near_half_h;
  // The camera must stay at y >= surface+margin (side above) / y <= surface-margin (side below).
  float const bound = *lvl + (side > 0 ? margin : -margin);
  bool const end_ok = (side > 0) ? (cam_end.y >= bound) : (cam_end.y <= bound);
  if (end_ok)
  {
    return dist;
  }
  float const dy = cam_end.y - head.y;
  if (std::abs(dy) < 0.0001f)
  {
    return dist; // level boom on the forbidden side of a level plane: nothing sane to clamp to
  }
  float const t = (bound - head.y) / dy;
  if (t <= 0.0f)
  {
    return 0.0f; // even the head side of the boom is past the bound: pinch fully in
  }
  return std::min(dist, t * dist);
}

// The water surface is a hard boundary for the CAMERA POSITION itself (user round 21): the band
// |y - surface| < band is unreachable -- above the plane the camera is held at surface+band, and
// the frame the motion carries it past the plane it JUMPS to surface-band (and vice versa). The
// near plane (0.25 -> half-height ~0.15) then never intersects the surface, so the edge-on
// grazing view is impossible and the point-test underwater state always matches what's on screen.
void MapView::snapCameraOffWaterSurface()
{
  _game_render_eye_dy = 0.0f;
  if (!_world || _display_mode != display_mode::in_3D)
  {
    return;
  }

  // GAME MODE (round 26 -- the ACTUAL failing mode; rounds 21-25 gated the snap off here and only
  // the boom-distance wall ran, which never lifts the eye off the plane). Do NOT move the character
  // (_camera.position = head -> "trapped under ice"). Push the RENDERED BOOM EYE (head - look*dist)
  // off the surface band instead; the same push feeds model_view() and render_eye, so the view and
  // the underwater test (keyed on render_eye) always sit on the same side -> no half-in grazing.
  if (_game_mode_camera.get())
  {
    if (_game_camera_actual_distance <= 0.01f)
    {
      return; // 1st person: eye == head, follows the character's own swim state
    }
    glm::vec3 const look(game_camera_look_direction());
    glm::vec3 const eye(_camera.position - look * _game_camera_actual_distance);
    auto const lvl = nearby_liquid_surface(_world.get(), eye);
    if (!lvl)
    {
      return;
    }
    float constexpr band = 0.35f; // clears the near plane (0.25 * tan(fov/2) ~ 0.14) with margin
    float const d = eye.y - *lvl;
    if (d < band && d > -band)
    {
      float const target = *lvl + (d >= 0.0f ? band : -band);
      _game_render_eye_dy = target - eye.y;
      _camera_moved_since_last_draw = true;
    }
    return;
  }

  // EDITOR camera: the eye IS _camera.position, so snap it directly. Dry-column beside water gets a
  // full-yard band (the plane crosses the frame from yards away); a wet column keeps the tight 0.30
  // so dive/surface flips are instant.
  auto const lvl = nearby_liquid_surface(_world.get(), _camera.position);
  if (!lvl)
  {
    return;
  }
  bool const column_wet = _world->getLiquidHeightAt(_camera.position).has_value();
  float const band = column_wet ? 0.30f : 1.00f;
  float const d = _camera.position.y - *lvl;
  if (d >= band || d <= -band)
  {
    return;
  }
  _camera.position.y = *lvl + (d >= 0.0f ? band : -band);
  _camera_moved_since_last_draw = true;
}

glm::mat4x4 MapView::model_view() const
{
  if (_display_mode == display_mode::in_2D)
  {
    glm::vec3 eye = _camera.position;
    glm::vec3 target = eye;
    target.y -= 1.f;
    target.z -= 0.001f;
    auto center = target;
    auto up = glm::vec3(0.f, 1.f, 0.f);

    return glm::lookAt(eye, target, up);
  }
  else
  {
    // [game mode] 3rd person: the eye rides a boom behind the head along the look direction, still
    // aimed at the head -- exactly the client's orbit camera. _game_camera_actual_distance is the
    // wheel distance AFTER the pull-in collision computed in the game tick, so the boom camera can
    // never be inside geometry. Every consumer (render, frustum, picking) sees the same matrix.
    bool const boom_active = _game_mode_camera.get()
      && (_game_camera_actual_distance > 0.01f
          || _game_orbit_yaw != 0.0f || _game_orbit_pitch != 0.0f);
    if (boom_active)
    {
      glm::vec3 const look(game_camera_look_direction());
      glm::vec3 eye(_camera.position - look * _game_camera_actual_distance);
      eye.y += _game_render_eye_dy; // water-surface push (round 26) -- same value as render_eye
      return glm::lookAt(eye, eye + look, glm::vec3(0.f, 1.f, 0.f));
    }
    return _camera.look_at_matrix();
  }
}

glm::mat4x4 MapView::projection() const
{
  float far_z = _settings->value("farZ", 900).toFloat();

  if (_display_mode == display_mode::in_2D)
  {
    float half_width = width() * 0.5f * _2d_zoom;
    float half_height = height() * 0.5f * _2d_zoom;

    return glm::ortho(-half_width, half_width, -half_height, half_height, -1.f, far_z);
  }
  else
  {
    // Client-like NEAR PLANE everywhere (the client runs ~0.222): at the old editor near=1.0 the
    // near rectangle was ~1.0x1.6 yd -- geometry a half-yard beside the camera cut through it long
    // before the camera was actually close ("camera has a bigger hitbox than the view"). 0.25
    // shrinks the clip volume to client size; with farZ ~2000 the depth ratio stays ~8000:1,
    // comfortably inside 24-bit depth precision.
    float const near_p = 0.25f;
    return glm::perspective(_camera.fov()._, aspect_ratio(), near_p, far_z);
  }
}

// [VK-DIFF 2026-08-29] GL-vs-Vulkan parity harness (self-verifying VK port, no eyeballing).
// NOGGIT_VK_DIFF=1 together with NOGGIT_VK=1 (and WITHOUT NOGGIT_VK_FULL, both renderers must draw): every
// frame the GL scene (default FBO, before the VK preview blit) and the VK image (the imported GL texture,
// after glWaitSemaphoreEXT) are read back and compared pixel-wise; a running "[VK-DIFF] live ..." line is
// logged every ~60 frames. NOGGIT_VK_DIFF_CAMS=<file> ("name x y z yaw pitch" per line, '#' comments) makes
// the harness step through the cameras: settle NOGGIT_VK_DIFF_SETTLE frames (default 90, lets streaming
// finish), then dump <NOGGIT_VK_DIFF_DIR>/<name>_gl.png, _vk.png, _diff.png (default dir "vk_diff" under
// the exe cwd) and log one "[VK-DIFF] cam=<name> ..." line; after the last camera a PASS/FAIL summary.
// PASS = mean abs error <= NOGGIT_VK_DIFF_TOL LSB (default 1.0) AND pixels off by > 8 LSB <= 0.5 %.
// Pure readback + CPU compare; never touches the semaphore choreography (fail-soft like the rest).
#include <fstream>
namespace vk_diff
{
  struct Cam { std::string name; glm::vec3 pos; float yaw; float pitch; };
  struct Stats { double mean = 0.0; int max = 0; double pct_off = 0.0; std::size_t pixels = 0;
                 double bias_r = 0.0, bias_g = 0.0, bias_b = 0.0; }; // signed VK-GL per channel

  // [VULKAN 2026-08-29] settings-driven (Settings > Graphics > "Graphics API" / "Vulkan parity check"),
  // loaded in MapView::initializeGL. The old NOGGIT_VK / NOGGIT_VK_FULL / NOGGIT_VK_DIFF env vars remain
  // as dev overrides (env OR settings) so nothing the user must edit.
  inline int& apiMode()      { static int m = 0; return m; }      // 0 GL, 1 VK preview, 2 VK full view
  inline bool& parityCheck() { static bool p = false; return p; }
  inline std::string& camsPath() { static std::string s; return s; }
  // self-run parity loop (--vk-parity-*): forced() = ignore QSettings, finished() = harness summary logged
  inline bool& forced()   { static bool f = false; return f; }
  inline bool& finished() { static bool f = false; return f; }
  inline bool& vkReady() { static bool r = false; return r; } // VK backend up + textured mesh uploaded
  inline int& meshTileX() { static int v = -1; return v; } // VK neighbourhood centre tile (parity gate)
  inline int& meshTileZ() { static int v = -1; return v; }
  inline bool vkOn()   { return apiMode() >= 1 || std::getenv("NOGGIT_VK") != nullptr; }
  inline bool vkFull() { return apiMode() == 2 || std::getenv("NOGGIT_VK_FULL") != nullptr; }
  inline bool enabled()
  {
    static bool const e = (parityCheck() || std::getenv("NOGGIT_VK_DIFF") != nullptr) && vkOn() && !vkFull();
    return e;
  }
  // [VULKAN NATIVE PRESENT, 2026-09-03] Settings "Vulkan" now means NATIVE: the backend owns a
  // swapchain on a child window; GL neither composes nor blits the frame. The GL-import compose
  // survives ONLY as the parity harness's instrument (enabled()) and as the
  // NOGGIT_VK_NO_PRESENT=1 dev A/B fallback.
  inline bool nativeView()
  {
    static bool const n = vkOn() && !enabled() && std::getenv("NOGGIT_VK_NO_PRESENT") == nullptr;
    return n;
  }
  inline double envf(char const* name, double def)
  {
    char const* v = std::getenv(name);
    return (v && *v) ? std::atof(v) : def;
  }
  inline std::string outDir()
  {
    char const* v = std::getenv("NOGGIT_VK_DIFF_DIR");
    std::string d = (v && *v) ? v : "vk_diff";
    QDir().mkpath(QString::fromStdString(d));
    return d;
  }
  inline std::vector<Cam> loadCams()
  {
    std::vector<Cam> cams;
    char const* env = std::getenv("NOGGIT_VK_DIFF_CAMS");
    std::string const f = (env && *env) ? std::string(env) : camsPath(); // default: <exe dir>/vk_diff_cams.txt
    if (f.empty()) return cams;
    std::ifstream in(f);
    if (!in) { LogError << "[VK-DIFF] no camera list at " << f << " -> live mode only" << std::endl; return cams; }
    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      Cam c;
      if (ss >> c.name >> c.pos.x >> c.pos.y >> c.pos.z >> c.yaw >> c.pitch) cams.push_back(c);
    }
    LogError << "[VK-DIFF] camera list " << f << ": " << cams.size() << " cameras" << std::endl;
    return cams;
  }
  // a, b = RGBA8, same size/orientation. diff (optional) = RGBA8 heat image (|d| * 4, alpha 255).
  inline Stats compare(std::uint8_t const* a, std::uint8_t const* b, int w, int h,
                       std::vector<std::uint8_t>* diff, int off_threshold = 8,
                       float const* vk_depth = nullptr) // depth >= 1 (VK background/sky) -> pixel excluded
  {
    Stats s;
    std::size_t const total = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    s.pixels = 0;
    if (diff) diff->assign(total * 4u, 255u);
    double sum = 0.0;
    std::size_t off = 0;
    for (std::size_t i = 0; i < total; ++i)
    {
      if (vk_depth && vk_depth[i] >= 1.f) { if (diff) { (*diff)[i * 4] = 0; (*diff)[i * 4 + 1] = 0; (*diff)[i * 4 + 2] = 64; } continue; }
      ++s.pixels;
      int pmax = 0;
      for (int c = 0; c < 3; ++c)
      {
        int const d = std::abs(int(a[i * 4 + c]) - int(b[i * 4 + c]));
        sum += d;
        if (d > pmax) pmax = d;
        if (diff) (*diff)[i * 4 + c] = static_cast<std::uint8_t>(std::min(255, d * 4));
      }
      if (pmax > s.max) s.max = pmax;
      if (pmax > off_threshold) ++off;
      s.bias_r += int(b[i * 4]) - int(a[i * 4]);
      s.bias_g += int(b[i * 4 + 1]) - int(a[i * 4 + 1]);
      s.bias_b += int(b[i * 4 + 2]) - int(a[i * 4 + 2]);
    }
    s.mean = s.pixels ? sum / double(s.pixels * 3u) : 0.0;
    if (s.pixels)
    {
      s.bias_r /= double(s.pixels); s.bias_g /= double(s.pixels); s.bias_b /= double(s.pixels);
    }
    s.pct_off = s.pixels ? 100.0 * double(off) / double(s.pixels) : 0.0;
    return s;
  }
  inline void savePng(std::string const& path, std::uint8_t const* rgba, int w, int h)
  {
    QImage img(rgba, w, h, w * 4, QImage::Format_RGBA8888);
    img.mirrored(false, true).save(QString::fromStdString(path)); // GL rows are bottom-up
  }
}

void MapView::draw_map()
{
  ZoneScoped;
  // [GL-COST HUNT] start of the frame's real work; closed at whichever point this api reaches the
  // scene traversal, so the pre-traversal part of draw_map is comparable between GL and VK.
  auto const t_paint0 = std::chrono::steady_clock::now();
  // [VK-DIFF] camera stepping must run BEFORE render_eye below: the world shaders render CAMERA-RELATIVE
  // (vertices subtract camera_pos = render_eye), so stepping the camera after it made GL draw the whole
  // scene relative to the PREVIOUS camera -- GL and VK then showed different viewpoints entirely.
  // [VK-DIFF] camera-list stepping runs at the TOP of the frame: the GL scene is drawn later in this
  // same frame, so GL and VK both render THIS camera (stepping it inside the VK block left the GL
  // readback one camera behind -> "VK shows different geometry" that was really a stale GL frame).
  // [VK-DIFF] camera-list stepping (runs BEFORE this frame's VK render; the GL scene above already drew
  // with the previous camera, so a settle window is required anyway -- streaming needs it too).
  static std::vector<vk_diff::Cam> const s_diff_cams = vk_diff::enabled() ? vk_diff::loadCams()
                                                                          : std::vector<vk_diff::Cam>{};
  static std::size_t s_diff_cam_i = 0;
  static int s_diff_settle = 0;
  static bool s_diff_done = s_diff_cams.empty();
  static std::size_t s_diff_pass = 0, s_diff_fail = 0;
  bool diff_capture_now = false;
  std::string diff_capture_name;
  // (VK readiness is not visible here -- the interop struct lives in the VK block below; the harness
  //  only steps once the backend has produced a textured mesh, which vk_diff::ready() records.)
  // [HARNESS-ONLY, 2026-09-16] vkReady() is raised by the terrain tile pack; a WMO-only map (Molten
  // Core) packs no tile, so the camera list below never applied and every capture stood at the
  // enterMapAt corner outside the dungeon. The global WMO having finished loading is that map's
  // readiness.
  if (Noggit::Rendering::g_noggit_harness_silent && vk_diff::enabled() && !s_diff_done && !vk_diff::vkReady()
      && _world->mapIndex.hasAGlobalWMO())
  {
    auto global_wmo = _world->getModelInstanceStorage().get_wmo_instance(_world->mWmoEntry.uniqueID);
    if (global_wmo.has_value() && global_wmo.value()->wmo->finishedLoading())
    {
      vk_diff::vkReady() = true;
    }
  }
  // HARNESS ONLY. This teleports the camera to the parity camera list, so it must never run in an
  // interactive session -- with render/vk_parity_check on it flung the user out of the world the
  // moment VK reported ready, on whatever map they had open.
  if (Noggit::Rendering::g_noggit_harness_silent && vk_diff::enabled() && !s_diff_done && vk_diff::vkReady())
  {
    int const settle_frames = static_cast<int>(vk_diff::envf("NOGGIT_VK_DIFF_SETTLE", 90.0));
    vk_diff::Cam const& c = s_diff_cams[s_diff_cam_i];
    if (s_diff_settle == 0)
    {
      _camera.position = c.pos;
      _camera.yaw(math::degrees(c.yaw));
      _camera.pitch(math::degrees(c.pitch));
      _camera_moved_since_last_draw = true;
      _needs_redraw = true; // paintGL early-outs when not dirty -> without this the GL frame can be stale
      LogError << "[VK-DIFF] camera " << (s_diff_cam_i + 1) << "/" << s_diff_cams.size() << " '" << c.name
               << "' pos=(" << c.pos.x << "," << c.pos.y << "," << c.pos.z << ") yaw=" << c.yaw
               << " pitch=" << c.pitch << " settling " << settle_frames << " frames" << std::endl;
    }
    TileIndex const cam_tile(c.pos);
    bool const mesh_centred = vk_diff::meshTileX() == static_cast<int>(cam_tile.x)
                           && vk_diff::meshTileZ() == static_cast<int>(cam_tile.z);
    if (++s_diff_settle > settle_frames && (mesh_centred || s_diff_settle > 900))
    {
      LogError << "[VK-DIFF] capture '" << c.name << "' cam tile (" << cam_tile.x << "," << cam_tile.z << ") vk mesh centre ("
               << vk_diff::meshTileX() << "," << vk_diff::meshTileZ() << ")" << (mesh_centred ? "" : " NOT CENTRED") << std::endl;
      diff_capture_now = true;
      diff_capture_name = c.name;
      s_diff_settle = 0;
      if (++s_diff_cam_i >= s_diff_cams.size()) s_diff_done = true;
    }
    _needs_redraw = true; // keep frames flowing while the harness runs
    update();
  }

  //! \ todo: make the current tool return the radius
  float radius = 0.0f, inner_radius = 0.0f, angle = 0.0f, orientation = 0.0f;
  glm::vec3 ref_pos;
  bool angled_mode = false, use_ref_pos = false;

  _cursorType = CursorType::CIRCLE;

  switch (terrainMode)
  {
  case editing_mode::ground:
    radius = terrainTool->brushRadius();
    inner_radius = terrainTool->innerRadius();
    if ((terrainTool->_edit_type != eTerrainType_Vertex || terrainTool->_edit_type != eTerrainType_Script) && terrainTool->getImageMaskSelector()->isEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::flatten_blur:
    radius = flattenTool->brushRadius();
    angle = flattenTool->angle();
    orientation = flattenTool->orientation();
    ref_pos = flattenTool->ref_pos();
    angled_mode = flattenTool->angled_mode();
    use_ref_pos = flattenTool->use_ref_pos();
    break;
  case editing_mode::paint:
    radius = texturingTool->brush_radius();
    inner_radius = texturingTool->hardness();
    if(texturingTool->getTexturingMode() == Noggit::Ui::texturing_mode::paint && texturingTool->getImageMaskSelector()->isEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::stamp:
    radius = stampTool->getRadius();
    inner_radius = stampTool->getInnerRadius();
    if(stampTool->getActiveBrushItem() && stampTool->getActiveBrushItem()->isMaskEnabled())
      _cursorType = CursorType::STAMP;
    break;
  case editing_mode::water:
    radius = guiWater->brushRadius();
    angle = guiWater->angle();
    orientation = guiWater->orientation();
    ref_pos = guiWater->ref_pos();
    angled_mode = guiWater->angled_mode();
    use_ref_pos = guiWater->use_ref_pos();
    break;
  case editing_mode::mccv:
    radius = shaderTool->brushRadius();
      if(shaderTool->getImageMaskSelector()->isEnabled())
        _cursorType = CursorType::STAMP;
    break;
  case editing_mode::areaid:
    radius = ZoneIDBrowser->brushRadius();
    break;
  case editing_mode::holes:
    radius = holeTool->brushRadius();
    break;
  case editing_mode::object:
    radius = objectEditor->brushRadius();
    break;
  case editing_mode::creature:
  case editing_mode::gameobject:
    // No brush in the spawn tools -- show a fixed aim circle on the ground under the cursor.
    // Small on purpose: it marks a placement POINT, so a wide ring just obscures what you are
    // aiming at. (Was 3.5, then 0.7.)
    radius = 0.4f;
    break;
  case editing_mode::minimap:
    radius = minimapTool->brushRadius();
    break;
  case editing_mode::scripting:
    radius = scriptingTool->get_settings()->brushRadius();
    inner_radius = scriptingTool->get_settings()->innerRadius();
    break;
  default:
    break;
  }

  //! \note Select terrain below mouse, if no item selected or the item is map.
  if (!_capture_probe
    && !(_world->has_selection()
    || _locked_cursor_mode.get()))
  {
    noggit::perf::Scoped _prof_sel(noggit::perf::Phase::Selection);
    doSelection(true);
  }

  if (_camera_moved_since_last_draw)
  {
      // PERF (2026-07-21): _minimap->update() schedules a full repaint of the minimap widget, which Qt
      // runs in the event loop BETWEEN paintGL calls -- i.e. inside the frame-to-frame gap, invisible to
      // WorldDraw but counted in Frame. Firing it every camera-move frame (i.e. every frame while panning)
      // is the "massive performance issues" flagged in tick() above. 4 Hz is ample for an overview marker.
      static QElapsedTimer s_minimap_throttle;
      if (!s_minimap_throttle.isValid() || s_minimap_throttle.elapsed() > 250)
      {
        _minimap->update();
        s_minimap_throttle.restart();
      }
  }

  bool classic_ui = _settings->value("classicUI", true).toBool();
  bool show_unpaintable = classic_ui ? texturingTool->show_unpaintable_chunks() : _left_sec_toolbar->showUnpaintableChunk();
  // [game mode] the world shaders render CAMERA-RELATIVE (the jitter fix: vertices subtract
  // camera_pos, the view matrix contributes rotation only) -- so the VISIBLE eye position is the
  // camera_pos argument below, not model_view's translation. The 3rd-person boom therefore has to
  // be applied to camera_pos too, or it cancels out exactly and the wheel zoom renders nothing.
  glm::vec3 render_eye =
    (_game_mode_camera.get() && _game_camera_actual_distance > 0.01f)
      ? _camera.position - game_camera_look_direction() * _game_camera_actual_distance
      : _camera.position;
  // Water-surface eye push (round 26): keep the render eye out of the surface band so the
  // underwater test (which keys on render_eye) matches what's on screen. Same dy model_view adds.
  if (_game_mode_camera.get() && _game_camera_actual_distance > 0.01f)
  {
    render_eye.y += _game_render_eye_dy;
  }
  // [VK-1c] NOGGIT_VK_FULL: preview mode where VULKAN renders the world -- skip the entire GL scene draw
  // (the VK interop block below fills the viewport instead). Editor overlays/tools that live inside the GL
  // draw are absent in this mode; it exists to fly the VK renderer and read its true frame cost.
  static bool const s_vk_full_view = vk_diff::vkOn() && vk_diff::vkFull(); // settings (Graphics API) or env
  // [VULKAN phase A] the GL scene draw is a lambda so the VK block below can run it AFTER VK has rendered
  // this frame's image and GL has waited on it (the compose hook then lays VK colour+depth under the scene).
  // Pure-GL / VK-not-ready paths call it at the end of this function exactly as before.
  bool gl_scene_drawn = false;
  auto const draw_gl_scene = [&]()
  {
  gl_scene_drawn = true;
  // How much of a VK frame is still GL's scene pass? That pass is also where every VK feed is built,
  // so this number is the ceiling on what "stop using GL when Vulkan is on" can actually save.
  VkPhaseTimer _t_glscene(vk_stat_glscene_ms());
  VkPhaseTimer _t_glscene_any(vk_stat_glscene_any_ms());
  // finding 92: GL calls issued INSIDE the walk, which decides whether it can leave this thread
  struct GlCallCounter
  {
    std::size_t start = OpenGL::gl_call_count();
    ~GlCallCounter() { vk_stat_walk_glcalls() += double(OpenGL::gl_call_count() - start); }
  } _walk_gl;
  // [spike hunt] ~10 of the 12 in-flight spikes per 900 frames are in code no VK timer covers, i.e.
  // in this walk. Snapshot the section accumulators around THIS frame and report the frames that
  // stall, with the split -- a 300-frame average hides them completely.
  extern double g_vk_sec_terrain_ms, g_vk_sec_wmo_ms, g_vk_sec_m2_ms;
  auto const _ss_t0 = std::chrono::steady_clock::now();
  double const _ss_terr = g_vk_sec_terrain_ms, _ss_wmo = g_vk_sec_wmo_ms, _ss_m2 = g_vk_sec_m2_ms;
  {
    // reported here (not in the VK block) so the number exists in GL mode too
    static int s_gls_frames = 0;
    if (++s_gls_frames >= 300)
    {
      LogError << "[VK] WALK GL CALLS/frame=" << (vk_stat_walk_glcalls() / s_gls_frames) << std::endl;
      vk_stat_walk_glcalls() = 0.0;
      if (OpenGL::gl_call_histo_on())
      {
        std::vector<std::pair<char const*, std::size_t>> v(OpenGL::gl_call_histo().begin(),
                                                          OpenGL::gl_call_histo().end());
        std::sort(v.begin(), v.end(),
                  [](auto const& a, auto const& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < v.size() && i < 18; ++i)
          LogError << "[VK] GLCALL " << (v[i].second / std::size_t(s_gls_frames))
                   << "/frame  " << v[i].first << std::endl;
        OpenGL::gl_call_histo().clear();
      }
      LogError << "[VK] SCENEPASS(any api)=" << (vk_stat_glscene_any_ms() / s_gls_frames)
               << " ms/frame  PRE-TRAVERSAL(any api)=" << (vk_stat_prevk_ms() / s_gls_frames)
               << " ms/frame" << std::endl;
      s_gls_frames = 0;
      vk_stat_glscene_any_ms() = 0.0;
      vk_stat_prevk_ms() = 0.0;
    }
  }
  if (!s_vk_full_view)
  {
  _world->renderer()->draw (
                 model_view()
               , projection()
               , _cursor_pos
               , _cursorRotation
               , terrainMode == editing_mode::mccv ? shaderTool->shaderColor() : cursor_color
               , _cursorType
               , radius
               , show_unpaintable
               , _left_sec_toolbar->drawOnlyInsideSphereLight()
               , _left_sec_toolbar->drawWireframeSphereLight()
               , _left_sec_toolbar->getAlphaSphereLight()
               , inner_radius
               , ref_pos
               , angle
               , orientation
               , use_ref_pos
               , angled_mode
               , terrainMode == editing_mode::paint
               , terrainMode
               , render_eye
               , _camera_moved_since_last_draw
               , _draw_mfbo.get()
               , _draw_terrain.get()
               , _draw_wmo.get()
               , _draw_water.get()
               , _draw_wmo_doodads.get()
               , _draw_models.get()
               , _draw_model_animations.get()
               , _draw_models_with_box.get()
               , _draw_hidden_models.get()
               , minimapTool->getMinimapRenderSettings()
               , _draw_fog.get()
               , terrainTool->_edit_type
               , _display_all_water_layers.get() ? -1 : _displayed_water_layer.get()
               , _display_mode
               , _draw_occlusion_boxes.get()
               ,false
               , _draw_wmo_exterior.get()
               , _draw_bloom.get()
               , _draw_ground_clutter.get()
               );
  // [PIPELINE step 1] Buckets Vulkan refused were collected during the walk instead of being drawn
  // inside it; issue them here, still on the render thread. Once the walk moves to a worker this
  // call stays exactly where it is.
  _world->renderer()->drawDeferredGlFallback();
  }

  {
    double const _ss_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - _ss_t0).count();
    // walk-frame counter: the bench's own SPIKE log only covers the MEASURED window, so without an
    // index there is no way to tell a warm-up stall from an in-flight one.
    static unsigned _ss_frame = 0;
    ++_ss_frame;
    if (_ss_ms > 12.0)
      LogError << "[VK] SCENE SPIKE f" << _ss_frame << " " << _ss_ms << " ms | terrain="
               << (g_vk_sec_terrain_ms - _ss_terr) << " wmo=" << (g_vk_sec_wmo_ms - _ss_wmo)
               << " m2=" << (g_vk_sec_m2_ms - _ss_m2)
               // the terrain section only runs when VK does NOT own terrain, so a non-zero terrain
               // number means ownership was lost this frame and GL redrew the whole thing
               << " | ownsTerrain=" << (_world->renderer()->vk_owns_terrain ? 1 : 0)
               << " ttReady=" << (s_vk_backend_tt_ready() ? 1 : 0)
               << std::endl;
  }
  // reset after each world::draw call
  _camera_moved_since_last_draw = false;
  }; // draw_gl_scene

#ifdef _WIN32
  // [VULKAN PHASE 0 -- interop proof of life, 2026-08-07] NOGGIT_VK=1: bring up the Vulkan backend, share an
  // image + semaphores with this GL context (EXT_memory_object_win32 / EXT_semaphore_win32), have VK clear it
  // to an animated colour each frame and blit it into the viewport corner. Proves the whole VK->GL bridge the
  // real scene passes will ride (VK renders, GL composites -- Qt UI untouched). Fails soft to pure GL.
  {
    static bool const s_vk_on = vk_diff::vkOn(); // Settings > Graphics > Graphics API (or NOGGIT_VK env)
    if (s_vk_on)
    {
      // Everything VK mode ADDS to a frame: feed building, uploads, record+submit, compose and blit.
      // With the GPU wait now ~0.03 ms the frame is entirely CPU-bound, and this is the half of it
      // that is not GL's scene pass.
      vk_stat_prevk_ms() += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t_paint0).count();
      VkPhaseTimer _t_vkblock(vk_stat_vkblock_ms());
      VkPhaseTimer _t_vkblock2(vk_stat_block2_ms());   // same span, harness-window reporting
      vk_stat_blk_frames() += 1.0;
      // [phase J] VK (or parity) will consume this frame's feeds, so WorldRender may build them.
      // The pure-GL fallback below clears this again before it draws, so a GL frame builds nothing.
      _world->renderer()->vk_feeding = true;
      auto const t_prep0 = std::chrono::steady_clock::now();
      auto t_prepA = t_prep0, t_prepB = t_prep0;
      auto t_after_prep = t_prep0;   // block-scope copy of t_prep_end, for the MID split below
      auto t_s2a = t_prep0, t_s2b = t_prep0, t_s2c = t_prep0;
      auto t_sk1 = t_prep0, t_sk2 = t_prep0, t_sk3 = t_prep0;
      struct VkInterop
      {
        Noggit::Rendering::VK::VulkanBackend backend;
        GLuint memobj = 0, tex = 0, fbo = 0, sem_vk_done = 0, sem_gl_done = 0;
        bool tried = false, ok = false, first_frame = true;
        bool native = false; // [NATIVE PRESENT] backend presents; every GL interop path is skipped
        // [VULKAN phase A] depth import + compose: the VK D32 depth image imported as a GL depth texture;
        // a fullscreen program writes VK colour + gl_FragDepth into the cleared scene target before the
        // GL passes (WorldRender::pre_scene_compose). compose_ok false -> old corner preview only.
        GLuint depth_memobj = 0, depth_tex = 0, compose_vao = 0;
        bool compose_ok = false;
        std::unique_ptr<OpenGL::program> compose_program;
        // EXT_memory_object / EXT_semaphore entry points (not in the gl wrapper)
        void (QOPENGLF_APIENTRYP pCreateMemoryObjects)(GLsizei, GLuint*) = nullptr;
        void (QOPENGLF_APIENTRYP pImportMemoryWin32Handle)(GLuint, GLuint64, GLenum, void*) = nullptr;
        void (QOPENGLF_APIENTRYP pTexStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
        void (QOPENGLF_APIENTRYP pGenSemaphores)(GLsizei, GLuint*) = nullptr;
        void (QOPENGLF_APIENTRYP pImportSemaphoreWin32Handle)(GLuint, GLenum, void*) = nullptr;
        void (QOPENGLF_APIENTRYP pWaitSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
        void (QOPENGLF_APIENTRYP pSignalSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
      };
      static VkInterop s_vk;

        // [VULKAN] BLP file name -> bindless texture id. Defined once here so the WMO batch
        // table and the M2 batches share it, and therefore share s_vk_tex_ids: a texture used
        // by both is uploaded once and keeps a single bindless id.
        // [finding 85] Two modes. Blocking is the original behaviour. Non-blocking returns the
        // sentinel -2 for a name that is not cached and not yet decoded, after handing it to the
        // background decoder -- so a caller that can wait a frame does not stall the render thread
        // on an archive read. -1 still means "resolved, but there is no texture".
        static constexpr std::int32_t kBlpPending = -2;
        auto vkCreateFromDecoded = [&](std::string const& name, VkPfDecoded const& d) -> std::int32_t
        {
          std::int32_t id = -1;
          if (d.ok)
            id = d.compressed ? s_vk.backend.addTextureCompressed(d.vf, d.w, d.h, d.mips)
                              : s_vk.backend.addTextureMips(d.mips, d.w, d.h);
          s_vk_tex_ids.emplace(name, id);
          // [2026-09-03 BLACK-MODEL DIAG] a name that resolves to no bindless texture draws its
          // batches BLACK (-1). The -1 is cached, so this logs exactly once per name.
          if (id < 0 && !name.empty())
            LogError << "[VK] BLP unresolved (draws black): '" << name
                     << "' decode_ok=" << (d.ok ? 1 : 0) << std::endl;
          {
            std::lock_guard<std::mutex> lk(s_pf_mtx);
            s_pf_seen.erase(name);
          }
          return id;
        };
        auto vkResolveBlpEx = [&](std::string const& name, bool blocking) -> std::int32_t
        {
          if (name.empty()) return -1;
          auto it = s_vk_tex_ids.find(name);
          if (it != s_vk_tex_ids.end()) return it->second;
          VkPfDecoded ready;
          bool have_ready = false;
          {
            std::unique_lock<std::mutex> lk(s_pf_mtx);
            auto rit = s_pf_ready.find(name);
            if (rit != s_pf_ready.end())
            {
              ready = std::move(rit->second);
              s_pf_ready.erase(rit);
              have_ready = true;
            }
            else if (!blocking)
            {
              if (!s_pf_thread.joinable())
                s_pf_thread = std::thread(vkTilesetDecodeLoop, _context);
              if (s_pf_seen.insert(name).second)
              {
                s_pf_queue.push_back(name);
                s_pf_cv.notify_all();
              }
              return kBlpPending;
            }
          }
          // the texture is created OUTSIDE the decoder lock in both paths
          return vkCreateFromDecoded(name, have_ready ? ready : vkDecodeTileset(name, _context));
        };
        auto vkResolveBlp = [&](std::string const& name) -> std::int32_t
        {
          return vkResolveBlpEx(name, true);
        };


      constexpr GLenum GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_ = 0x9587;
      constexpr GLenum GL_LAYOUT_GENERAL_EXT_ = 0x958D;
      // [VK-1c] viewport-sized shared image (queried once at init; a later window resize keeps rendering at
      // the init size and scales in the blit -- proper swap-on-resize comes with the real integration).
      static std::uint32_t VK_W = 512, VK_H = 512;

      // [phase B] latch the VK image size only after the Qt layout has settled (docks/status bar shrink the
      // viewport during the first frames -> a size mismatch would disable the parity comparison for good)
      // [NATIVE 2026-09-03] the 30-frame wait existed for the PARITY compare (size latch after the
      // Qt layout settles). In native mode it meant 30 frames of the PURE-GL world on screen and
      // then a visible GL->VK handover: full scene -> flash -> everything restreams. Native now
      // initialises on the FIRST frame -- the GL scene is never shown, the swapchain sizes to the
      // window on its own, and the offscreen blit scales if the layout still settles afterwards.
      static int s_vk_warmup_frames = 0;
      ++s_vk_warmup_frames;
      if (!s_vk.tried && s_vk_warmup_frames > (vk_diff::nativeView() ? 0 : 30))
      {
        s_vk.tried = true;
        {
          GLint ivp[4] = {0, 0, 0, 0};
          gl.getIntegerv(GL_VIEWPORT, ivp);
          if (ivp[2] > 63 && ivp[3] > 63)
          {
            VK_W = static_cast<std::uint32_t>(ivp[2]);
            VK_H = static_cast<std::uint32_t>(ivp[3]);
            // [2026-09-06] NOGGIT_VK_FORCE_SIZE=WxH: the harness window cannot exceed the monitor,
            // so the user's 2288x1329 offscreen target could never be reproduced by resizing the
            // window. Force the render-target extent directly (present blit scales to the surface).
            if (char const* fs = std::getenv("NOGGIT_VK_FORCE_SIZE"))
            {
              unsigned fw = 0, fh = 0;
              if (std::sscanf(fs, "%ux%u", &fw, &fh) == 2 && fw >= 64 && fh >= 64)
              { VK_W = fw; VK_H = fh; }
            }
          }
        }
        auto* ctx = QOpenGLContext::currentContext();
        auto gp = [&](char const* n) { return ctx ? ctx->getProcAddress(n) : nullptr; };
        s_vk.pCreateMemoryObjects = reinterpret_cast<decltype(s_vk.pCreateMemoryObjects)>(gp("glCreateMemoryObjectsEXT"));
        s_vk.pImportMemoryWin32Handle = reinterpret_cast<decltype(s_vk.pImportMemoryWin32Handle)>(gp("glImportMemoryWin32HandleEXT"));
        s_vk.pTexStorageMem2D = reinterpret_cast<decltype(s_vk.pTexStorageMem2D)>(gp("glTexStorageMem2DEXT"));
        s_vk.pGenSemaphores = reinterpret_cast<decltype(s_vk.pGenSemaphores)>(gp("glGenSemaphoresEXT"));
        s_vk.pImportSemaphoreWin32Handle = reinterpret_cast<decltype(s_vk.pImportSemaphoreWin32Handle)>(gp("glImportSemaphoreWin32HandleEXT"));
        s_vk.pWaitSemaphore = reinterpret_cast<decltype(s_vk.pWaitSemaphore)>(gp("glWaitSemaphoreEXT"));
        s_vk.pSignalSemaphore = reinterpret_cast<decltype(s_vk.pSignalSemaphore)>(gp("glSignalSemaphoreEXT"));

        // [VULKAN NATIVE PRESENT, 2026-09-03] Settings "Vulkan" = native: the backend gets its own
        // swapchain on a child window and GL touches nothing. The GL-import compose below survives
        // untouched for the parity harness (enabled()) and the NOGGIT_VK_NO_PRESENT dev fallback.
        s_vk.native = vk_diff::nativeView();
        // [MSAA] mirror GL's SCENE MSAA -- Settings "render/msaa" (default 4, NOGGIT_MSAA A/B
        // override), the same source WorldRender reads for its _msaa_fbo. The GL SURFACE has no
        // samples (SamplesCount 0), so format().samples() would read 0. GL applies it live; VK
        // applies it at map (re)open, like the Graphics API dropdown itself. Parity/compose = 1.
        int vk_msaa = _settings->value("render/msaa", 4).toInt();
        if (char const* msaa_env = std::getenv("NOGGIT_MSAA"))
          vk_msaa = std::atoi(msaa_env);
        if (vk_msaa != 2 && vk_msaa != 4 && vk_msaa != 8)
          vk_msaa = 1;
        if (s_vk.native && s_vk.backend.init(VK_W, VK_H, static_cast<std::uint32_t>(vk_msaa)))
        {
          if (s_vk.backend.presentCapable())
          {
            s_vk.ok = true;
            s_vk.backend.setFrameSemExternallyWaited(false);
            g_vk_capture_backend = &s_vk.backend;
            if (!_vk_present_requested)
            {
              _vk_present_requested = true;
              QMetaObject::invokeMethod(this, [this]() { ensureVkPresentSurface(); }, Qt::QueuedConnection);
            }
            LogError << "[VK] NATIVE mode: GL interop skipped; present surface requested" << std::endl;
          }
          else
          {
            // a driver without surface/swapchain support keeps the proven GL-import compose
            LogError << "[VK] native present unavailable -- falling back to GL compose" << std::endl;
            s_vk.native = false;
          }
        }
        if (!s_vk.native && !s_vk.ok
            && (!s_vk.pCreateMemoryObjects || !s_vk.pImportMemoryWin32Handle || !s_vk.pTexStorageMem2D
                || !s_vk.pGenSemaphores || !s_vk.pImportSemaphoreWin32Handle
                || !s_vk.pWaitSemaphore || !s_vk.pSignalSemaphore))
        {
          LogError << "[VK] GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32 not available -- interop off" << std::endl;
        }
        else if (!s_vk.native && !s_vk.ok && (s_vk.backend.ready() || s_vk.backend.init(VK_W, VK_H)))
        {
          s_vk.pCreateMemoryObjects(1, &s_vk.memobj);
          s_vk.pImportMemoryWin32Handle(s_vk.memobj, s_vk.backend.imageMemorySize(),
                                        GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_, s_vk.backend.imageMemoryHandle());
          gl.genTextures(1, &s_vk.tex);
          gl.bindTexture(GL_TEXTURE_2D, s_vk.tex);
          s_vk.pTexStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, VK_W, VK_H, s_vk.memobj, 0);
          gl.bindTexture(GL_TEXTURE_2D, 0);
          s_vk.pGenSemaphores(1, &s_vk.sem_vk_done);
          s_vk.pImportSemaphoreWin32Handle(s_vk.sem_vk_done, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_, s_vk.backend.vkDoneSemaphoreHandle());
          s_vk.pGenSemaphores(1, &s_vk.sem_gl_done);
          s_vk.pImportSemaphoreWin32Handle(s_vk.sem_gl_done, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_, s_vk.backend.glDoneSemaphoreHandle());
          gl.genFramebuffers(1, &s_vk.fbo);
          GLint prev_fbo = 0;
          gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
          gl.bindFramebuffer(GL_FRAMEBUFFER, s_vk.fbo);
          gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_vk.tex, 0);
          gl.bindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
          s_vk.ok = true;
          LogError << "[VK] GL interop imported (memobj + texture + semaphores) -- proof-of-life overlay active" << std::endl;

          // [VULKAN phase A] import the exported D32 depth image + build the compose program. Any failure
          // -> compose_ok stays false (corner preview path), never touches the colour route above.
          // Parity-check mode deliberately does NOT compose: the harness must compare GL-only vs VK-only.
          // DEPTH AS COLOUR (2026-08-29): the D32 image import read as garbage (probe: -inf/3e38/NaN --
          // depth tiling metadata is not importable); VK now writes gl_FragCoord.z into an exported R32F
          // COLOUR attachment and GL imports that exactly like the RGBA8 image (proven path).
          if (s_vk.backend.zMemoryHandle()) // (also in parity mode: the depth masks sky pixels out of the diff)
          {
            while (glGetError() != GL_NO_ERROR) {} // clear stale errors before probing the import
            s_vk.pCreateMemoryObjects(1, &s_vk.depth_memobj);
            s_vk.pImportMemoryWin32Handle(s_vk.depth_memobj, s_vk.backend.zMemorySize(),
                                          GL_HANDLE_TYPE_OPAQUE_WIN32_EXT_, s_vk.backend.zMemoryHandle());
            gl.genTextures(1, &s_vk.depth_tex);
            gl.bindTexture(GL_TEXTURE_2D, s_vk.depth_tex);
            s_vk.pTexStorageMem2D(GL_TEXTURE_2D, 1, GL_R32F, VK_W, VK_H, s_vk.depth_memobj, 0);
            gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.bindTexture(GL_TEXTURE_2D, 0);
            GLenum const err = glGetError();
            if (err == GL_NO_ERROR)
            {
              try
              {
                s_vk.compose_program.reset(new OpenGL::program(
                  {{ GL_VERTEX_SHADER, R"(#version 330 core
out vec2 uv;
void main()
{
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})" },
                   { GL_FRAGMENT_SHADER, R"(#version 330 core
// [VULKAN phase A] VK image row 0 == GL bottom row (the un-flipped blit proved the orientation), so uv
// samples straight. VK depth is already in GL window-depth convention: terrain.vert maps GL clip z to
// VK's [0,1] with (z+w)*0.5, which equals GL's (ndc*0.5+0.5) window z for the same projection.
in vec2 uv;
uniform sampler2D vk_color;
uniform sampler2D vk_depth;
uniform float near_z;      // projection near (0.25)
uniform float far_z;       // projection far (settings farZ)
uniform float max_view_z;  // GL terrain/view distance: VK content beyond it is discarded (GL draws sky/fog there)
// 1 when VULKAN owns the sky dome. Then GL is NOT drawing a sky, and the far-plane discard below
// would throw away the only sky there is: the dome draws with no depth interaction (it is meant to
// sit behind everything), so its pixels are at depth 1.0 exactly like an empty background. Keeping
// them is the difference between a sky and a black void -- the bug that made VK render black sky
// whenever the camera looked up, invisible to a parity check whose cameras all look down.
uniform int vk_owns_sky;
out vec4 out_color;
void main()
{
  float d = texture(vk_depth, uv).r;
  if (d >= 1.0)
  {
    if (vk_owns_sky == 0) discard;                        // GL still draws its own sky there
    out_color = texture(vk_color, uv);
    gl_FragDepth = 0.999995;                              // behind all GL geometry, still in front of nothing
    return;
  }
  float z = near_z * far_z / (far_z - d * (far_z - near_z)); // window depth -> view distance
  if (z > max_view_z) discard;
  out_color = texture(vk_color, uv);
  // bias so GL wins depth TIES: while a pass is still drawn by BOTH renderers (same triangles), GL's
  // version must cover VK's (no z-fighting); once a pass moves to VK, GL stops drawing it and VK shows.
  // SLOPE-SCALED: the scene target is MSAA (per-sample depth) while this quad writes ONE depth per pixel;
  // on sloped surfaces a fixed bias lost to the sample offset at grazing angles (view-dependent streaks).
  // fwidth(d) = depth change per pixel -> 1.5px of slope + a floor covers every sample position.
  float slope = fwidth(d);
  gl_FragDepth = min(d + max(4e-6, 1.5 * slope), 0.99999);
})" }}));
                gl.genVertexArrays(1, &s_vk.compose_vao);
                s_vk.compose_ok = true;
                LogError << "[VK] phase A compose ready: depth imported (D32 -> GL_DEPTH_COMPONENT32F), VK draws UNDER the editor" << std::endl;
              }
              catch (std::exception const& e)
              {
                LogError << "[VK] compose program failed: " << e.what() << " -- corner preview only" << std::endl;
              }
            }
            else
            {
              LogError << "[VK] depth import rejected (GL error 0x" << std::hex << err << std::dec << ") -- corner preview only" << std::endl;
            }
          }
        }
      }

      if (s_vk.ok && s_vk.backend.ready())
      {
        // [VULKAN NATIVE PRESENT] attach the swapchain as soon as the queued child window exists,
        // and declare native ownership of every pass to the renderer for this frame.
        if (s_vk.native)
        {
          if (!s_vk.backend.presentActive() && _vk_present_window)
            s_vk.backend.initPresent(reinterpret_cast<void*>(_vk_present_window->winId()));
          auto* wrn = _world->renderer();
          wrn->vk_native = true;
          wrn->setVkOwnsWmo(true);
          wrn->setVkOwnsCelestials(true);
          if (wrn->skies())
            wrn->skies()->setVkOwnsDome(true);
        }
        else
        {
          _world->renderer()->vk_native = false;
        }
        // [VK-1b/1c] feed the REAL terrain to the backend: the nearest tile + its loaded 3x3 neighbourhood
        // (rebuilt when the nearest tile changes). Chunk layout: 145 verts (9x9 outer + 8x8 centre,
        // 17-stride interleave), 4 tris per cell.
        static MapTile* s_vk_tile = nullptr;
        // [phase I] per-chunk draw ranges + bounds, so the terrain can be frustum-culled per frame
        // without touching the (tile-bound) mesh itself.
        struct VkChunkBounds
        {
          std::uint32_t first_index, index_count;
          glm::vec3 centre;
          float radius;
        };
        static std::vector<VkChunkBounds> s_vk_chunk_bounds;
        // Same for the ADT water: it is packed per 8x8 sub-chunk and was drawn in ONE unculled
        // draw over every liquid surface in the neighbourhood.
        static std::vector<VkChunkBounds> s_vk_water_bounds;

        // ADT-water half of the liquid mesh, re-packed only when the terrain neighbourhood is.
        MapTile* best = nullptr;
        float bestd = std::numeric_limits<float>::max();
        // distance computed HERE from the tile origin (xbase/zbase), NOT tile->camDist(): camDist is filled
        // by the GL cull loop inside WorldRender::draw, which the VK_FULL mode skips entirely -- relying on
        // it left every tile's distance frozen and picked a wrong/far tile (the "all blue" bug).
        for (MapTile* t : _world->mapIndex.loaded_tiles())
        {
          if (!t || !t->finishedLoading())
            continue;
          float const cx = t->xbase + 266.6666f; // TILESIZE/2
          float const cz = t->zbase + 266.6666f;
          float const dx = _camera.position.x - cx;
          float const dz = _camera.position.z - cz;
          float const d2 = dx * dx + dz * dz;
          if (d2 < bestd)
          {
            bestd = d2;
            best = t;
          }
        }
        // rebuild when the centre tile changes OR when more of its 3x3 neighbourhood finishes loading --
        // building only on centre-change froze the mesh at ONE tile (built before the neighbours streamed
        // in, never refreshed -> the world visibly ended at the tile border).
        static std::size_t s_vk_tile_count = 0;
        std::vector<MapTile*> vk_tiles;
        vk_tiles.reserve(9);
        if (best)
        {
          for (MapTile* t : _world->mapIndex.loaded_tiles())
          {
            // [VULKAN parity 08-29] EXACTLY GL's terrain cull (WorldRender::draw): a tile draws when its
            // CENTRE is within view_distance + TILESIZE of the camera. The old index-ring drew 5x5 corner
            // tiles at ~1508yd that GL culls at 1433 -> 8-23% "onlyVK" pixels in the geometry probe.
            // (GL's other two terrain skips don't change pixels here: frustum-culled tiles are off-screen,
            //  and occlusion culling is hard-disabled / WDL horizon is opt-in default off.)
            static float const s_vk_cull = _settings->value("view_distance", 900.f).toFloat() + 533.33333f;
            if (!t || !t->finishedLoading())
              continue;
            glm::vec3 const t_centre(t->index.x * 533.33333f + 266.66666f,
                                     (t->getExtents()[0].y + t->getExtents()[1].y) * 0.5f,
                                     t->index.z * 533.33333f + 266.66666f);
            if (glm::distance(_camera.position, t_centre) <= s_vk_cull)
            {
              vk_tiles.push_back(t);
            }
          }
        }
        // doodad instance-buffer availability changes AFTER tiles finish (they build lazily during the GL
        // draw) -- include their count in the rebuild trigger so late-built buffers get picked up.
        static std::size_t s_vk_dood_count = 0;
        std::size_t vk_dood_count = 0;
        for (MapTile* t : vk_tiles)
          vk_dood_count += t->renderer()->doodadInstanceBuffers().size();
        // WMOs stream in independently of tiles -- count the loaded ones near the camera so a late-arriving
        // building triggers a rebuild too (cheap: pointer walk under the storage mutex).
        _world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& wi)
        {
          if (wi.finishedLoading())
            ++vk_dood_count;
        });
        // Debounce: while content is still streaming in (counts changing frame to frame), wait for ~20
        // stable frames before rebuilding -- otherwise every arriving WMO/doodad-buffer forces a full
        // queue-idle re-extract and the first minute hitches. Tile-centre changes rebuild immediately.
        static std::size_t s_vk_seen_count = 0;
        static int s_vk_stable_frames = 0;
        if (vk_dood_count == s_vk_seen_count)
          ++s_vk_stable_frames;
        else
        {
          s_vk_seen_count = vk_dood_count;
          s_vk_stable_frames = 0;
        }
        bool const content_grew = vk_dood_count != s_vk_dood_count && (s_vk_stable_frames >= 20 || s_vk_tile == nullptr);
        // [phase B] rebuild throttle: a textured rebuild re-packs 2304 chunks (+ decodes new tilesets), so
        // neighbourhood-growth rebuilds are rate-limited; a nearest-tile change still rebuilds at once.
        static std::chrono::steady_clock::time_point s_vk_last_rebuild{};
        auto const now_tp = std::chrono::steady_clock::now();
        bool const tile_changed = best && best != s_vk_tile;
        bool const throttled = !tile_changed
          && std::chrono::duration<float>(now_tp - s_vk_last_rebuild).count() < 2.0f;
        { VkPhaseTimer _t_tile(vk_stat_tile_ms());
        // [spike hunt, MapView side] A spike frame is ~200 ms of which setTerrainTextured is ~54 ms.
        // The backend instrumentation cannot see the rest because it happens HERE -- packing the
        // neighbourhood's vertices, indices, chunk records, alphamaps and tilesets. Report the whole
        // rebuild and let the difference against the backend's own numbers locate what is left.
        struct RebuildTimer
        {
          std::chrono::steady_clock::time_point t0{ std::chrono::steady_clock::now() };
          ~RebuildTimer()
          {
            double const ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
            if (ms > 30.0)
              LogError << "[VK-SLOW] MapView::tileRebuild = " << ms << " ms" << std::endl;
          }
        } _rebuild_timer;
        // Phase split of the rebuild, so the 170 ms is attributed rather than guessed at.
        auto _rb_t0 = std::chrono::steady_clock::now();
        double _rb_geom = 0.0, _rb_water = 0.0;
        // The pack loop decodes BLPs and creates VK textures INLINE. That is the only backend
        // call inside it, so it is both a likely part of the geom cost and the one thing that
        // would have to be hoisted before the pack could run on a worker.
        double _rb_tex = 0.0;
        int _rb_tex_n = 0;
        double _rb_pf_decode = 0.0, _rb_pf_upload = 0.0;
        int _rb_pf_n = 0, _rb_pf_hit = 0, _rb_pf_miss = 0;
        // [SPIKE FIX -- PACK BUDGET] Packing the whole neighbourhood in one frame is what makes a
        // crossing (and especially the first build: 587 ms) a visible freeze. Now that every tile owns
        // a stable slot, packing can be SPREAD: at most kPackBudget tiles are packed per frame, the
        // rest keep their slots and are picked up on following frames. A tile that has not been packed
        // yet simply is not drawn for a frame or two -- a brief pop-in instead of a stall.
        static bool s_pack_pending = false;
        if (best && (!throttled || s_pack_pending)
            && (best != s_vk_tile || vk_tiles.size() != s_vk_tile_count || content_grew || s_pack_pending))
        {
          s_vk_last_rebuild = now_tp;
          // kept across rebuilds: these are ~60 MB and ~30 MB for a full neighbourhood and were
          // allocated, filled and FREED every tile crossing
          static constexpr std::uint32_t kTileSlots   = 64u;                  // 8 x 8
          static constexpr std::uint32_t kChunkSlots  = kTileSlots * 256u;    // 16384
          static constexpr std::uint32_t kAtlasPerRow = 80u;                  // chunks per atlas row
          static constexpr std::uint32_t kAtlasRows   = (kChunkSlots + kAtlasPerRow - 1u) / kAtlasPerRow;
          static constexpr std::uint32_t kAtlasW      = kAtlasPerRow * 64u;
          static constexpr std::uint32_t kAtlasH      = kAtlasRows * 64u;
          // [STABLE SLOTS -- geometry] The vertex/index/cidx streams now live in the SAME fixed slot
          // space as the atlas, so a tile that is still resident keeps its geometry untouched across a
          // crossing. Before this they were re-derived and re-copied in full every crossing (~145 MB),
          // which was the whole remaining rebuild hitch (8 rebuilds x ~120 ms per 900-frame flight).
          static constexpr std::uint32_t kVertsPerChunk = 145u;
          static constexpr std::uint32_t kIdxPerChunk   = 8u * 8u * 12u;   // 768, holes zero-filled
          static std::vector<float> vk_verts;
          static std::vector<std::uint32_t> vk_idx;
          // [cold start] The slot buffers are ~470 MB in total and assign() zero-fills all of it on
          // the first rebuild; time it so the cold-start stall is attributed rather than assumed.
          auto const _alloc_t0 = std::chrono::steady_clock::now();
          bool _alloc_did = false;
          if (vk_verts.size() != std::size_t(kChunkSlots) * kVertsPerChunk * 9u)
          { vk_verts.assign(std::size_t(kChunkSlots) * kVertsPerChunk * 9u, 0.f); _alloc_did = true; }
          if (vk_idx.size() != std::size_t(kChunkSlots) * kIdxPerChunk)
            vk_idx.assign(std::size_t(kChunkSlots) * kIdxPerChunk, 0u);
          // byte ranges touched this rebuild, one contiguous span per repacked tile
          static std::vector<std::pair<std::size_t, std::size_t>> dirty_v, dirty_i, dirty_c;
          dirty_v.clear(); dirty_i.clear(); dirty_c.clear();
          std::uint32_t tile_base = 0;
          bool mesh_ok = !vk_tiles.empty();
          // [SPIKE FIX] s_vk_chunk_bounds is a STATIC that was only ever appended to, so every tile
          // crossing added a whole neighbourhood (~11.5k entries) on top of the last one: the list
          // doubled per rebuild (11515 -> 23030 -> ...). Two consequences, both bad and both growing
          // the longer you fly: the per-frame frustum loop below walks the entire list every frame,
          // and every stale entry's first_index/index_count refers to an index buffer that has since
          // been rebuilt, so they address geometry that no longer exists. Bounds describe the CURRENT
          // neighbourhood only -- rebuild them from scratch with it.
          s_vk_chunk_bounds.clear();
          // textured-terrain side streams: per-vertex chunk index, per-chunk data, 64x64 alphamap
          // (RGBA8: r,g,b = layers 1..3) + 64x64 shadow (R8) blocks, tileset -> bindless id
          bool const tt_path = s_vk.backend.terrainTexturedAvailable();
          // [SPIKE FIX] Same problem as vk_alpha/vk_shadow: these are ~60 MB and ~30 MB for a full
          // neighbourhood and were allocated, filled and FREED on every rebuild. reserve() only stops
          // the regrowth -- it does not stop the malloc/free churn, and a rebuild happens every tile
          // crossing. Kept across rebuilds; reserve still covers the first one and any growth.
          // [SPIKE FIX -- STABLE SLOTS] Every tile owns a FIXED slot derived from its map
          // coordinates, so a tile that is still resident after a crossing keeps its atlas cells and
          // its chunk records and does not have to be re-packed at all. (tx & 7, tz & 7) cannot
          // collide inside a 7x7 neighbourhood. This is the only thing that helps: caching the
          // packed bytes does not, because the cost IS the data volume -- copying ~300 MB out of a
          // cache costs what rebuilding it costs.
          static std::vector<std::uint32_t> vk_cidx;
          static std::vector<Noggit::Rendering::VK::VulkanBackend::TerrainChunk> vk_chunks;
          if (vk_cidx.size() != std::size_t(kChunkSlots) * 145u)
            vk_cidx.assign(std::size_t(kChunkSlots) * 145u, 0u);
          // NOT cleared per rebuild: a skipped tile's records must survive. Sized once.
          if (vk_chunks.size() != kChunkSlots) vk_chunks.assign(kChunkSlots, {});
          // [SPIKE FIX] These were plain locals with NO reserve, grown by resize(+16 KB) once per
          // chunk -- ~11k incremental resizes building a 193 MB array, each one able to reallocate
          // and copy everything written so far, and thrown away every rebuild. That is the bulk of a
          // ~385 ms tile-rebuild frame (setTerrainTextured itself only accounts for ~65 ms of it).
          // Kept across rebuilds and reserved up front so the fill is a straight write.
          // These now hold the atlas EXACTLY as the GPU image is laid out, so the backend copies
          // them straight in with no repack (that repack was a ~193 MB memcpy per crossing).
          // Persistent: only the rows belonging to a newly resident tile are rewritten.
          static std::vector<std::uint8_t> vk_alpha, vk_shadow;
          static constexpr std::size_t kAtlasTexels = static_cast<std::size_t>(kAtlasW) * kAtlasH;
          if (vk_alpha.size() != kAtlasTexels * 4u) vk_alpha.assign(kAtlasTexels * 4u, 0);
          if (vk_shadow.size() != kAtlasTexels) { vk_shadow.assign(kAtlasTexels, 0); _alloc_did = true; }
          if (_alloc_did)
            LogError << "[VK] slot buffer first-touch: "
                     << std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - _alloc_t0).count()
                     << " ms (verts+idx+cidx+alpha+shadow)" << std::endl;
          // which tile currently owns each slot, and which atlas rows this rebuild dirtied
          static MapTile* s_slot_owner[kTileSlots] = {};
          // [finding 117] the pointer alone does not prove identity: a freed MapTile can be
          // replaced at the same address, and slots repeat across neighbourhoods. Stamp the index.
          static int s_slot_ix[kTileSlots], s_slot_iz[kTileSlots];
          static bool s_slot_stamp_init = false;
          if (!s_slot_stamp_init)
          {
            for (std::uint32_t i = 0; i < kTileSlots; ++i) { s_slot_ix[i] = -1; s_slot_iz[i] = -1; }
            s_slot_stamp_init = true;
          }
          auto const slot_owned_by = [&](std::uint32_t slot, MapTile* t)
          {
            return s_slot_owner[slot] == t
                && s_slot_ix[slot] == t->index.x && s_slot_iz[slot] == t->index.z;
          };
          // how many indices each chunk slot actually uses (holes make it < kIdxPerChunk); kept so a
          // resident tile can report its spans without re-deriving them
          static std::vector<std::uint32_t> s_slot_idx_count;
          if (s_slot_idx_count.size() != kChunkSlots) s_slot_idx_count.assign(kChunkSlots, 0u);
          std::uint32_t dirty_row_lo = kAtlasRows, dirty_row_hi = 0u;
          // how many tiles may be (re)packed this frame; the rest wait for the next one
          // one atlas row-band per repacked tile (its 256 chunk slots are contiguous, so ~4 rows)
          static std::vector<std::pair<std::uint32_t, std::uint32_t>> atlas_bands;
          atlas_bands.clear();
          // [finding 107] Retuned. 8 -> 3 was right when a rebuild carried a ~24 ms fixed cost
          // (finding 73); that cost is gone -- the concat, the inline tileset decode, the doodad
          // and WMO geometry rebuild, and setDoodads' buffer recreate have all been removed since.
          // With the per-rebuild floor now ~0.3 ms, spreading further is cheap: 2 beats 3 on lost
          // with no overlap across three runs each (124/125/132 vs 133/136/138 ms). 1 lowers lost
          // again but its p99 is worse and one run in three blew out to 174.
          static constexpr int kPackBudget = 2;   // retuned after the concat was removed (73): 8 -> 3 is
                                                  // now better (max 45-52 -> 34-36 ms), because a rebuild no
                                                  // longer carries a ~24 ms fixed cost. 2 is worse again.   // 8: a normal crossing (1-2 new tiles) finishes in ONE
                                                  // frame, so no extra rebuild events; only a cold start
                                                  // (~45 tiles) spreads, over ~6 frames instead of one 587 ms stall.
          int packed_this_frame = 0;
          bool deferred_any = false;
          static std::vector<MapTile*> deferred_tiles;
          deferred_tiles.clear();
          // [finding 83] TILESET PREFETCH, two halves.
          //
          // NOW: resolve every tileset the tiles being packed this frame need. Names the
          // background decoder already finished cost only the VK texture creation; the rest are
          // decoded here, on the render pool, exactly as before.
          //
          // AHEAD: queue the tilesets of every tile in the neighbourhood that has NOT been packed
          // yet. Those tiles are already deferred by the pack budget, so there are frames of slack
          // before they are needed -- which is the only place this work can go, since the archive
          // read serialises and more threads on the same frame do not help.
          {
            // A tile's tileset list is fixed until the tile is edited, but the first version of
            // this walked all 256 chunks x 4 layers of every not-yet-packed tile on EVERY rebuild,
            // building ~43k std::strings and linear-searching a vector for each. That cost as much
            // as the decode it was saving: texture-free crossings went 2.0 -> 4.5 ms while
            // texture-heavy ones went 10.6 -> 5.3, for no net change. Cache the list per tile.
            static std::map<MapTile*, std::vector<std::string>> s_tile_tex_names;
            auto const names_of = [&](MapTile* t) -> std::vector<std::string> const&
            {
              auto it = s_tile_tex_names.find(t);
              if (it != s_tile_tex_names.end() && !t->changed.load())
                return it->second;
              std::vector<std::string> names;
              for (unsigned cz = 0; cz < 16; ++cz)
                for (unsigned cx = 0; cx < 16; ++cx)
                {
                  MapChunk* ch = t->getChunk(cx, cz);
                  TextureSet* ts = ch ? ch->getTextureSet() : nullptr;
                  if (!ts) continue;
                  auto* texs = ts->getTextures();
                  int const n = std::min(4, static_cast<int>(ts->num()));
                  for (int k = 0; k < n; ++k)
                  {
                    std::string nm = (*texs)[k]->file_key().filepath();
                    if (std::find(names.begin(), names.end(), nm) == names.end())
                      names.push_back(std::move(nm));
                  }
                }
              return s_tile_tex_names[t] = std::move(names);
            };
            // drop entries for tiles that have left the neighbourhood
            if (s_tile_tex_names.size() > 256u)
            {
              std::set<MapTile*> live(vk_tiles.begin(), vk_tiles.end());
              for (auto it = s_tile_tex_names.begin(); it != s_tile_tex_names.end(); )
                it = live.count(it->first) ? std::next(it) : s_tile_tex_names.erase(it);
            }
            static std::unordered_set<std::string> seen_here;
            auto const collect = [&](std::vector<MapTile*> const& tiles, std::vector<std::string>& into)
            {
              seen_here.clear();
              for (MapTile* t : tiles)
                for (std::string const& nm : names_of(t))
                {
                  if (s_vk_tex_ids.count(nm)) continue;
                  if (seen_here.insert(nm).second)
                    into.push_back(nm);
                }
            };

            // --- which tiles will this frame pack? (repeats the main loop residency + budget
            // decision; a disagreement only costs an inline decode, never correctness)
            static std::vector<MapTile*> to_pack, not_packed;
            to_pack.clear(); not_packed.clear();
            {
              int budget_probe = 0;
              static bool const s_no_skip_probe = std::getenv("NOGGIT_VK_NO_GEOM_SKIP") != nullptr;
              for (MapTile* t : vk_tiles)
              {
                std::uint32_t const ts_slot = (static_cast<std::uint32_t>(t->index.x) & 7u) * 8u
                                            + (static_cast<std::uint32_t>(t->index.z) & 7u);
                if (!s_no_skip_probe && slot_owned_by(ts_slot, t) && !t->changed.load())
                  continue;
                // only a few tiles ahead: the decoder queue is bounded and the remainder are
                // picked up on the following frames, so scanning all 42 buys nothing
                if (budget_probe >= kPackBudget)
                {
                  if (not_packed.size() < 8u) not_packed.push_back(t);
                  continue;
                }
                ++budget_probe;
                to_pack.push_back(t);
              }
            }

            // --- NOW half
            static std::vector<std::string> want;
            want.clear();
            collect(to_pack, want);
            if (!want.empty())
            {
              std::vector<VkPfDecoded> out(want.size());
              static std::vector<std::size_t> missing;
              missing.clear();
              {
                std::lock_guard<std::mutex> lk(s_pf_mtx);
                for (std::size_t k = 0; k < want.size(); ++k)
                {
                  auto it = s_pf_ready.find(want[k]);
                  if (it == s_pf_ready.end()) { missing.push_back(k); continue; }
                  out[k] = std::move(it->second);
                  s_pf_ready.erase(it);
                  ++_rb_pf_hit;
                }
              }
              auto const _pf0 = std::chrono::steady_clock::now();
              if (!missing.empty())
              {
                auto const decode_one = [&](std::size_t mi)
                {
                  std::size_t const k = missing[mi];
                  out[k] = vkDecodeTileset(want[k], _context);
                };
                if (auto* tp = noggit::render_pool(); tp && missing.size() > 1)
                  tp->parallel_for(missing.size(), decode_one);
                else
                  for (std::size_t mi = 0; mi < missing.size(); ++mi) decode_one(mi);
                _rb_pf_miss = static_cast<int>(missing.size());
              }
              _rb_pf_decode = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - _pf0).count();
              // texture CREATION stays on this thread -- the only backend call involved
              auto const _pu0 = std::chrono::steady_clock::now();
              for (std::size_t k = 0; k < want.size(); ++k)
              {
                if (!out[k].ok) continue;   // leave it unresolved; the pack loop retries inline
                std::int32_t const id = out[k].compressed
                  ? s_vk.backend.addTextureCompressed(out[k].vf, out[k].w, out[k].h, out[k].mips)
                  : s_vk.backend.addTextureMips(out[k].mips, out[k].w, out[k].h);
                s_vk_tex_ids.emplace(want[k], id);
              }
              {
                std::lock_guard<std::mutex> lk(s_pf_mtx);
                for (auto const& nm : want) s_pf_seen.erase(nm);
              }
              _rb_pf_upload = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - _pu0).count();
              _rb_pf_n = static_cast<int>(want.size());
            }

            // --- AHEAD half: hand the not-yet-packed tiles to the background decoder
            if (!not_packed.empty())
            {
              static std::vector<std::string> ahead;
              ahead.clear();
              collect(not_packed, ahead);
              if (!ahead.empty())
              {
                std::lock_guard<std::mutex> lk(s_pf_mtx);
                if (!s_pf_thread.joinable())
                  s_pf_thread = std::thread(vkTilesetDecodeLoop, _context);
                for (auto& nm : ahead)
                {
                  if (s_pf_queue.size() >= 128u) break;   // bounded; the rest come next frame
                  if (!s_pf_seen.insert(nm).second) continue;
                  s_pf_queue.push_back(nm);
                }
                s_pf_cv.notify_all();
              }
            }
          }
          for (MapTile* t : vk_tiles)
          {
            if (!mesh_ok) break;
            std::uint32_t const tile_slot = (static_cast<std::uint32_t>(t->index.x) & 7u) * 8u
                                          + (static_cast<std::uint32_t>(t->index.z) & 7u);
            // already packed into this slot and not edited since -> its atlas cells and chunk
            // records are still correct; skip every byte of that work
            // NOGGIT_VK_NO_GEOM_SKIP=1: keep the slot ADDRESSING but repack every tile, to separate
            // "the slot layout is wrong" from "the residency skip is wrong".
            static bool const s_no_geom_skip = std::getenv("NOGGIT_VK_NO_GEOM_SKIP") != nullptr;
            bool tile_resident = !s_no_geom_skip
                              && slot_owned_by(tile_slot, t) && !t->changed.load();
            if (!tile_resident)
            {
              if (packed_this_frame >= kPackBudget)
              {
                // over budget: leave this tile for a later frame. It is NOT drawn meanwhile -- its
                // slot may still hold a previous owner's data, so emitting bounds would draw the
                // wrong terrain.
                deferred_any = true;
                deferred_tiles.push_back(t);
                continue;
              }
              ++packed_this_frame;
              std::uint32_t const r0 = (tile_slot * 256u) / kAtlasPerRow;
              std::uint32_t const r1 = (tile_slot * 256u + 255u) / kAtlasPerRow;
              atlas_bands.emplace_back(r0, r1);
            }
            s_slot_owner[tile_slot] = t;
            s_slot_ix[tile_slot] = t->index.x;   // finding 117
            s_slot_iz[tile_slot] = t->index.z;
            if (!tile_resident)
            {
              // this tile's 256 chunk slots are contiguous, so each stream is ONE range
              std::size_t const c0 = std::size_t(tile_slot) * 256u;
              dirty_v.emplace_back(c0 * kVertsPerChunk * 9u * sizeof(float),
                                   std::size_t(256u) * kVertsPerChunk * 9u * sizeof(float));
              dirty_i.emplace_back(c0 * kIdxPerChunk * sizeof(std::uint32_t),
                                   std::size_t(256u) * kIdxPerChunk * sizeof(std::uint32_t));
              dirty_c.emplace_back(c0 * kVertsPerChunk * sizeof(std::uint32_t),
                                   std::size_t(256u) * kVertsPerChunk * sizeof(std::uint32_t));
            }
            for (unsigned cz = 0; cz < 16 && mesh_ok; ++cz)
            {
              for (unsigned cx = 0; cx < 16 && mesh_ok; ++cx)
              {
                MapChunk* ch = t->getChunk(cx, cz);
                glm::vec3 const* hm = ch ? ch->getHeightmap() : nullptr;
                // NORMALS: MapChunk::mNormals is a dead array (declared, never filled) -- GL lights from
                // the tile heightmap buffer (rgb = MCNR normal in the shader's frame, a = height). Use
                // exactly that so VK's N.L is GL's N.L by construction.
                float const* hb = ch ? t->getChunkHeightmapBuffer().data() + (ch->px * 16 + ch->py) * 145 * 4 : nullptr;
                void const* nm = hb; // presence check only; components read via hb[i * 4 + c]
                glm::vec3 const* vc = ch ? ch->getVertexColors() : nullptr;
                if (!hm || !nm || !vc) { mesh_ok = false; break; }
                std::uint32_t const base = (tile_slot * 256u + (cz * 16u + cx)) * 145u;
                // the SLOT, not a running counter -- this is the index the shader uses for both the
                // chunk SSBO and the atlas cell, so it must stay put for a tile that did not move
                std::uint32_t const chunk_id = tile_slot * 256u + (cz * 16u + cx);
                if (!tile_resident)
                {
                  std::size_t const vbase = std::size_t(chunk_id) * kVertsPerChunk * 9u;
                  std::size_t const cbase = std::size_t(chunk_id) * kVertsPerChunk;
                  for (unsigned i = 0; i < 145; ++i)
                  {
                    float* v = &vk_verts[vbase + std::size_t(i) * 9u];
                    v[0] = hm[i].x; v[1] = hm[i].y; v[2] = hm[i].z;
                    v[3] = hb[i * 4 + 0];   // normal from the tile heightmap buffer (RGBA stride)
                    v[4] = hb[i * 4 + 1];
                    v[5] = hb[i * 4 + 2];
                    v[6] = vc[i].x;         // MCCV painted colour (interleaved, stride 36)
                    v[7] = vc[i].y;
                    v[8] = vc[i].z;
                    vk_cidx[cbase + i] = chunk_id;
                  }
                }
                // per-chunk data for the textured path -- skipped entirely for a tile that is
                // already packed into this slot: no tileset lookup, no alphamap read, no atlas write
                if (!tile_resident)
                {
                  Noggit::Rendering::VK::VulkanBackend::TerrainChunk cd{};
                  TextureSet* ts = ch->getTextureSet();
                  int const n = ts ? static_cast<int>(ts->num()) : 0;
                  cd.layer_count = std::min(4, n);
                  cd.holes = static_cast<std::int32_t>(ch->getHoleMask());
                  cd.origin_x = hm[0].x;
                  cd.origin_z = hm[0].z;
                  for (int k = 0; k < 4; ++k)
                  {
                    cd.tex[k] = -1;
                    cd.anim[k] = 0;
                  }
                  if (ts)
                  {
                    auto* texs = ts->getTextures();
                    for (int k = 0; k < cd.layer_count; ++k)
                    {
                      std::string const name = (*texs)[k]->file_key().filepath();
                      auto it = s_vk_tex_ids.find(name);
                      if (it == s_vk_tex_ids.end())
                      {
                        auto const _tx0 = std::chrono::steady_clock::now();
                        ++_rb_tex_n;
                        // Decode the BLP ourselves (a private blp_texture instance -- the shared GL one drops
                        // its CPU mips after upload). DXT stays compressed (BC1/2/3 on the VK side).
                        std::int32_t id = -1;
                        try
                        {
                          blp_texture tex(BlizzardArchive::Listfile::FileKey(name), _context);
                          tex.finishLoading();
                          std::uint32_t const w = static_cast<std::uint32_t>(tex.width()), h = static_cast<std::uint32_t>(tex.height());
                          if (w && h)
                          {
                            if (tex.compression_format())
                            {
                              GLint const cf = *tex.compression_format();
                              VkFormat const vf = (cf == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) ? VK_FORMAT_BC2_UNORM_BLOCK
                                                : (cf == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) ? VK_FORMAT_BC3_UNORM_BLOCK
                                                : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                              std::vector<std::vector<std::uint8_t>> mips;
                              for (auto const& [lvl, bytes] : tex.compressed_data()) { (void)lvl; mips.push_back(bytes); }
                              id = s_vk.backend.addTextureCompressed(vf, w, h, mips);
                            }
                            else if (!tex.data().empty())
                            {
                              // the BLP's own mip chain (level order), exactly what GL uploads
                              std::vector<std::vector<std::uint8_t>> mips;
                              for (auto const& [lvl, px] : tex.data())
                              {
                                (void)lvl;
                                auto const* p8 = reinterpret_cast<std::uint8_t const*>(px.data());
                                mips.emplace_back(p8, p8 + px.size() * 4u);
                              }
                              id = s_vk.backend.addTextureMips(mips, w, h);
                            }
                          }
                        }
                        catch (std::exception const& e)
                        {
                          LogError << "[VK] tileset decode failed for " << name << ": " << e.what() << std::endl;
                        }
                        if (id < 0)
                        {
                          static int s_fail_log = 0;
                          if (s_fail_log++ < 8) LogError << "[VK] tileset NOT loaded: " << name << std::endl;
                        }
                        it = s_vk_tex_ids.emplace(name, id).first;
                        _rb_tex += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - _tx0).count();
                      }
                      cd.tex[k] = it->second;
                      unsigned layer_flags = ts->flag(static_cast<std::size_t>(k));
                      auto const* fv = reinterpret_cast<MCLYFlags const*>(&layer_flags);
                      cd.anim[k] = (fv->animation_enabled ? 1 : 0) | (fv->overbright ? 2 : 0)
                                 | (static_cast<int>(fv->animation_speed) << 8) | (static_cast<int>(fv->animation_rotation) << 16);
                    }
                  }
                  vk_chunks[chunk_id] = cd;
                  // write this chunk's 64x64 cell directly into the ATLAS at its slot position, so
                  // the backend has nothing left to repack
                  std::uint32_t const arow = chunk_id / kAtlasPerRow;
                  std::uint32_t const ax = (chunk_id % kAtlasPerRow) * 64u;
                  std::uint32_t const ay = arow * 64u;
                  for (std::uint32_t y = 0; y < 64u; ++y)
                    std::memset(&vk_alpha[((static_cast<std::size_t>(ay) + y) * kAtlasW + ax) * 4u], 0, 64u * 4u);
                  if (ts)
                  {
                    auto* amaps = ts->getAlphamaps();
                    for (int k = 0; k < 3 && k + 1 < cd.layer_count; ++k)
                    {
                      if (!(*amaps)[k]) continue;
                      unsigned char const* a = (*amaps)[k]->getAlpha();
                      for (std::uint32_t y = 0; y < 64u; ++y)
                      {
                        std::uint8_t* dst = &vk_alpha[((static_cast<std::size_t>(ay) + y) * kAtlasW + ax) * 4u];
                        unsigned char const* src = a + y * 64u;
                        for (std::uint32_t x = 0; x < 64u; ++x) dst[x * 4u + k] = src[x];
                      }
                    }
                  }
                  std::uint8_t const* sm = ch->vkShadowMapData();
                  for (std::uint32_t y = 0; y < 64u; ++y)
                    std::memcpy(&vk_shadow[(static_cast<std::size_t>(ay) + y) * kAtlasW + ax], sm + y * 64u, 64u);
                  dirty_row_lo = std::min(dirty_row_lo, arow);
                  dirty_row_hi = std::max(dirty_row_hi, arow);
                }
                // Indices occupy the chunk's OWN fixed span; a hole simply leaves its 12 entries at
                // zero (a degenerate triangle the GPU discards), so the span is a constant stride and
                // the chunk keeps its place across crossings.
                std::uint32_t const chunk_first_index = chunk_id * kIdxPerChunk;
                std::uint32_t emitted_idx = 0;
                if (!tile_resident)
                {
                  std::uint32_t w = chunk_first_index;
                  for (unsigned r = 0; r < 8; ++r)
                  {
                    for (unsigned c = 0; c < 8; ++c)
                    {
                      // MCNK holes: 4x4 cells over the chunk, bit (row/2)*4 + (col/2); skip the 4 tris
                      if (tt_path && (ch->getHoleMask() & (1u << (((r >> 1) * 4u) + (c >> 1)))) != 0u) continue;
                      std::uint32_t const o00 = base + 17u * r + c;
                      std::uint32_t const o01 = o00 + 1u;
                      std::uint32_t const o10 = base + 17u * (r + 1u) + c;
                      std::uint32_t const o11 = o10 + 1u;
                      std::uint32_t const ctr = base + 17u * r + 9u + c;
                      std::uint32_t const quad[12] = { o00, o01, ctr, o01, o11, ctr, o11, o10, ctr, o10, o00, ctr };
                      std::memcpy(&vk_idx[w], quad, sizeof(quad));
                      w += 12u;
                    }
                  }
                  emitted_idx = w - chunk_first_index;
                  // zero the tail so a chunk that shrank does not draw last owner's triangles
                  if (emitted_idx < kIdxPerChunk)
                    std::memset(&vk_idx[w], 0, (kIdxPerChunk - emitted_idx) * sizeof(std::uint32_t));
                  s_slot_idx_count[chunk_id] = emitted_idx;
                }
                else
                {
                  emitted_idx = s_slot_idx_count[chunk_id];
                }
                // Chunk bounds for the per-frame frustum test. A chunk is CHUNKSIZE across; the
                // radius covers its diagonal plus generous vertical slack for the heightmap.
                // [finding 108] bounds are emitted by the parallel pass below, not here
              }
            }
            tile_base += 256u * 145u;
          }

          // [finding 108] BOUNDS PASS. Read-only per chunk, and each tile owns its own output
          // vector, so this runs on the render pool; the concatenation afterwards restores exact
          // tile order. This is the part of `geom` that runs for the whole neighbourhood every
          // rebuild (~45 tiles x 256 chunks) regardless of how few tiles are actually packed.
          {
            static std::vector<std::vector<VkChunkBounds>> per_tile_bounds;
            if (per_tile_bounds.size() < vk_tiles.size())
              per_tile_bounds.resize(vk_tiles.size());
            auto const bounds_one = [&](std::size_t ti)
            {
              MapTile* const t = vk_tiles[ti];
              auto& out = per_tile_bounds[ti];
              out.clear();
              std::uint32_t const tile_slot = (static_cast<std::uint32_t>(t->index.x) & 7u) * 8u
                                            + (static_cast<std::uint32_t>(t->index.z) & 7u);
              for (unsigned cz = 0; cz < 16; ++cz)
                for (unsigned cx = 0; cx < 16; ++cx)
                {
                  MapChunk* ch = t->getChunk(cx, cz);
                  if (!ch) continue;
                  std::uint32_t const chunk_id = tile_slot * 256u + (cz * 16u + cx);
                  std::uint32_t const emitted = s_slot_idx_count[chunk_id];
                  if (!emitted) continue;
                  VkChunkBounds b;
                  b.first_index = chunk_id * kIdxPerChunk;
                  b.index_count = emitted;
                  b.centre = glm::vec3(ch->xbase + CHUNKSIZE * 0.5f,
                                       ch->vmin.y + (ch->vmax.y - ch->vmin.y) * 0.5f,
                                       ch->zbase + CHUNKSIZE * 0.5f);
                  float const half_h = std::max(1.f, (ch->vmax.y - ch->vmin.y) * 0.5f);
                  b.radius = std::sqrt(2.f * (CHUNKSIZE * 0.5f) * (CHUNKSIZE * 0.5f)
                                       + half_h * half_h);
                  out.push_back(b);
                }
            };
            if (auto* tp = noggit::render_pool(); tp && vk_tiles.size() > 1)
              tp->parallel_for(vk_tiles.size(), bounds_one, 2);
            else
              for (std::size_t ti = 0; ti < vk_tiles.size(); ++ti) bounds_one(ti);
            for (std::size_t ti = 0; ti < vk_tiles.size(); ++ti)
              s_vk_chunk_bounds.insert(s_vk_chunk_bounds.end(),
                                       per_tile_bounds[ti].begin(), per_tile_bounds[ti].end());
            ++vk_bounds_generation();   // finding 114
          }
          {
            // one-shot dump of what the slot layout actually produced -- reasoning about the
            // addressing has not found the black frame, so print the real numbers
            static int s_slot_dbg = 0;
            if (s_slot_dbg++ < 2)
            {
              std::size_t nz_v = 0;
              for (std::size_t k = 0; k < vk_verts.size() && nz_v < 1; k += 9)
                if (vk_verts[k] != 0.f || vk_verts[k + 1] != 0.f) ++nz_v;
              std::uint32_t nz_i = 0;
              for (std::size_t k = 0; k < vk_idx.size() && nz_i < 1; ++k)
                if (vk_idx[k]) ++nz_i;
              LogError << "[VK-SLOTDBG] verts=" << (vk_verts.size() / 9)
                       << " idx=" << vk_idx.size()
                       << " chunks=" << vk_chunks.size()
                       << " cidx=" << vk_cidx.size()
                       << " bounds=" << s_vk_chunk_bounds.size()
                       << " anyNonZeroVert=" << nz_v << " anyNonZeroIdx=" << nz_i << std::endl;
              if (!s_vk_chunk_bounds.empty())
              {
                auto const& b0 = s_vk_chunk_bounds.front();
                LogError << "[VK-SLOTDBG] bounds[0] first_index=" << b0.first_index
                         << " index_count=" << b0.index_count
                         << " centre=(" << b0.centre.x << "," << b0.centre.y << "," << b0.centre.z
                         << ") r=" << b0.radius
                         << " | idx@first=" << (b0.first_index < vk_idx.size() ? vk_idx[b0.first_index] : 9999999u)
                         << std::endl;
              }
            }
          }
          // [overnight stage 3] WATER: real liquid heights (liquid_layer 9x9 verts, 8x8 cell mask) from the
          // same neighbourhood, emitted as translucent quads (per-cell, up-normals).
          // [SPIKE FIX] same churn as the terrain streams: allocated and freed on every rebuild
          _rb_geom = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - _rb_t0).count();
          _rb_t0 = std::chrono::steady_clock::now();
          static std::vector<float> vk_wverts;
          static std::vector<std::uint32_t> vk_widx;
          vk_wverts.clear();
          vk_widx.clear();
          // [SPIKE FIX] Exactly the same leak as s_vk_chunk_bounds: this static was only ever
          // appended to, so every rebuild stacked another neighbourhood of water bounds on the last
          // (20305 -> 23742 -> 49091 entries, visible 1105 -> 8927). The per-frame cull then walked
          // the whole thing and the draw issued thousands of STALE water chunks -- blended geometry,
          // so it is expensive fill -- and it got worse the longer you flew. Every tile re-pushes its
          // bounds below (from the cached pack or freshly built), so this describes the CURRENT
          // neighbourhood only.
          s_vk_water_bounds.clear();
          // [SPIKE FIX] ADT water is TILE-bound (see the comment on setWaterMesh), yet the whole
          // neighbourhood's water was re-derived on every rebuild -- 54 ms of the ~167 ms hitch,
          // a third of it, to pick up the ~4% of tiles that actually changed. Cache each tile's
          // packed block and concatenate; only new tiles are walked.
          //
          // A tile with unsaved edits (`changed`) ALWAYS repacks, so editing water is never served
          // stale data -- and that repacks one tile, not forty-six. The stored block is rebased to
          // vertex 0 / index 0 so it can be concatenated at any offset.
          struct WaterPack
          {
            std::vector<float> wverts;
            std::vector<std::uint32_t> widx;
            std::vector<VkChunkBounds> bounds;
          };
          static std::map<MapTile*, WaterPack> s_water_packs;
          // [SPIKE FIX] Each tile owns a PERMANENT span of vk_wverts/vk_widx, handed out by a
          // free-list allocator and kept while the tile is resident. Deriving writes straight into
          // that span, so a crossing copies only the new tiles instead of re-concatenating the whole
          // ~70 MB neighbourhood (finding 71).
          // tile_index stamps the span with stable identity: a MapTile* can be freed and a
          // DIFFERENT tile allocated at the same address, and slots repeat across neighbourhoods
          // (x & 7), so the pointer alone is not enough to prove the span still belongs (116).
          struct WaterSpan { std::size_t voff = 0, vlen = 0, ioff = 0, ilen = 0;
                             std::vector<VkChunkBounds> bounds;
                             int tile_x = -1, tile_z = -1; };
          static std::map<MapTile*, WaterSpan> s_water_spans;
          struct FreeBlock { std::size_t off, len; };
          static std::vector<FreeBlock> s_free_v, s_free_i;
          static std::vector<std::pair<std::size_t, std::size_t>> wdirty_v, wdirty_i;
          wdirty_v.clear(); wdirty_i.clear();
          // [SPIKE FIX] These are cleared every rebuild and were then grown ONE ELEMENT AT A TIME
          // over millions of water indices with no reserve -- the same pattern that cost 385 ms in
          // the alpha arrays. With the terrain streams now slot-addressed (3.4 ms/crossing) this
          // concatenation was the whole remaining hitch at ~35 ms. Reserve from the previous
          // rebuild's totals so the fill is a straight write.
          // high-water marks from previous rebuilds; capacity is kept across rebuilds by the statics,
          // so after the first crossing these reserves are already satisfied and cost nothing
          double _w_concat = 0.0, _w_derive = 0.0;   // cached-pack concat vs walking a NEW tile
          // finding 80: split the derive into the PARALLEL walk and the SERIAL span assignment
          // (memcpy + per-index rebase + any vector growth), to see which one the crossings pay.
          double _w_par = 0.0, _w_asg = 0.0;
          double _w_alloc = 0.0, _w_fill = 0.0;
          double _w_pre = 0.0, _w_tail = 0.0;
          double _w_pre_res = 0.0, _w_pre_scan = 0.0, _w_pre_liq = 0.0;
          auto const _w_sec0 = std::chrono::steady_clock::now();
          std::atomic<double> _w_tile_max{ 0.0 };
          std::atomic<double> _w_tile_sum{ 0.0 };
          // finding 80: _rb_water spans the DOODAD and WMO gathers too, and derive+concat only
          // account for part of it -- there is a ~6 ms floor on every rebuild that is neither.
          double _rb_dgather = 0.0, _rb_demit = 0.0, _rb_wmoemit = 0.0;
          std::size_t _w_grow_v = 0, _w_grow_i = 0;   // elements added by growing (not free-list reuse)
          static std::size_t s_wv_max = 0, s_wi_max = 0;
          auto _wpr0 = std::chrono::steady_clock::now();
          if (s_wv_max) vk_wverts.reserve(s_wv_max);
          if (s_wi_max) vk_widx.reserve(s_wi_max);
          _w_pre_res = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - _wpr0).count();
          // [SPIKE FIX] PHASE 1: make sure every tile has a cached pack. Only genuinely new/edited
          // tiles are walked here; everything else is already packed from a previous frame.
          // Tiles that need packing are derived INDEPENDENTLY (each into its own buffers), so the
          // work runs on the render pool. Deriving one tile's water costs ~10 ms; with a pack budget
          // of 8 that was up to ~80 ms serial in a single frame -- the 412 ms rebuild still showing up.
          auto _wps0 = std::chrono::steady_clock::now();
          static std::vector<MapTile*> to_derive;
          to_derive.clear();
          for (MapTile* t : vk_tiles)
          {
            auto const cached = s_water_packs.find(t);
            if (cached != s_water_packs.end() && !t->changed)
              continue;
            // [SPIKE FIX] Honour the pack budget here too. Without this the cold start packed 8
            // tiles' geometry but still DERIVED ALL 45 tiles' water ("derived=45" against
            // "packed=8/45"), which is most of what was left of the 345 ms rebuild.
            if (std::find(deferred_tiles.begin(), deferred_tiles.end(), t) != deferred_tiles.end())
              continue;
            to_derive.push_back(t);
          }
          _w_pre_scan = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - _wps0).count();
          auto _wpl0 = std::chrono::steady_clock::now();
          static std::vector<WaterPack> derived;
          // grow only: the packs are cleared by the derive itself, so their buffers survive and a
          // crossing reuses the previous one's allocation instead of freeing and re-faulting it
          if (derived.size() < to_derive.size())
            derived.resize(to_derive.size());
          // [finding 88] The derive resolved the animated liquid texture PER SUB-CHUNK: two map
          // lookups, a std::to_string and two string concatenations (three heap allocations), and
          // a hashed lookup, up to 256 chunks x 4 layers x 64 sub-chunks deep. All of it depends
          // only on the layer's liquidID and on animtime, which is fixed for the frame.
          //
          // It was also a DATA RACE: vkResolveBlp inserts into s_vk_tex_ids and can call the VK
          // backend, and derive_one runs on pool workers. It only survived because the textures
          // are normally already cached by the time several tiles derive at once.
          //
          // Resolve once per liquid id, here, on the render thread. The values are identical.
          static std::unordered_map<unsigned, std::pair<float, glm::vec2>> s_liq_tex;
          s_liq_tex.clear();
          {
            auto const& names  = _world->renderer()->liquidTextureManager().vkTextureNames();
            auto const& frames = _world->renderer()->liquidTextureManager().getTextureFrames();
            auto const entry = [&](unsigned lid)
            {
            if (s_liq_tex.count(lid)) return;
            float tex = -1.f;
            glm::vec2 anim(1.f, 1.f);
            auto const it = names.find(lid);
            if (it != names.end() && it->second.second > 0)
            {
              unsigned const n_frames = it->second.second;
              unsigned const frame =
                static_cast<unsigned>(std::ceil(_world->animtime / 60.0)) % n_frames;
              tex = static_cast<float>(
                vkResolveBlp(it->second.first + std::to_string(frame + 1u) + ".blp"));
            }
            auto const fit = frames.find(lid);
            if (fit != frames.end())
              anim = std::get<1>(fit->second);
            s_liq_tex.emplace(lid, std::make_pair(tex, anim));
            };
            for (auto const& kv : names)  entry(kv.first);
            for (auto const& kv : frames) entry(kv.first);

            // [finding 90] Hoisting the resolve out of the derive (88) did not remove its cost, it
            // MOVED it: the table costs 2.6-4.5 ms whenever the animation clock reaches a frame
            // whose BLP has not been decoded yet, because each frame index is a different file.
            // Same inline archive read as findings 83 and 85, in a third place.
            //
            // Queue EVERY frame of every liquid to the background decoder, once. Deferring the
            // frame instead would change which animation frame is drawn; prefetching does not --
            // the resolve below still returns the exact frame the clock asks for, it just finds it
            // already decoded. The set is small (a few liquid types x ~30 frames).
            static bool s_liq_prefetched = false;
            if (!s_liq_prefetched && !names.empty())
            {
              s_liq_prefetched = true;
              std::lock_guard<std::mutex> lk(s_pf_mtx);
              if (!s_pf_thread.joinable())
                s_pf_thread = std::thread(vkTilesetDecodeLoop, _context);
              bool queued = false;
              for (auto const& kv : names)
              {
                for (unsigned f = 1; f <= kv.second.second; ++f)
                {
                  std::string nm = kv.second.first + std::to_string(f) + ".blp";
                  if (s_vk_tex_ids.count(nm)) continue;
                  if (!s_pf_seen.insert(nm).second) continue;
                  s_pf_queue.push_back(std::move(nm));
                  queued = true;
                }
              }
              if (queued) s_pf_cv.notify_all();
            }
          }
          _w_pre_liq = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - _wpl0).count();
          {
            auto const _wd0 = std::chrono::steady_clock::now();
            _w_pre = std::chrono::duration<double, std::milli>(_wd0 - _w_sec0).count();
            auto const derive_one = [&](std::size_t _k)
            {
            auto const _dt0 = std::chrono::steady_clock::now();
            MapTile* const t = to_derive[_k];
            // [finding 89] These used to be local vectors that were COPIED wholesale into the
            // output WaterPack at the end of the derive ("rebased to 0" -- but wv0/wi0/wb0 were
            // literal zeros, so the rebase subtracted nothing and the block was a pure copy of
            // megabytes per tile). Derive straight into the output instead.
            //
            // It also means the buffers KEEP THEIR CAPACITY between rebuilds. They used to be
            // destroyed by derived.clear() at the start of the next rebuild, and freeing several
            // MB back to the OS (then page-faulting them in again) cost up to 4.5 ms in the
            // pre-derive stretch -- on crossings that derived almost no water at all.
            std::vector<float>& vk_wverts = derived[_k].wverts;
            std::vector<std::uint32_t>& vk_widx = derived[_k].widx;
            std::vector<VkChunkBounds>& s_vk_water_bounds = derived[_k].bounds;
            vk_wverts.clear(); vk_widx.clear(); s_vk_water_bounds.clear();
            for (int wz = 0; wz < 16; ++wz)
            {
              for (int wx = 0; wx < 16; ++wx)
              {
                ChunkWater* cw = t->Water.getChunk(wx, wz);
                if (!cw)
                  continue;
                // Cull water per MAP CHUNK, not per 8x8 sub-chunk: the sub-chunks of one chunk are
                // emitted contiguously, so one range covers them. Per-sub-chunk meant walking 256,335
                // bounds and building a 24k-entry indirect list EVERY frame -- trading GPU time for
                // CPU time. Per chunk is 11,515 bounds for the same GPU saving.
                std::uint32_t const chunk_water_first = static_cast<std::uint32_t>(vk_widx.size());
                glm::vec3 chunk_water_lo(1e30f), chunk_water_hi(-1e30f);
                for (liquid_layer& lay : *cw->getLayers())
                {
                  if (lay.empty())
                    continue;
                  auto& lv = lay.getVertices(); // 9x9 world-space liquid surface
                  for (int cz = 0; cz < 8; ++cz)
                  {
                    for (int cx = 0; cx < 8; ++cx)
                    {
                      if (!lay.hasSubchunk(cx, cz))
                        continue;
                      std::uint32_t const vbase = static_cast<std::uint32_t>(vk_wverts.size() / 17u);
                      std::uint32_t const wfirst = static_cast<std::uint32_t>(vk_widx.size());
                      int const corner[4] = { cz * 9 + cx, cz * 9 + cx + 1, (cz + 1) * 9 + cx, (cz + 1) * 9 + cx + 1 };
                      auto& ldepth = lay.getDepth();
                      auto& ltex = lay.getTexCoords();
                      // Animated liquid texture: same frame GL picks (ceil(animtime/60) % n_frames),
                      // resolved by FILE to a bindless id. anim_uv comes from the manager entry.
                      float vk_liquid_tex = -1.f;
                      glm::vec2 vk_anim(1.f, 1.f);
                      {
                        // finding 88: resolved once per liquid id on the render thread, above
                        auto const lt = s_liq_tex.find(static_cast<unsigned>(lay.liquidID()));
                        if (lt != s_liq_tex.end())
                        { vk_liquid_tex = lt->second.first; vk_anim = lt->second.second; }
                      }
                      // The shader branches on the 0..3 CATEGORY (0 water, 1 ocean, 2 magma, 3 slime),
                      // not the DBC liquid id.
                      int const liquid_cat = lay.mclq_liquid_type();
                      float const ltype = static_cast<float>(liquid_cat);
                      // Same world-unit depth GL derives in LiquidRender: the authored byte scaled so
                      // 255 saturates this type's colour ramp, raised to the physical terrain thinness
                      // where the heightmap is available (continuous across tiles, unlike the raw byte).
                      // [BISECT 2026-09-01] Reverted to the pre-3f05f266 depth model. That commit
                      // scaled the authored byte by 20x (river) / 83x (ocean) when hasAuthoredDepth()
                      // and fell back otherwise, so neighbouring chunks landed on wildly different
                      // opacities -- dense where a depth byte was authored, near-transparent where
                      // not. On a custom map that reads as patchy water with chunk-shaped holes.
                      float const authored_deep_units = 100.0f;
                      MapChunk* const terrain_chunk = t->getChunk(static_cast<unsigned>(wx), static_cast<unsigned>(wz));
                      glm::vec3 const* const heightmap = terrain_chunk ? terrain_chunk->getHeightmap() : nullptr;
                      for (int k = 0; k < 4; ++k)
                      {
                        int const ci = corner[k];
                        int const vz = ci / 9, vxi = ci % 9;
                        glm::vec3 const& p = lv[ci];
                        float const raw = ci < static_cast<int>(ldepth.size()) ? ldepth[ci] : 1.f;
                        float render_depth = raw * authored_deep_units;
                        if (heightmap)
                        {
                          float const diff = p.y - heightmap[17 * vz + vxi].y;
                          render_depth = std::max(0.f, diff);
                        }
                        glm::vec2 const uv = ci < static_cast<int>(ltex.size()) ? ltex[ci] : glm::vec2(0.f);
                        vk_wverts.push_back(p.x); vk_wverts.push_back(p.y); vk_wverts.push_back(p.z);
                        vk_wverts.push_back(0.f); vk_wverts.push_back(1.f); vk_wverts.push_back(0.f);
                        vk_wverts.push_back(uv.x); vk_wverts.push_back(uv.y);
                        vk_wverts.push_back(render_depth);
                        vk_wverts.push_back(ltype);
                        vk_wverts.push_back(vk_anim.x); vk_wverts.push_back(vk_anim.y);
                        vk_wverts.push_back(vk_liquid_tex);
                        // ADT water takes no WMO-liquid parameters (see water.frag).
                        vk_wverts.push_back(0.f); vk_wverts.push_back(0.f);
                        vk_wverts.push_back(0.f); vk_wverts.push_back(0.f);
                      }
                      std::uint32_t const q[6] = { vbase, vbase + 1u, vbase + 2u, vbase + 1u, vbase + 3u, vbase + 2u };
                      vk_widx.insert(vk_widx.end(), q, q + 6);
                      // Grow this CHUNK's bounds; the range is closed once the chunk is done.
                      (void)wfirst;
                      for (int k = 0; k < 4; ++k)
                      {
                        glm::vec3 const& cp = lv[corner[k]];
                        chunk_water_lo = glm::min(chunk_water_lo, cp);
                        chunk_water_hi = glm::max(chunk_water_hi, cp);
                      }
                    }
                  }
                }
                // one bounds entry per water CHUNK
                {
                  std::uint32_t const emitted =
                    static_cast<std::uint32_t>(vk_widx.size()) - chunk_water_first;
                  if (emitted)
                  {
                    VkChunkBounds b;
                    b.first_index = chunk_water_first;
                    b.index_count = emitted;
                    b.centre = (chunk_water_lo + chunk_water_hi) * 0.5f;
                    b.radius = glm::length(chunk_water_hi - chunk_water_lo) * 0.5f + 1.f;
                    s_vk_water_bounds.push_back(b);
                  }
                }
              }
            }
            // nothing to stash: the derive wrote into derived[_k] directly (finding 89)
            {
              double const _dt = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - _dt0).count();
              double prev = _w_tile_max.load(std::memory_order_relaxed);
              while (_dt > prev && !_w_tile_max.compare_exchange_weak(prev, _dt)) {}
              double sum = _w_tile_sum.load(std::memory_order_relaxed);
              while (!_w_tile_sum.compare_exchange_weak(sum, sum + _dt)) {}
            }
            };
            {
              auto const _wp0 = std::chrono::steady_clock::now();
              if (auto* tp = noggit::render_pool(); tp && to_derive.size() > 1)
                tp->parallel_for(to_derive.size(), derive_one, 2);   // 2 tiles is worth dispatching
              else
                for (std::size_t k = 0; k < to_derive.size(); ++k) derive_one(k);
              _w_par += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - _wp0).count();
            }
            // [finding 110] Release a tile's water only when its SLOT has been taken by another
            // tile -- the same condition that invalidates its geometry. Releasing on "left the
            // neighbourhood" instead meant a tile coming back re-derived its water while keeping
            // its geometry, which is why crossings showed derived=3..5 against packed=2.
            // Bounded by construction: there are only 64 slots, so at most 64 tiles hold one.
            {
              for (auto it = s_water_spans.begin(); it != s_water_spans.end(); )
              {
                MapTile* const t = it->first;
                // [2026-09-01] USE-AFTER-FREE. s_water_spans is keyed by MapTile*, tiles unload
                // constantly while flying, and this loop dereferenced the key (t->index) to decide
                // whether the key was still valid -- i.e. it read a freed MapTile. Two captured crash
                // stacks landed on exactly this line, on the MAIN thread, and it is also a good
                // candidate for water flashing/appearing where it should not: the keep-vs-evict
                // decision is made on garbage, so spans are wrongly retained or wrongly recycled and
                // their vertex/index ranges get handed to other geometry.
                //
                // Finding 117 already established the rule for the geometry slots ("the pointer alone
                // does not prove identity: a freed MapTile can be reused"), and WaterSpan records
                // tile_x/tile_z precisely so this loop never has to touch t. Take the identity from
                // the span and compare only the POINTER, which never loads through it.
                std::uint32_t const slot = (static_cast<std::uint32_t>(it->second.tile_x) & 7u) * 8u
                                         + (static_cast<std::uint32_t>(it->second.tile_z) & 7u);
                bool const same_tile = s_slot_ix[slot] == it->second.tile_x
                                    && s_slot_iz[slot] == it->second.tile_z;
                if (s_slot_owner[slot] == t && same_tile) { ++it; continue; }   // still its slot: keep
                if (it->second.vlen) s_free_v.push_back({ it->second.voff, it->second.vlen });
                if (it->second.ilen) s_free_i.push_back({ it->second.ioff, it->second.ilen });
                s_water_packs.erase(t);
                it = s_water_spans.erase(it);
              }
            }
            // [finding 87] Coalesce first. Releases were appended unmerged, so a tile freeing a
            // span next to another free span left two blocks that individually fit nothing, first
            // fit missed, and the buffer GREW instead -- on 12 of 15 crossings. Growing means
            // vk_wverts.resize() reallocating and copying the whole (tens of MB) buffer, on the
            // render thread, inside the crossing.
            auto const coalesce = [](std::vector<FreeBlock>& f)
            {
              if (f.size() < 2) return;
              std::sort(f.begin(), f.end(),
                        [](FreeBlock const& a, FreeBlock const& b) { return a.off < b.off; });
              std::size_t w = 0;
              for (std::size_t r = 1; r < f.size(); ++r)
              {
                if (f[w].off + f[w].len == f[r].off) f[w].len += f[r].len;
                else f[++w] = f[r];
              }
              f.resize(w + 1);
            };
            coalesce(s_free_v);
            coalesce(s_free_i);
            // [finding 87] RESERVE ONCE, GEOMETRICALLY. alloc_v/alloc_i fall back to
            // vk_wverts.resize(), and std::vector grows by ~1.5x, so a crossing that allocates
            // three spans could reallocate and copy the entire buffer up to three times. Capacity
            // climbed 1.7M -> 2.5M -> 3.8M -> 5.7M floats over one flight and the copy cost 5.5 ms
            // on the render thread (alloc=5.495 against fill=1.40).
            //
            // Project this crossing's worst case (nothing reused from the free list) and reserve it
            // in one step, doubling so this fires a handful of times over a session instead of on
            // every crossing.
            {
              std::size_t need_v = 0, need_i = 0;
              for (std::size_t k = 0; k < to_derive.size(); ++k)
              {
                need_v += derived[k].wverts.size();
                need_i += derived[k].widx.size();
              }
              if (vk_wverts.size() + need_v > vk_wverts.capacity())
                vk_wverts.reserve(std::max(vk_wverts.capacity() * 2u,
                                           vk_wverts.size() + need_v));
              if (vk_widx.size() + need_i > vk_widx.capacity())
                vk_widx.reserve(std::max(vk_widx.capacity() * 2u,
                                         vk_widx.size() + need_i));
            }
            // first-fit allocator; falls back to growing the buffer
            auto alloc_v = [&_w_grow_v](std::size_t n) -> std::size_t
            {
              for (auto it = s_free_v.begin(); it != s_free_v.end(); ++it)
                if (it->len >= n)
                { std::size_t const o = it->off; it->off += n; it->len -= n;
                  if (!it->len) s_free_v.erase(it); return o; }
              _w_grow_v += n;
              std::size_t const o = vk_wverts.size(); vk_wverts.resize(o + n); return o;
            };
            auto alloc_i = [&_w_grow_i](std::size_t n) -> std::size_t
            {
              for (auto it = s_free_i.begin(); it != s_free_i.end(); ++it)
                if (it->len >= n)
                { std::size_t const o = it->off; it->off += n; it->len -= n;
                  if (!it->len) s_free_i.erase(it); return o; }
              _w_grow_i += n;
              std::size_t const o = vk_widx.size(); vk_widx.resize(o + n); return o;
            };
            auto const _wa0 = std::chrono::steady_clock::now();
            // [finding 87] Two passes. ALLOCATION is serial -- it mutates the free list and can grow
            // vk_wverts / vk_widx, which reallocates and would invalidate any pointer a worker held.
            // FILLING is per-tile into a span nothing else touches, so it goes on the pool. This was
            // one serial loop costing up to 5.9 ms of memcpy and index rebasing on the render thread.
            static std::vector<std::size_t> fill_k;
            fill_k.clear();
            for (std::size_t k = 0; k < to_derive.size(); ++k)
            {
              MapTile* const t = to_derive[k];
              WaterPack& wp = derived[k];
              // a repacked tile releases its old span first
              auto const prev = s_water_spans.find(t);
              if (prev != s_water_spans.end())
              {
                if (prev->second.vlen) s_free_v.push_back({ prev->second.voff, prev->second.vlen });
                if (prev->second.ilen) s_free_i.push_back({ prev->second.ioff, prev->second.ilen });
                s_water_spans.erase(prev);
              }
              if (wp.widx.empty()) { s_water_packs[t] = WaterPack{}; continue; }
              WaterSpan sp;
              sp.vlen = wp.wverts.size();
              sp.ilen = wp.widx.size();
              sp.voff = alloc_v(sp.vlen);
              sp.ioff = alloc_i(sp.ilen);
              sp.tile_x = t->index.x;   // finding 116: stable identity for the span
              sp.tile_z = t->index.z;
              sp.bounds.reserve(wp.bounds.size());
              for (VkChunkBounds b : wp.bounds)
              { b.first_index += static_cast<std::uint32_t>(sp.ioff); sp.bounds.push_back(b); }
              wdirty_v.emplace_back(sp.voff * sizeof(float), sp.vlen * sizeof(float));
              wdirty_i.emplace_back(sp.ioff * sizeof(std::uint32_t), sp.ilen * sizeof(std::uint32_t));
              s_water_spans[t] = std::move(sp);
              s_water_packs[t] = WaterPack{};   // presence marker; the bytes now live in the span
              fill_k.push_back(k);
            }
            _w_alloc = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - _wa0).count();
            auto const _wf0 = std::chrono::steady_clock::now();
            // every span is allocated now, so the buffers will not move under the workers
            auto const fill_one = [&](std::size_t fi)
            {
              std::size_t const k = fill_k[fi];
              WaterPack const& wp = derived[k];
              WaterSpan const& sp = s_water_spans[to_derive[k]];
              std::memcpy(vk_wverts.data() + sp.voff, wp.wverts.data(), sp.vlen * sizeof(float));
              std::uint32_t const vbase = static_cast<std::uint32_t>(sp.voff / 17u);
              for (std::size_t q = 0; q < sp.ilen; ++q)
                vk_widx[sp.ioff + q] = wp.widx[q] + vbase;
            };
            if (auto* tp = noggit::render_pool(); tp && fill_k.size() > 1)
              tp->parallel_for(fill_k.size(), fill_one, 2);
            else
              for (std::size_t fi = 0; fi < fill_k.size(); ++fi) fill_one(fi);
            _w_fill = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - _wf0).count();
            _w_asg += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - _wa0).count();
            _w_derive += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - _wd0).count();
          }

          // [SPIKE FIX] PHASE 2: bounds only. The concatenation is GONE -- each tile's water was
          // written straight into its own permanent span of vk_wverts/vk_widx when it was derived
          // (see the allocator in phase 1), so there is nothing left to copy here. This used to be
          // ~24 ms on every rebuild, copying ~70 MB of water to pick up the few tiles that changed,
          // and it was the single largest remaining hitch (finding 71).
          {
            auto const _wc0 = std::chrono::steady_clock::now();
            s_vk_water_bounds.clear();
            for (MapTile* t : vk_tiles)
            {
              auto const it = s_water_spans.find(t);
              if (it == s_water_spans.end()) continue;
              auto const& sp = it->second;
              s_vk_water_bounds.insert(s_vk_water_bounds.end(), sp.bounds.begin(), sp.bounds.end());
            }
            ++vk_bounds_generation();   // finding 114: water bounds rewritten
            _w_concat += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - _wc0).count();
          }
          auto const _w_tail0 = std::chrono::steady_clock::now();

          // [2026-09-03] the clay doodad/WMO geometry build that lived here is REMOVED --
          // setDoodads (its only consumer) is gone, so it rebuilt and cached ~64 MB of
          // clay geometry per crossing for nothing. Doodads reach VK through
          // vkFeedClassicBucket + the textured M2 pipeline; WMOs through the WMO arena.
          s_wv_max = std::max(s_wv_max, vk_wverts.size());
          s_wi_max = std::max(s_wi_max, vk_widx.size());
          _rb_water = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - _rb_t0).count();
          if (_rb_geom + _rb_water > 6.0)
            LogError << "[VK-SLOW] rebuild phases: geom+alpha=" << _rb_geom
                     << " water=" << _rb_water
                     << " (concat=" << _w_concat << " deriveNew=" << _w_derive
                     << " [par=" << _w_par
                     << " tiles=" << to_derive.size()
                     << " tileMax=" << _w_tile_max.load()
                     << " tileSum=" << _w_tile_sum.load()
                     << " assign=" << _w_asg
                     << " (alloc=" << _w_alloc << " fill=" << _w_fill << ")"
                     << " wvCap=" << vk_wverts.capacity() << " freeV=" << s_free_v.size()
                     << " growV=" << _w_grow_v << " growI=" << _w_grow_i << "])"
                     << " wPre=" << _w_pre << "(res=" << _w_pre_res
                     << " scan=" << _w_pre_scan << " liq=" << _w_pre_liq << ")"
                     << " wTail=" << _w_tail
                     << " texDecode=" << _rb_tex << "/" << _rb_tex_n
                     << " prefetch=" << _rb_pf_decode << "+" << _rb_pf_upload
                     << "/" << _rb_pf_n << " hit=" << _rb_pf_hit << " miss=" << _rb_pf_miss
                     << " dGather=" << _rb_dgather << " dEmit=" << _rb_demit
                     << " wmoEmit=" << _rb_wmoemit
                     << " | packed=" << packed_this_frame << "/" << vk_tiles.size()
                     << " deferred=" << (deferred_any ? 1 : 0)
                     << " derived=" << to_derive.size()
                     << " ms" << std::endl;

          // only the repacked tiles' bytes move to the GPU
          s_vk.backend.setTerrainDirtyRanges(dirty_v, dirty_i, dirty_c);
          s_vk.backend.setAtlasDirtyBands(atlas_bands);
          bool const mesh_uploaded = mesh_ok
                && ((tt_path && s_vk.backend.setTerrainTextured(vk_verts.data(), vk_verts.size() / 9, vk_cidx.data(),
                                                             vk_idx.data(), vk_idx.size(),
                                                             vk_chunks.data(), vk_chunks.size(),
                                                             vk_alpha.data(), vk_shadow.data(),
                                                             kAtlasW, kAtlasH,
                                                             dirty_row_lo, dirty_row_hi))
                || s_vk.backend.setTerrainMesh(vk_verts.data(), vk_verts.size() / 9, vk_idx.data(), vk_idx.size()));
          if (mesh_uploaded)
          {
            // [VULKAN phase E] ADT water is TILE-bound, so it is packed here with the terrain and
            // cached; WMO liquid is VIEW-bound (it comes from whichever WMOs are visible this frame)
            // and is appended per frame below. Binding both to the tile rebuild left VK drawing a
            // stale/empty WMO-liquid set whenever the camera moved without changing the nearest tile.
            // (Removed: two statics that were ASSIGNED the whole ADT water set here -- ~70 MB of
            // vertices plus 6 MB of indices copied on every rebuild -- and never read anywhere. Four
            // references existed in the entire tree: two declarations and these two assignments.)
            // ADT water is TILE-bound: upload it here, with the terrain it belongs to, and leave it
            // alone until the neighbourhood is repacked.
            if (_draw_water.get() && !vk_wverts.empty() && !vk_widx.empty())
            {
              s_vk.backend.setWaterDirtyRanges(wdirty_v, wdirty_i);
              s_vk.backend.setWaterMesh(vk_wverts.data(), vk_wverts.size() / 17,
                                        vk_widx.data(), vk_widx.size());
            }
            else
              s_vk.backend.setWaterMesh(nullptr, 0, nullptr, 0);
            // [phase A] while VK doodads/WMOs are CLAY (no alpha test), composing them under GL leaks
            // through every alpha-tested leaf/fence texel (blue polygons around trees). Compose mode
            // therefore feeds terrain + water only; doodads/WMOs return when VK renders them textured.
            // [2026-09-02 NATIVE VK] setDoodads / the clay doodad pass is gone -- it was untextured
            // scaffolding that compose mode never fed anyway. Tile doodads reach VK through
            // vkFeedClassicBucket and the textured M2 pipeline, in every mode.
            s_pack_pending = deferred_any;
            if (!deferred_any)
              s_vk_tile = best;
            vk_diff::vkReady() = true;
            vk_diff::meshTileX() = static_cast<int>(best->index.x);
            vk_diff::meshTileZ() = static_cast<int>(best->index.z);
            s_vk_tile_count = vk_tiles.size();
            s_vk_dood_count = vk_dood_count;
          }
        }

        } // vk_stat_tile_ms

        // [phase I] TERRAIN VISIBILITY, per frame: sphere-vs-frustum per chunk, planes taken from the
        // same MVP the draw uses. ~2300 tests is nothing; drawing the whole neighbourhood was ~11 ms
        // of GPU. The mesh is untouched -- only the indirect command list changes.
        if (vk_diff::vkReady() && !s_vk_chunk_bounds.empty())
        {
          VkPhaseTimer _t_cull(vk_stat_cull_ms());
          glm::mat4x4 const m = projection() * model_view();
          // [finding 114] identical inputs -> identical lists; skip the walk and both uploads
          static glm::mat4x4 s_cull_last_mvp(0.0f);
          static std::uint64_t s_cull_last_gen = ~0ull;
          bool const cull_same = (s_cull_last_gen == vk_bounds_generation())
                              && std::memcmp(&s_cull_last_mvp[0][0], &m[0][0], sizeof(m)) == 0;
          if (!cull_same)
          {
          s_cull_last_mvp = m;
          s_cull_last_gen = vk_bounds_generation();
          glm::vec4 planes[6];
          for (int i = 0; i < 3; ++i)
          {
            planes[i * 2 + 0] = glm::vec4(m[0][3] + m[0][i], m[1][3] + m[1][i],
                                          m[2][3] + m[2][i], m[3][3] + m[3][i]);
            planes[i * 2 + 1] = glm::vec4(m[0][3] - m[0][i], m[1][3] - m[1][i],
                                          m[2][3] - m[2][i], m[3][3] - m[3][i]);
          }
          for (auto& pl : planes)
          {
            float const len = glm::length(glm::vec3(pl));
            if (len > 0.f) pl /= len;
          }

          static std::vector<Noggit::Rendering::VK::VulkanBackend::ChunkDraw> vis;
          vis.clear();
          vis.reserve(s_vk_chunk_bounds.size());
          for (auto const& b : s_vk_chunk_bounds)
          {
            bool inside = true;
            for (auto const& pl : planes)
            {
              if (glm::dot(glm::vec3(pl), b.centre) + pl.w < -b.radius) { inside = false; break; }
            }
            if (inside)
              vis.push_back({ b.index_count, b.first_index, 0 });
          }
          s_vk.backend.setTerrainVisibleChunks(vis.data(), vis.size());

          // Water, same planes. It is alpha-blended, so unculled surfaces cost far more per pixel
          // than terrain does.
          static std::vector<Noggit::Rendering::VK::VulkanBackend::ChunkDraw> wvis;
          wvis.clear();
          wvis.reserve(s_vk_water_bounds.size());
          for (auto const& b : s_vk_water_bounds)
          {
            bool inside = true;
            for (auto const& pl : planes)
            {
              if (glm::dot(glm::vec3(pl), b.centre) + pl.w < -b.radius) { inside = false; break; }
            }
            if (inside)
              wvis.push_back({ b.index_count, b.first_index, 0 });
          }
          s_vk.backend.setWaterVisibleChunks(wvis.data(), wvis.size());

          static int s_cull_dbg = 0;
          if ((s_cull_dbg++ % 300) == 0)
            LogError << "[VK] cull: terrain " << vis.size() << "/" << s_vk_chunk_bounds.size()
                     << "  water " << wvis.size() << "/" << s_vk_water_bounds.size() << std::endl;
          }   // !cull_same
        }

        // [VULKAN phase E/I] WMO LIQUID, rebuilt every frame because it is VIEW-bound. It used to be
        // appended onto a COPY of the whole ADT water vector, which was then compared against the
        // previous frame to decide whether to re-upload: on a harbour view that is ~34 MB of memcpy
        // plus ~34 MB of compare EVERY FRAME, for the sake of a few thousand WMO vertices. Measured
        // at ~70 ms/frame of pure CPU. The ADT half is tile-bound and now uploads only on the tile
        // rebuild; this builds and uploads just the WMO half, into its own buffer.
        if (vk_diff::vkReady())
        {
          VkPhaseTimer _t_wliq(vk_stat_wliq_ms());
          static std::vector<float> wverts;
          static std::vector<std::uint32_t> widx;

          // [phase K] This rebuild walked EVERY visible WMO liquid vertex every frame (0.38 ms --
          // the largest single item left in the VK-only block). Almost all of that was wasted: of
          // the three UV cases only magma (type 2) and slime (type 3) depend on animtime; ordinary
          // water is a STATIC rotation of tc, so its vertices are identical frame to frame.
          //
          // So: hash what the mesh actually depends on -- the visible groups, their placement, and
          // the animated TEXTURE FRAME index -- and skip the walk when nothing changed. animtime
          // only enters the signature when a scrolling liquid is actually visible, so a scene with
          // magma in view still rebuilds every frame exactly as before. Output is byte-identical.
          std::size_t wliq_sig = 1469598103934665603ull;
          auto hash_mix = [&wliq_sig](std::size_t v)
          {
            wliq_sig ^= v + 0x9e3779b97f4a7c15ull + (wliq_sig << 6) + (wliq_sig >> 2);
          };
          if (_draw_water.get())
          {
            // Pointers and placement only -- NO per-group profile lookup. Resolving the texture
            // profile for every group every frame cost as much as it saved (0.15 ms). Instead the
            // texture-frame clock enters the hash globally below: worst case that rebuilds ~16x a
            // second instead of every frame, which is the same result for a fraction of the work.
            for (auto const& ref : _world->renderer()->vkWmoLiquids())
            {
              hash_mix(std::hash<float>{}(ref.transform[3][0]));
              hash_mix(std::hash<float>{}(ref.transform[3][2]));
              for (WMOGroup* grp : ref.groups)
                if (grp && grp->vkLiquid())
                  hash_mix(reinterpret_cast<std::size_t>(grp));
            }
          }
          hash_mix(static_cast<std::size_t>(_draw_water.get() ? 1 : 0));
          // covers BOTH the animated texture frame (advances every 60 animtime) and the magma/slime
          // UV scroll, without having to know which liquids are on screen
          hash_mix(static_cast<std::size_t>(
            static_cast<unsigned>(std::max<int>(static_cast<int>(_world->animtime), 0)) / 60u));

          static std::size_t s_wliq_sig = 0;
          static bool s_wliq_valid = false;
          bool const wliq_rebuild = !s_wliq_valid || wliq_sig != s_wliq_sig;
          if (wliq_rebuild)
          {
          wverts.clear();
          widx.clear();

          if (_draw_water.get())
          {
            for (auto const& ref : _world->renderer()->vkWmoLiquids())
            {
              for (WMOGroup* grp : ref.groups)
              {
                if (!grp || !grp->vkLiquid())
                  continue;
                wmo_liquid const* lq = grp->vkLiquid();
                auto const& lqv = lq->vkVertices();
                auto const& lqi = lq->vkIndices();
                if (lqv.empty() || lqi.empty())
                  continue;

                // Same profile lookup wmo_liquid::draw does -- WMO liquid ids are REMAPPED before the
                // table hit, so the raw id would pick the wrong texture, anim and type.
                unsigned const profile_id = wmo_liquid::vkTextureProfileId(lq->liquid_id());
                auto const& frames = _world->renderer()->liquidTextureManager().getTextureFrames();
                auto const prof = frames.find(profile_id);
                if (prof == frames.end())
                  continue;   // GL skips the draw too
                glm::vec2 const anim_uv = std::get<1>(prof->second);
                int const liquid_type = std::get<2>(prof->second);
                unsigned const frame_count = std::get<3>(prof->second);
                int const frame = frame_count == 0
                  ? 0
                  : static_cast<int>((static_cast<unsigned>(std::max<int>(static_cast<int>(_world->animtime), 0)) / 60u) % frame_count);

                float lq_tex = -1.f;
                {
                  auto const& names = _world->renderer()->liquidTextureManager().vkTextureNames();
                  auto const it = names.find(profile_id);
                  if (it != names.end())
                    lq_tex = static_cast<float>(vkResolveBlp(it->second.first + std::to_string(frame + 1) + ".blp"));
                }

                float flags = 1.f;                                   // bit0: this is WMO liquid
                if (lq->vkUseMaterialColor()) flags += 2.f;
                if (lq->vkIndoorChannel())    flags += 4.f;
                if (lq->vkDbcExterior())      flags += 8.f;           // client flat river-deep (wmo_liquid.hpp)
                // bit3 rides the material-colour slot: the flat colour IS the WATER param's river-deep
                // band (the same Skies value GL's wmo_water_river_dark uniform carries)
                glm::vec3 const mat = lq->vkDbcExterior() ? Skies::water_river_dark() : lq->materialColor();
                float const at = static_cast<float>(_world->animtime);

                std::uint32_t const base = static_cast<std::uint32_t>(wverts.size() / 17u);
                std::size_t const nv = lqv.size() / 6u;
                for (std::size_t i = 0; i < nv; ++i)
                {
                  glm::vec4 const wp = ref.transform * glm::vec4(lqv[i * 6 + 0], lqv[i * 6 + 1], lqv[i * 6 + 2], 1.f);
                  glm::vec2 const tc(lqv[i * 6 + 4], lqv[i * 6 + 5]);
                  glm::vec2 uv;
                  if (liquid_type == 2)
                    uv = tc + glm::vec2(0.f, 1.f) * (at * (0.25f / 2880.f));
                  else if (liquid_type == 3)
                    uv = tc + anim_uv * (at / 11520.f);
                  else
                  {
                    glm::vec2 const p2 = tc * anim_uv.x;
                    float const a = glm::radians(anim_uv.y);
                    uv = glm::vec2(std::cos(a) * p2.x - std::sin(a) * p2.y,
                                   std::sin(a) * p2.x + std::cos(a) * p2.y);
                  }

                  wverts.push_back(wp.x); wverts.push_back(wp.y); wverts.push_back(wp.z);
                  wverts.push_back(0.f); wverts.push_back(1.f); wverts.push_back(0.f);
                  wverts.push_back(uv.x); wverts.push_back(uv.y);
                  wverts.push_back(lqv[i * 6 + 3]);   // depth_: the raw 0..1 the GL shader mixes with
                  wverts.push_back(static_cast<float>(liquid_type));
                  wverts.push_back(anim_uv.x); wverts.push_back(anim_uv.y);
                  wverts.push_back(lq_tex);
                  wverts.push_back(flags);
                  wverts.push_back(mat.x); wverts.push_back(mat.y); wverts.push_back(mat.z);
                }
                for (std::uint16_t idx : lqi)
                  widx.push_back(base + idx);
              }
            }
          }

          // Small and view-dependent: just write it, no whole-mesh comparison.
          s_vk.backend.setWmoLiquidMesh(wverts.empty() ? nullptr : wverts.data(),
                                        wverts.size() / 17,
                                        widx.empty() ? nullptr : widx.data(), widx.size());
          s_wliq_sig = wliq_sig;
          s_wliq_valid = true;
          }   // else: the mesh the backend already holds is still exactly right
        }

        // self-contained clock (this function has no `now` local)
        static auto const s_vk_t0 = std::chrono::steady_clock::now();
        float const vk_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_vk_t0).count();
        glm::mat4x4 const vk_mvp = projection() * model_view();
        // [VULKAN phase B] per-frame lighting/fog block for the textured terrain (same numbers GL uses)
        {
          auto const& lb = _world->renderer()->lightingBlock();
          glm::vec3 const sun_spec = _world->renderer()->sunSpecColor();
          static_assert(sizeof(OpenGL::LightingUniformBlock) == 864, "VK terrain_tex.frag Lighting block layout must match (extras at byte 864)");
          glm::vec3 const sheen_dir = _world->renderer()->celestialDirForVk();
          // Camera FORWARD: the clutter fade is a view-DEPTH ramp (the client's viewZ), which is
          // dot(world - camera, forward) -- cheaper than shipping the whole view matrix.
          glm::mat4x4 const mv_for_fwd = model_view();
          glm::vec3 const cam_fwd = -glm::normalize(glm::vec3(mv_for_fwd[0][2], mv_for_fwd[1][2], mv_for_fwd[2][2]));
          // The other two view axes: m2.vert rebuilds view space from these to compute the M2
          // SPHERE-MAP uv (env shine), which GL does with the real view matrix.
          glm::vec3 const cam_right = glm::normalize(glm::vec3(mv_for_fwd[0][0], mv_for_fwd[1][0], mv_for_fwd[2][0]));
          glm::vec3 const cam_up = glm::normalize(glm::vec3(mv_for_fwd[0][1], mv_for_fwd[1][1], mv_for_fwd[2][1]));
          float extra[28] = {
            _camera.position.x, _camera.position.y, _camera.position.z, 0.f,   // camera
            sun_spec.x, sun_spec.y, sun_spec.z,                                // terrain specular colour (SUN band)
            _settings->value("render/terrain_specular", true).toBool() ? 1.f : 0.f,
            _settings->value("render/baked_shadows", true).toBool() ? 1.f : 0.f,
            _draw_fog.get() ? 1.f : 0.f,
            _settings->value("render/vertex_color", true).toBool() ? 1.f : 0.f, 0.f, // toggles
            sheen_dir.x, sheen_dir.y, sheen_dir.z, 0.f,   // liquid sun-sheen direction
            cam_fwd.x, cam_fwd.y, cam_fwd.z,
            _world->renderer()->vkClutterDetailDist(),        // ground-clutter fade ramp
            cam_right.x, cam_right.y, cam_right.z, 0.f,
            cam_up.x, cam_up.y, cam_up.z, 0.f };             // view basis for the sphere map
          s_vk.backend.setLighting(&lb, sizeof(lb), extra);
        }


        // [VULKAN phase D] WMO groups. The arena is append-only and uploaded whenever it grew; the
        // per-frame draw list replays exactly the runs GL emitted this frame (verified against a GL
        // draw-call counter: 35/35, 267/267, 23/23, 55/55).
        if (s_vk.backend.wmoAvailable())
        {
          auto* wr = _world->renderer();
          // VK owns the WMO pass -- EXCEPT in parity mode. The harness renders the FULL GL scene as
          // its reference (s_vk_owned_passes carries the same !vk_diff::enabled() guard for terrain),
          // so gating GL's WMO there would strip WMOs from the reference and compare VK-with-WMOs
          // against GL-without. Outside parity mode the gate is what stops GL redrawing over VK.
          wr->setVkOwnsWmo(!vk_diff::enabled());

          // [phase F] SKY GRADIENT DOME. Upload whenever the zone light re-derived the band colours;
          // ownership follows the same rule as every other pass -- VK draws it outside parity mode,
          // and inside parity mode only when NOGGIT_PARITY_SKY asks for it (otherwise GL keeps
          // drawing the reference dome and the metric masks those pixels out anyway).
          t_prepA = std::chrono::steady_clock::now();
          if (auto& skies = wr->skies())
          {
            // vkDomeDirty() is set on essentially every frame because the dome re-tints with the
            // clock, but the tint only changes VISIBLY over seconds. Sample a handful of colours
            // and skip the re-upload when the quantised sample is unchanged.
            bool dome_changed = true;
            if (!skies->vkDomeColors().empty())
            {
              std::size_t dsig = 1469598103934665603ull;
              auto const& dc = skies->vkDomeColors();
              for (std::size_t i = 0; i < 12; ++i)
              {
                auto const& c = dc[(i * dc.size()) / 12u];
                dsig ^= static_cast<std::size_t>(static_cast<int>(c.x * 512.f)) + 0x9e3779b9 + (dsig << 6) + (dsig >> 2);
                dsig ^= static_cast<std::size_t>(static_cast<int>(c.y * 512.f)) + 0x9e3779b9 + (dsig << 6) + (dsig >> 2);
                dsig ^= static_cast<std::size_t>(static_cast<int>(c.z * 512.f)) + 0x9e3779b9 + (dsig << 6) + (dsig >> 2);
              }
              static std::size_t s_dome_sig = 0;
              static bool s_dome_seen = false;
              dome_changed = !s_dome_seen || dsig != s_dome_sig;
              if (dome_changed) { s_dome_sig = dsig; s_dome_seen = true; }
            }
            if (dome_changed
                && skies->vkDomeDirty()
                && !skies->vkDomeVertices().empty() && !skies->vkDomeIndices().empty()
                && skies->vkDomeColors().size() == skies->vkDomeVertices().size())
            {
              if (s_vk.backend.setSkyDome(&skies->vkDomeVertices()[0].x,
                                          &skies->vkDomeColors()[0].x,
                                          skies->vkDomeVertices().size(),
                                          skies->vkDomeIndices().data(),
                                          skies->vkDomeIndices().size()))
              {
                skies->clearVkDomeDirty();
              }
            }
            // Same rule as every other pass: NEVER gate GL inside parity mode -- the reference image
            // must keep rendering the full GL scene. NOGGIT_PARITY_SKY only stops the metric from
            // masking sky pixels out.
            t_sk1 = std::chrono::steady_clock::now();
            {
              // [SKY BLACK] VK's sky renders black looking UP while GL renders the dome + clouds.
              // Everything about the dome feed and the draw gate, once a second.
              static int s_skydbg = 0;
              if ((s_skydbg++ % 60) == 0)
                LogError << "[VK] SKYDBG domeChanged=" << (dome_changed ? 1 : 0)
                         << " domeDirty=" << (skies->vkDomeDirty() ? 1 : 0)
                         << " verts=" << skies->vkDomeVertices().size()
                         << " idx=" << skies->vkDomeIndices().size()
                         << " colors=" << skies->vkDomeColors().size()
                         << " domeReady=" << (s_vk.backend.skyDomeReady() ? 1 : 0)
                         << " vkOwnsDome=" << ((s_vk.backend.skyDomeReady() && !vk_diff::enabled()) ? 1 : 0)
                         << std::endl;
            }
            skies->setVkOwnsDome((s_vk.native || s_vk.backend.skyDomeReady()) && !vk_diff::enabled());

            // [phase F] CLOUD DECK. The cap mesh uploads once; the 128x128 deck texture owns one
            // bindless slot that is re-uploaded in place whenever tick_clouds regenerates it (10 Hz),
            // because addTexture() would otherwise burn a new slot every tick.
            if (skies->vkCloudMeshDirty()
                && !skies->vkCloudVertices().empty() && !skies->vkCloudIndices().empty())
            {
              if (s_vk.backend.setCloudDome(skies->vkCloudVertices().data(),
                                            skies->vkCloudVertices().size() / 6u,
                                            skies->vkCloudIndices().data(),
                                            skies->vkCloudIndices().size()))
              {
                skies->clearVkCloudMeshDirty();
              }
            }
            {
              t_sk2 = std::chrono::steady_clock::now();
              static std::int32_t s_cloud_tex = -1;
              static unsigned s_cloud_serial = 0xFFFFFFFFu;
              auto const& rgba = skies->vkCloudRgba();
              if (rgba.size() >= 4u && skies->vkCloudTexSerial() != s_cloud_serial)
              {
                auto const* px = reinterpret_cast<std::uint32_t const*>(rgba.data());
                unsigned const side = 128u;   // CloudGen::SIZE
                if (rgba.size() >= static_cast<std::size_t>(side) * side * 4u)
                {
                  if (s_cloud_tex < 0)
                    s_cloud_tex = s_vk.backend.addTexture(px, side, side);
                  else
                    s_vk.backend.updateTexture(s_cloud_tex, px, side, side);
                  s_cloud_serial = skies->vkCloudTexSerial();
                }
              }
              s_vk.backend.setCloudParams(s_cloud_tex, skies->vkCloudOpacity());
              {
                static int s_cdbg = 0;
                if ((s_cdbg++ % 120) == 0 && rgba.size() >= 4u)
                {
                  unsigned amin = 255, amax = 0; unsigned long asum = 0, n = 0;
                  for (std::size_t i = 3; i < rgba.size(); i += 4)
                  { unsigned const a = rgba[i]; amin = std::min(amin, a); amax = std::max(amax, a); asum += a; ++n; }
                  LogError << "[VK] cloud: texId=" << s_cloud_tex << " opacity=" << skies->vkCloudOpacity()
                           << " serial=" << skies->vkCloudTexSerial()
                           << " alpha min/max/mean=" << amin << "/" << amax << "/" << (n ? asum / n : 0)
                           << " verts=" << (skies->vkCloudVertices().size() / 6u)
                           << " idx=" << skies->vkCloudIndices().size() << std::endl;
                }
              }
            }
            t_sk3 = std::chrono::steady_clock::now();
            skies->setVkOwnsClouds(s_vk.backend.cloudDomeReady() && !vk_diff::enabled());
          }

          t_s2a = std::chrono::steady_clock::now();
          // [phase F] CELESTIAL billboards (sun/moon discs + glare). WorldRender records what the sky
          // pass issued LAST frame; resolve each texture to a bindless id and hand the list over.
          {
            static std::vector<Noggit::Rendering::VK::VulkanBackend::Celestial> cels;
            cels.clear();
            for (auto const& c : wr->vkCelestials())
            {
              // non-blocking: this loop ALREADY skips an unresolved texture for the frame, so
              // waiting on the background decoder costs nothing but one frame of latency, where
              // decoding inline costs the whole render thread an archive read (finding 85).
              std::int32_t const tex = vkResolveBlpEx(c.blp, false);
              if (tex < 0)
                continue;   // not resolvable yet -- GL still owns it this frame
              Noggit::Rendering::VK::VulkanBackend::Celestial rec{};
              rec.center_rel[0] = c.center_rel.x; rec.center_rel[1] = c.center_rel.y; rec.center_rel[2] = c.center_rel.z;
              rec.half_size = c.half_size;
              rec.right[0] = c.right.x; rec.right[1] = c.right.y; rec.right[2] = c.right.z;
              rec.opacity = c.opacity;
              rec.up[0] = c.up.x; rec.up[1] = c.up.y; rec.up[2] = c.up.z;
              rec.tex_index = static_cast<float>(tex);
              rec.color[0] = c.color.x; rec.color[1] = c.color.y; rec.color[2] = c.color.z;
              rec.additive = c.additive ? 1.f : 0.f;
              cels.push_back(rec);
            }
            {
              static int s_cel_dbg = 0;
              if ((s_cel_dbg++ % 120) == 0)
                LogError << "[VK] celestial feed: recorded=" << wr->vkCelestials().size()
                         << " resolved=" << cels.size()
                         << (wr->vkCelestials().empty() ? " (sky pass issued none)" : "") << std::endl;
            }
            s_vk.backend.setCelestials(cels);
            wr->setVkOwnsCelestials((s_vk.native || s_vk.backend.celestialsReady()) && !vk_diff::enabled());
          }

          // [phase G] PARTICLES: the emitters mirrored this frame's quads in world space; resolve each
          // emitter texture to a bindless id and hand the streams over in GL's draw order.
          t_s2b = std::chrono::steady_clock::now();
          {
            VkPhaseTimer _t_part(vk_stat_part_ms());
            auto& feed = Noggit::Rendering::VK::particleFeed();
            static std::vector<Noggit::Rendering::VK::VulkanBackend::ParticleDraw> pdraws;
            pdraws.clear();
            pdraws.reserve(feed.draws.size());
            for (auto const& d : feed.draws)
            {
              // finding 85: with the WMO table deferring its decodes, THIS became the blocking
              // one -- s2part jumped to 15 ms the moment s2wmo stopped paying for the same
              // textures. Defer here too and drop the draw for a frame instead.
              std::int32_t const ptex = vkResolveBlpEx(d.blp, false);
              if (ptex == kBlpPending)
                continue;
              Noggit::Rendering::VK::VulkanBackend::ParticleDraw pd{};
              pd.first_index = d.first_index;
              pd.index_count = d.index_count;
              pd.base_vertex = d.base_vertex;
              pd.tex_index = ptex;
              pd.blend = static_cast<float>(d.blend);
              pd.alpha_test = d.alpha_test;
              pd.alpha_mod = d.alpha_mod;
              pd.ribbon = d.ribbon ? 1.f : 0.f;
              pdraws.push_back(pd);
            }
            s_vk.backend.setParticles(feed.vertices.data(), feed.vertices.size() / 9,
                                      feed.indices.data(), feed.indices.size(),
                                      pdraws.data(), pdraws.size());
            bool const vk_owns_quads = s_vk.backend.particlesReady() && !vk_diff::enabled();
            Noggit::Rendering::VK::vkOwnsParticles() = vk_owns_quads;
            Noggit::Rendering::VK::vkOwnsRibbons() = vk_owns_quads;
            static int s_pdbg = 0;
            if ((s_pdbg++ % 120) == 0 && (!feed.draws.empty() || feed.skipped))
            {
              LogError << "[VK] particle feed: draws=" << feed.draws.size()
                       << " verts=" << (feed.vertices.size() / 9)
                       << " idx=" << feed.indices.size()
                       << " GL-ONLY skipped=" << feed.skipped << std::endl;
            }
          }
          {
          }
          t_s2c = std::chrono::steady_clock::now();
          if (wr->vkWmoArenaDirty() && !wr->vkWmoArenaVertices().empty())
          {
            VkPhaseTimer _t_warena(vk_stat_warena_ms());
            if (s_vk.backend.setWmoArena(wr->vkWmoArenaVertices().data(),
                                         wr->vkWmoArenaVertices().size() * sizeof(Noggit::Rendering::VkWmoVertex),
                                         wr->vkWmoArenaIndices().data(),
                                         wr->vkWmoArenaIndices().size()))
            {
              wr->clearVkWmoArenaDirty();
            }
          }
          // Batch table: resolve each batch's BLP pair to bindless ids (shared texture cache) and
          // upload once whenever the arena grew. Indexed per fragment by the per-vertex batch id.
          if (wr->vkWmoBatchesDirty() && !wr->vkWmoBatches().empty())
          {
            auto const& pairs = wr->vkWmoBlpPairs();
            std::vector<glm::ivec2> pair_ids;
            pair_ids.reserve(pairs.size());
            // [finding 85] This used to decode every unseen WMO BLP inline, which is the whole of
            // the 10-13 ms "s2wmo" spike -- the same inline archive read that made tile crossings
            // hitch. Ask the background decoder first and leave the table dirty if anything is not
            // ready: the WMO keeps last frame's batch table for a frame or two instead of the
            // render thread blocking on a disk read. After kBlpWaitFrames the wait is abandoned and
            // it resolves inline, so a name that never decodes cannot stall the table forever.
            static int s_blp_wait = 0;
            // Only a backstop now: every queued name publishes a result (success or failure)
            // within a few frames, so this should never fire. At 8 it fired routinely and cost
            // 8.9 ms when it did.
            static constexpr int kBlpWaitFrames = 600;
            bool const blp_blocking = ++s_blp_wait > kBlpWaitFrames;
            bool blp_pending = false;
            auto const _wp0 = std::chrono::steady_clock::now();
            // [finding 86] Resolving the pairs one at a time took the decoder mutex and called
            // notify_all once PER NAME, thousands of times, and cost 5.8 ms on a frame where it
            // decoded nothing at all (pairs=5.81, table=0). Do the whole pass as one batch: a
            // lock-free sweep of the resolved cache, then ONE lock to collect what the decoder
            // finished and queue what it has not, then create the textures outside the lock.
            {
              static std::vector<std::string> need;
              static std::unordered_set<std::string> need_set;
              need.clear(); need_set.clear();
              auto const want_name = [&](std::string const& n)
              {
                if (n.empty() || s_vk_tex_ids.count(n)) return;
                if (need_set.insert(n).second) need.push_back(n);
              };
              auto _ws0 = std::chrono::steady_clock::now();
              for (auto const& pr : pairs) { want_name(pr.first); want_name(pr.second); }
              vk_stat_wp_sweep_ms() += std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - _ws0).count();
              vk_stat_wp_need() += static_cast<double>(need.size());

              std::vector<VkPfDecoded> got(need.size());
              std::vector<char> have(need.size(), 0);
              auto _wl0 = std::chrono::steady_clock::now();
              if (!need.empty())
              {
                std::lock_guard<std::mutex> lk(s_pf_mtx);
                if (!s_pf_thread.joinable())
                  s_pf_thread = std::thread(vkTilesetDecodeLoop, _context);
                bool queued = false;
                for (std::size_t k = 0; k < need.size(); ++k)
                {
                  auto rit = s_pf_ready.find(need[k]);
                  if (rit != s_pf_ready.end())
                  {
                    got[k] = std::move(rit->second);
                    s_pf_ready.erase(rit);
                    s_pf_seen.erase(need[k]);
                    have[k] = 1;
                    continue;
                  }
                  if (s_pf_seen.insert(need[k]).second)
                  { s_pf_queue.push_back(need[k]); queued = true; }
                }
                if (queued) s_pf_cv.notify_all();   // once, not once per name
              }
              vk_stat_wp_lock_ms() += std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - _wl0).count();
              auto _wm0 = std::chrono::steady_clock::now();
              // [finding 86] Creating every decoded WMO texture in one frame is the remaining
              // s2wmo spike: 315 texture pairs is nowhere near enough lookups to cost 6 ms, but
              // ~600 VK image creations plus their staging uploads is. Budget them.
              static constexpr int kWmoTexPerFrame = 4;    // ~0.24 ms per VK image create+upload: 21 in one
                                                           // frame was 4.96 ms, 4 is ~1 ms
              int created = 0;
              for (std::size_t k = 0; k < need.size(); ++k)
              {
                if (!have[k]) continue;
                if (created >= kWmoTexPerFrame)
                {
                  // hand it back to the ready map so the next frame picks it up without
                  // re-decoding, and keep the table dirty
                  std::lock_guard<std::mutex> lk(s_pf_mtx);
                  s_pf_ready.emplace(need[k], std::move(got[k]));
                  s_pf_seen.insert(need[k]);
                  blp_pending = true;
                  continue;
                }
                vkCreateFromDecoded(need[k], got[k]);
                ++created;
              }

              vk_stat_wp_make_ms() += std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - _wm0).count();
              auto _wi0 = std::chrono::steady_clock::now();
              auto const id_of = [&](std::string const& n) -> std::int32_t
              {
                if (n.empty()) return -1;
                auto it2 = s_vk_tex_ids.find(n);
                if (it2 != s_vk_tex_ids.end()) return it2->second;
                if (!blp_blocking) { blp_pending = true; return -1; }
                return vkCreateFromDecoded(n, vkDecodeTileset(n, _context));
              };
              for (auto const& pr : pairs)
                pair_ids.emplace_back(id_of(pr.first), id_of(pr.second));
              vk_stat_wp_idof_ms() += std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - _wi0).count();
            }
            vk_stat_wpairs_ms() += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - _wp0).count();
            if (!blp_pending)
            {
            VkPhaseTimer _t_wtable(vk_stat_wtable_ms());
            s_blp_wait = 0;

            auto const& bts = wr->vkWmoBatches();
            std::vector<glm::ivec4> recs(bts.size());
            for (std::size_t i = 0; i < bts.size(); ++i)
            {
              int const pi = bts[i].x;
              recs[i] = glm::ivec4(pi >= 0 && pi < static_cast<int>(pair_ids.size()) ? pair_ids[pi].x : -1,
                                   pi >= 0 && pi < static_cast<int>(pair_ids.size()) ? pair_ids[pi].y : -1,
                                   bts[i].y,
                                   // flags | alpha_test_mode << 16 -- the mode was being dropped, and
                                   // without it every alpha-keyed WMO material rendered solid
                                   bts[i].z | (bts[i].w << 16));
            }
            if (s_vk.backend.setWmoBatches(reinterpret_cast<std::int32_t const*>(recs.data()), recs.size()))
            {
              wr->clearVkWmoBatchesDirty();
              // The table is rebuilt on ~150 frames of a flight; logging seven lines each time is
              // file I/O on the render thread inside the phase being measured. Diagnose once.
              static int s_wmo_tab_log = 0;
              if (s_wmo_tab_log++ < 2)
              {
                // [VK-DIFF] flag histogram: if ExteriorLit (0x1) is never set, every batch falls to the
                // interior branch and the VK lighting goes flat (constant MOHD ambient).
                int ext = 0, mocv = 0, unlit = 0, zero = 0;
                for (auto const& b : bts)
                {
                  if (b.z == 0) ++zero;
                  if (b.z & 0x1) ++ext;
                  if (b.z & 0x2) ++mocv;
                  if (b.z & 0x4) ++unlit;
                }
                LogError << "[VK] WMO batch flags: n=" << bts.size() << " ExteriorLit=" << ext
                         << " HasMOCV=" << mocv << " Unlit=" << unlit << " zeroFlags=" << zero << std::endl;
              }
              if (s_wmo_tab_log <= 2)
              {
                int unresolved = 0, empty_name = 0;
                for (std::size_t i = 0; i < pair_ids.size(); ++i)
                {
                  if (pairs[i].first.empty()) ++empty_name;
                  else if (pair_ids[i].x < 0) ++unresolved;
                }
                LogError << "[VK] WMO batch table: " << recs.size() << " batches / "
                         << pairs.size() << " texture pairs, unresolved=" << unresolved
                         << " emptyName=" << empty_name << std::endl;
              }
            }
            }   // !blp_pending
          }

          t_prepB = std::chrono::steady_clock::now();
          auto const& wdraws = wr->vkWmoDraws();
          auto const& wxf = wr->vkWmoTransforms();
          if (!wdraws.empty() && !wxf.empty())
          {
            static_assert(sizeof(Noggit::Rendering::WorldRender::VkWmoDraw)
                          == sizeof(Noggit::Rendering::VK::VulkanBackend::WmoDraw),
                          "WMO draw record must match the backend's layout");
            s_vk.backend.setWmoFrame(reinterpret_cast<float const*>(wxf.data()),
                                     reinterpret_cast<float const*>(wr->vkWmoAmbients().data()), wxf.size(),
                                     reinterpret_cast<Noggit::Rendering::VK::VulkanBackend::WmoDraw const*>(wdraws.data()),
                                     wdraws.size());
          }
          else
          {
            s_vk.backend.setWmoFrame(nullptr, nullptr, 0, nullptr, 0);
          }
        }

        // ---- [VULKAN CLUTTER PERSISTENT, 2026-09-03] --------------------------------------------
        // Ground clutter is static per chunk. Each computed chunk registers ONCE into a per-chunk
        // VK instance buffer; per frame only the VISIBLE chunks' draw records go over (a few KB),
        // and the walk skips their per-blade re-collection entirely (wr->vk_clutter_chunks). Active
        // only in native mode at 100% density -- any other state empties the set and the classic
        // per-frame path takes over seamlessly next frame.
        {
          using ClutterDraw = Noggit::Rendering::VK::VulkanBackend::ClutterDraw;
          struct ClReg
          {
            MapTile* tile; std::size_t tx, tz;   // TileIndex fields are size_t
            MapChunk const* chunk;
            glm::vec3 vmin, vmax, center;
            std::int32_t slot;
            std::vector<ClutterDraw> draws;
          };
          static std::vector<ClReg> s_cl_regs;
          auto* wr_cl = _world->renderer();
          float const cl_density = std::clamp(_settings->value("render/ground_clutter_density", 100.0f).toFloat(), 0.0f, 100.0f) / 100.0f;
          float const cl_dist = _settings->value("render/ground_clutter_distance", 70.0f).toFloat();
          // [2026-09-04] DEFAULT OFF. This persistent path is one day old, was never validated in a
          // real grass field, and splits clutter between two render paths at the registration
          // radius -- the prime suspect for the reported horizontal grass BANDS that track the
          // camera, and the grass flashing on camera movement. The proven per-frame path is the
          // default again; NOGGIT_VK_CLUTTER_PERSIST=1 opts back in for testing.
          static bool const s_cl_persist_ab = []() {
            char const* v = std::getenv("NOGGIT_VK_CLUTTER_PERSIST");
            return v && *v == '1';
          }();
          bool const cl_active = s_cl_persist_ab && s_vk.native && _draw_ground_clutter.get()
                              && cl_density >= 0.999f;
          float const cl_reach = cl_dist + 24.0f;   // chunk half-diagonal slack, like the walk's gate
          auto const cl_dist2 = [](glm::vec3 const& a, glm::vec3 const& b)
          { glm::vec3 const d = a - b; return d.x * d.x + d.y * d.y + d.z * d.z; };
          // engagement diagnostics: why chunks do (not) register, one line per ~300 frames
          static unsigned s_cl_dbg_tick = 0;
          unsigned dbg_computed = 0, dbg_reach = 0, dbg_bfail = 0, dbg_pend = 0;

          // SWEEP: release registrations whose tile is gone. Pointer-safe (water-span lesson): the
          // stored MapTile* is only dereferenced after it is proven to be in the LIVE set, and its
          // identity is confirmed against the recorded tile coordinates.
          {
            static std::unordered_set<MapTile*> s_cl_live;
            s_cl_live.clear();
            for (MapTile* t : _world->mapIndex.loaded_tiles())
              if (t)
                s_cl_live.insert(t);
            for (std::size_t i = 0; i < s_cl_regs.size(); )
            {
              MapTile* const t = s_cl_regs[i].tile;
              bool const alive = s_cl_live.count(t)
                              && t->index.x == s_cl_regs[i].tx && t->index.z == s_cl_regs[i].tz;
              if (!alive)
              {
                s_vk.backend.clutterReleaseChunk(s_cl_regs[i].slot);
                s_cl_regs[i] = std::move(s_cl_regs.back());
                s_cl_regs.pop_back();
              }
              else
                ++i;
            }
          }

          // registry -> renderer set, REBUILT every frame (fallback safety)
          wr_cl->vk_clutter_chunks.clear();
          if (cl_active)
            for (auto const& r : s_cl_regs)
              wr_cl->vk_clutter_chunks.insert(r.chunk);

          // REGISTER new computed chunks in range, a few per frame
          if (cl_active)
          {
            int cl_budget = 6;
            for (MapTile* t : _world->mapIndex.loaded_tiles())
            {
              if (cl_budget <= 0)
                break;
              if (!t || !t->finishedLoading())
                continue;
              for (int ccz = 0; ccz < 16 && cl_budget > 0; ++ccz)
                for (int ccx = 0; ccx < 16 && cl_budget > 0; ++ccx)
                {
                  MapChunk* const ch = t->getChunk(ccx, ccz);
                  if (!ch || !ch->_detail_doodads_computed)
                    continue;
                  ++dbg_computed;
                  if (wr_cl->vk_clutter_chunks.count(ch))
                    continue;
                  if (cl_dist2(_camera.position, ch->vcenter) > cl_reach * cl_reach)
                    continue;
                  ++dbg_reach;
                  Noggit::Rendering::WorldRender::VkClutterOut outp;
                  if (!wr_cl->vkClutterBuildChunk(ch, outp))
                  {
                    ++dbg_bfail;
                    continue;   // species still streaming / arena slot missing -> retry later
                  }
                  --cl_budget;
                  if (outp.tf.empty())
                  {
                    // registrable-as-empty: remember it so the walk stops re-testing the chunk
                    s_cl_regs.push_back(ClReg{ t, t->index.x, t->index.z, ch,
                                               ch->vmin, ch->vmax, ch->vcenter, -1, {} });
                    wr_cl->vk_clutter_chunks.insert(ch);
                    continue;
                  }
                  // BLP names -> bindless ids (non-blocking; a pending decode defers the chunk)
                  bool cl_pending = false;
                  for (auto const& d : outp.draws)
                  {
                    std::int32_t const i0 = vkResolveBlpEx(d.blp0, false);
                    std::int32_t const i1 = d.blp1.empty() ? -1 : vkResolveBlpEx(d.blp1, false);
                    if (i0 == kBlpPending || i1 == kBlpPending)
                    {
                      cl_pending = true;
                      break;
                    }
                    for (std::size_t k = 0; k < d.tex_count; ++k)
                    {
                      outp.tex[d.tex_from + k].x = i0;
                      outp.tex[d.tex_from + k].y = i1;
                    }
                  }
                  if (cl_pending)
                  {
                    ++dbg_pend;
                    ++cl_budget;   // decode queued in the background; costs no budget
                    continue;
                  }
                  std::int32_t const cl_slot = s_vk.backend.clutterRegisterChunk(
                      reinterpret_cast<float const*>(outp.tf.data()),
                      reinterpret_cast<float const*>(outp.interior.data()),
                      reinterpret_cast<std::int32_t const*>(outp.tex.data()),
                      reinterpret_cast<std::int32_t const*>(outp.state.data()),
                      outp.tf.size());
                  if (cl_slot < 0)
                    continue;
                  ClReg reg{ t, t->index.x, t->index.z, ch, ch->vmin, ch->vmax, ch->vcenter, cl_slot, {} };
                  reg.draws.reserve(outp.draws.size());
                  for (auto const& d : outp.draws)
                  {
                    ClutterDraw cd;
                    cd.index_count = d.index_count;
                    cd.first_index = d.first_index;
                    cd.base_vertex = d.base_vertex;
                    cd.first_instance = d.first_instance;
                    cd.instance_count = d.instance_count;
                    cd.chunk_slot = cl_slot;
                    cd.state_key = d.state_key;
                    reg.draws.push_back(cd);
                  }
                  s_cl_regs.push_back(std::move(reg));
                  wr_cl->vk_clutter_chunks.insert(ch);
                  static int s_cl_reg_log = 0;
                  if (s_cl_reg_log++ < 4)
                    LogError << "[VK] clutter chunk registered: " << outp.tf.size()
                             << " instances, " << outp.draws.size() << " draws (slot " << cl_slot << ")" << std::endl;
                }
            }
          }

          if ((s_cl_dbg_tick++ % 300u) == 0)
          {
            std::size_t dbg_slots = 0, dbg_inst = 0, dbg_draws = 0;
            for (auto const& r : s_cl_regs)
              if (r.slot >= 0)
              {
                ++dbg_slots;
                dbg_draws += r.draws.size();
                if (!r.draws.empty())
                  dbg_inst += r.draws.front().instance_count;
              }
            LogError << "[VK] clutter reg scan: computed=" << dbg_computed
                     << " inReach=" << dbg_reach << " buildFail=" << dbg_bfail
                     << " blpPending=" << dbg_pend << " regs=" << s_cl_regs.size()
                     << " withSlots=" << dbg_slots << " inst~=" << dbg_inst
                     << " draws=" << dbg_draws
                     << " active=" << (cl_active ? 1 : 0)
                     << " density=" << cl_density << std::endl;
          }

          // FRAME LIST: visible registered chunks, sorted by (pipeline state, chunk)
          {
            static std::vector<ClutterDraw> s_cl_frame;
            s_cl_frame.clear();
            if (cl_active && !s_cl_regs.empty())
            {
              glm::mat4x4 const cl_mvp = projection() * model_view();
              math::frustum const cl_fr(cl_mvp);
              for (auto const& r : s_cl_regs)
              {
                if (r.slot < 0 || r.draws.empty())
                  continue;
                if (cl_dist2(_camera.position, r.center) > cl_reach * cl_reach)
                  continue;
                if (!cl_fr.intersects(r.vmax, r.vmin))
                  continue;
                s_cl_frame.insert(s_cl_frame.end(), r.draws.begin(), r.draws.end());
              }
              std::sort(s_cl_frame.begin(), s_cl_frame.end(),
                        [](ClutterDraw const& a, ClutterDraw const& b)
                        {
                          return a.state_key != b.state_key ? a.state_key < b.state_key
                                                            : a.chunk_slot < b.chunk_slot;
                        });
            }
            s_vk.backend.setClutterFrame(s_cl_frame.empty() ? nullptr : s_cl_frame.data(),
                                         s_cl_frame.size());
          }

          // live sway: species headers + current matrices appended to the snapshot bone stream
          if (cl_active)
            wr_cl->vkClutterAppendBones(model_view(), static_cast<int>(_world->model_animtime));
        }

        // [VULKAN phase C] M2 / doodad batches: hand VK the SAME arena + streams GL batched this frame.
        // Transitional: only batches whose textures resolve are drawn by VK; the rest still come from GL
        // (tracked TODO -- the port is not done until nothing falls back).
        if (s_vk.backend.m2Available())
        {
          VkPhaseTimer _t_m2(vk_stat_m2up_ms());
          auto* wr = _world->renderer();
          // [dev] NOGGIT_VK_M2_ARENA_ALWAYS=1 re-uploads the arena every frame -- proves whether the
          // dirty flag is letting VK keep a STALE copy of the shared arena.
          static bool const s_arena_always = std::getenv("NOGGIT_VK_M2_ARENA_ALWAYS") != nullptr;
          if ((wr->mdiMirrorDirty() || s_arena_always) && !wr->mdiMirrorVertices().empty())
          {
            if (s_vk.backend.setM2Arena(wr->mdiMirrorVertices().data(), wr->mdiMirrorVertices().size(),
                                        wr->mdiMirrorIndices().data(), wr->mdiMirrorIndices().size()))
            {
              wr->clearMdiMirrorDirty();
            }
          }
          auto const& tf = wr->vkM2Transforms();
          auto const& inter = wr->vkM2Interiors();
          auto const& texinfo = wr->vkM2TexInfo();
          auto const& blpidx = wr->vkM2BlpIndex();
          auto const& pairs = wr->vkM2BlpPairs();
          auto const& cmds = wr->vkM2Commands();
          {
            static int s_m2_diag = 0;
            if (s_m2_diag++ % 120 == 0)
            {
              {
                // Do the draw commands actually address the arena MIRROR we hand VK? If GL batches
                // reference vertices/indices past the mirror, VK rasterises nothing for those draws.
                std::uint32_t max_idx = 0, max_vtx = 0;
                for (auto const& c : cmds)
                {
                  max_idx = std::max(max_idx, c.firstIndex + c.count);
                  max_vtx = std::max(max_vtx, static_cast<std::uint32_t>(c.baseVertex));
                }
                LogError << "[VK] M2 arena: mirrorVerts=" << (wr->mdiMirrorVertices().size() / 48)
                         << " mirrorIdx=" << wr->mdiMirrorIndices().size()
                         << " cmdMaxIndex=" << max_idx << " cmdMaxBaseVertex=" << max_vtx
                         << " dirty=" << wr->mdiMirrorDirty()
                         << " | GL-ONLY mdiIssued=" << wr->mdiIssued()
                         << " classicInstances=" << wr->classicIssued()
                         << " vkClassicFed=" << wr->vkClassicFed()
                         << " FALLBACK pass=" << wr->vkFallbackPass()
                         << " arena=" << wr->vkFallbackArena()
                         << " empty=" << wr->vkFallbackEmpty() << std::endl;
                for (auto const& fb : wr->vkFallbackNames())
                  LogError << "[VK]   GL-ONLY " << fb << std::endl;
              }
              LogError << "[VK] wmoAvailable=" << s_vk.backend.wmoAvailable()
                       << " wmoDrawsSubmitted=" << wr->vkWmoDraws().size() << std::endl;
              LogError << "[VK] WMO feed: draws=" << wr->vkWmoDraws().size()
                       << " instances=" << wr->vkWmoTransforms().size()
                       << " arenaVerts=" << wr->vkWmoArenaVertices().size()
                       << " arenaIdx=" << wr->vkWmoArenaIndices().size()
                       << " dirty=" << wr->vkWmoArenaDirty()
                       << " FALLBACK groups=" << wr->vkWmoFallback()
                       << " | GL issued=" << wr->glWmoDrawCalls() << std::endl;
              LogError << "[VK] M2 feed src=" << wr->batchSource()
                       << " slice=" << wr->batchSliceDist()
                       << " glIssued=" << wr->batchIssued()
                       << " groups=" << wr->vkM2Groups().size() << std::endl;
              LogError << "[VK] M2 feed sizes: tf=" << tf.size() << " inter=" << inter.size()
                       << " tex=" << texinfo.size() << " blpidx=" << blpidx.size()
                       << " pairs=" << pairs.size() << " cmds=" << cmds.size()
                       << " arenaVerts=" << wr->mdiMirrorVertices().size() / 48 << std::endl;
            }
          }
          if (!tf.empty() && !cmds.empty() && blpidx.size() == tf.size()
              && wr->vkM2State().size() == tf.size())
          {
            // resolve each batch's BLP pair to bindless ids (same cache as the tilesets)
            // [finding 112] FOURTH inline-decode site (see 83, 85, 90): vkResolveBlp BLOCKS on an
            // archive read for a name it has not seen, on the render thread. It is what makes S3
            // jump to 1.9-4.3 ms on the occasional p95-p99 frame. NOT deferred like the WMO batch
            // table: that path keeps last frame's table while it waits, whereas vk_tex here is
            // rebuilt every frame, so a pending name would give -1 and draw the model untextured
            // for a frame. Fixing it needs a per-pair carry-over of the previous id, not a defer.
            auto const& resolve = vkResolveBlp;   // shared resolver defined above
            // [finding 129] static: these were constructed fresh every frame -- pair_ids
            // reallocating, and vk_tex mallocing + zero-filling one ivec4 per M2 instance
            // (~11k here, ~176 KB a frame). Same class as the clutter growth bug (127).
            static std::vector<glm::ivec2> pair_ids;
            pair_ids.clear();
            pair_ids.reserve(pairs.size());
            for (auto const& pr : pairs)
              pair_ids.emplace_back(resolve(pr.first), resolve(pr.second));

            static std::vector<glm::ivec4> vk_tex;
            // [finding 131] resize, not assign: every element is written below, so the
            // zero-fill was pure waste. And the loop is ~11k independent iterations with no
            // shared state -- 0.70 ms on one thread of 24.
            vk_tex.resize(texinfo.size());
            auto const build_tex = [&](std::size_t i)
            {
              int const pi = (i < blpidx.size() && blpidx[i] >= 0 && blpidx[i] < static_cast<int>(pair_ids.size()))
                               ? blpidx[i] : -1;
              vk_tex[i] = glm::ivec4(pi >= 0 ? pair_ids[pi].x : -1,
                                     pi >= 0 ? pair_ids[pi].y : -1,
                                     texinfo[i].z, texinfo[i].w); // bone base / count unchanged
            };
            // [finding 132] The pool now claims work in CHUNKS, so a fine-grained loop is viable
            // (it was not under the old per-item dispatch -- finding 131).
            if (auto* tp = noggit::render_pool(); tp && texinfo.size() >= 2048u)
              tp->parallel_for(texinfo.size(), build_tex, 2);
            else
              for (std::size_t i = 0; i < texinfo.size(); ++i) build_tex(i);
            static std::vector<Noggit::Rendering::VK::VulkanBackend::M2Draw> vk_draws;   // finding 131
            vk_draws.clear();
            vk_draws.reserve(cmds.size());
            for (auto const& c : cmds)
            {
              Noggit::Rendering::VK::VulkanBackend::M2Draw d;
              d.index_count = c.count;
              d.instance_count = c.instanceCount;
              d.first_index = c.firstIndex;
              d.vertex_offset = static_cast<std::int32_t>(c.baseVertex);
              d.first_instance = c.baseInstance;
              vk_draws.push_back(d);
            }
            // glm::mat4 indexing yields vec4&, so the matrix streams need a flat float view
            s_vk.backend.setM2Frame(reinterpret_cast<float const*>(tf.data()),
                                    reinterpret_cast<float const*>(inter.data()),
                                    reinterpret_cast<std::int32_t const*>(vk_tex.data()),
                                    reinterpret_cast<std::int32_t const*>(wr->vkM2State().data()),
                                    wr->vkM2Groups().empty()
                                      ? nullptr
                                      : reinterpret_cast<std::int32_t const*>(wr->vkM2Groups().data()),
                                    wr->vkM2Groups().size(),
                                    wr->batchSliceDist(),
                                    tf.size(),
                                    wr->vkM2Bones().empty() ? nullptr
                                                             : reinterpret_cast<float const*>(wr->vkM2Bones().data()),
                                    wr->vkM2Bones().size(), vk_draws.data(), vk_draws.size());
            vkLastM2Draws()     = vk_draws.size();
            vkLastM2Instances() = tf.size();
            vkLastM2TexPairs()  = pairs.size();
            vkLastM2Bones()     = wr->vkM2Bones().size();
            {
              std::size_t unres = 0;
              for (auto const& pi : pair_ids) if (pi.x < 0) ++unres;   // blp1 is legitimately -1 for single-texture passes
              vkLastM2UnresolvedPairs() = unres;
            }
            static int s_m2_log = 0;
            if (s_m2_log++ % 300 == 0)
            {
              LogError << "[VK] M2 frame: " << vk_draws.size() << " draws / " << tf.size()
                       << " instances / " << wr->vkM2Bones().size() << " bones / "
                       << pairs.size() << " tex pairs" << std::endl;
            }
          }
          else
          {
            // STALE-OBJECT GUARD: setM2Frame is the ONLY thing that updates VK's instance streams,
            // so skipping it on an empty or desynced feed left Vulkan re-drawing the PREVIOUS frame's
            // doodads -- objects from wherever the camera used to be, rendered into the new view.
            // Draw nothing instead: a missing object is a visible, findable bug; a stale one is not.
            s_vk.backend.setM2Frame(nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0.f, 0,
                                    nullptr, 0, nullptr, 0);
            // [2026-09-04 DIAG] This branch draws NO M2 AT ALL -- every doodad vanishes for the
            // frame. It was silent, so a DESYNCED feed looked identical to an empty one.
            static int s_stale_log = 0;
            if ((s_stale_log++ % 120) == 0)
            {
              LogError << "[VK] M2 STALE-GUARD hit (no doodads this frame): tf=" << tf.size()
                       << " cmds=" << cmds.size()
                       << " blpidx=" << blpidx.size()
                       << " state=" << wr->vkM2State().size()
                       << " -- needs tf!=0, cmds!=0, blpidx==tf, state==tf" << std::endl;
            }
          }
        }
        // [phase A HARD SYNC] GL->VK: before VK overwrites the shared images, make sure every GL command
        // that READ them last frame (compose quad, readbacks) has completed on the GPU.
        static GLsync s_gl_read_fence = nullptr;
        if (s_gl_read_fence)
        {
          if (auto* xf = QOpenGLContext::currentContext() ? QOpenGLContext::currentContext()->extraFunctions() : nullptr)
          {
            auto const t_sync0 = std::chrono::steady_clock::now();
            xf->glClientWaitSync(s_gl_read_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); // <=1 s
            vk_stat_gl_sync_ms() += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t_sync0).count();
            xf->glDeleteSync(s_gl_read_fence);
          }
          s_gl_read_fence = nullptr;
        }
        {
          auto const t_prep_end = std::chrono::steady_clock::now();
          using ms = std::chrono::duration<double, std::milli>;
          t_after_prep = t_prep_end;
          vk_stat_prep_frames() += 1.0;
          vk_stat_prep_ms()  += ms(t_prep_end - t_prep0).count();
          vk_stat_prep1_ms() += ms(t_prepA - t_prep0).count();
          vk_stat_prep2_ms() += ms(t_prepB - t_prepA).count();
          vk_stat_prep3_ms() += ms(t_prep_end - t_prepB).count();
          vk_stat_s2sky_ms()  += ms(t_s2a - t_prepA).count();
          vk_stat_s2cel_ms()  += ms(t_s2b - t_s2a).count();
          vk_stat_s2part_ms() += ms(t_s2c - t_s2b).count();
          vk_stat_s2wmo_ms()  += ms(t_prepB - t_s2c).count();
          vk_stat_skdome_ms()  += ms(t_sk1 - t_prepA).count();
          vk_stat_skcmesh_ms() += ms(t_sk2 - t_sk1).count();
          vk_stat_skctex_ms()  += ms(t_sk3 - t_sk2).count();
        }
        // [2026-09-04 NATIVE UI COMPOSITE] Upload the overlay Qt painted for us. The GRAB happens
        // on a timer (grabVkUiOverlay) because QWidget::render() must never run inside paintGL --
        // it re-enters Qt's painting machinery and one overlay widget lazily constructs the status
        // bar, which reparents a widget mid-paint and throws. Here we only push bytes to the GPU.
        if (s_vk.native && _vk_ui_image_dirty && !_vk_ui_image.isNull())
        {
          s_vk.backend.setUiOverlay(_vk_ui_image.constBits(),
                                    static_cast<std::uint32_t>(_vk_ui_image.width()),
                                    static_cast<std::uint32_t>(_vk_ui_image.height()));
          _vk_ui_image_dirty = false;
        }
        if (s_vk.backend.renderFrame(vk_t, !s_vk.first_frame && !s_vk.native, &vk_mvp[0][0]))
        {
          // [phase H] renderFrame now hands the frame to the SUBMIT THREAD and returns. Everything
          // between there and here overlaps with the GPU; this is the point where GL is about to
          // touch the shared images, so this is where the wait belongs.
          auto const t_compose0 = std::chrono::steady_clock::now();
          GLenum const layout = GL_LAYOUT_GENERAL_EXT_;
          // [VULKAN phase A] wait on BOTH imported images when depth is composed (layouts must match the VK
          // render pass finalLayouts: colour GENERAL, depth DEPTH_STENCIL_ATTACHMENT). Same pairing as
          // before -- exactly one wait and one signal per frame, whichever path runs.
          constexpr GLenum GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT_ = 0x958F;
          GLuint const wait_texs[2] = { s_vk.tex, s_vk.depth_tex };
          GLenum const wait_layouts[2] = { GL_LAYOUT_GENERAL_EXT_, GL_LAYOUT_GENERAL_EXT_ }; // both colour images (R32F z)
          (void)GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT_;
          // [NATIVE PRESENT] no GL interop: the present consumes the frame semaphore, GL reads no
          // shared image, and the walk below overlaps the GPU instead of waiting on it here.
          // Submit failures surface at the next renderFrame (it checks the submit result).
          if (!s_vk.native)
          {
          if (!s_vk.backend.waitFrameComplete())
          {
            LogError << "[VK] frame submission failed -- backend going inert" << std::endl;
            s_vk.compose_ok = false;
          }
          auto const t_sem0 = std::chrono::steady_clock::now();
          s_vk.pWaitSemaphore(s_vk.sem_vk_done, 0, nullptr, s_vk.compose_ok ? 2 : 1, wait_texs, wait_layouts);
          vk_stat_sem_ms() += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t_sem0).count();
          }

          // [VULKAN phase A] compose hook: WorldRender::draw calls this right after clearing the scene
          // target -> VK colour + depth become the base layer, then the GL passes depth-test over it.
          // [phase A, 2026-08-29 decision] compose ONLY carries passes VK OWNS. Composing a full VK terrain
          // under a full GL scene can only produce depth ties (stipple, MSAA streaks, blotches -- three
          // rounds proved it), so until a pass is gated off in GL, VK renders underneath but writes
          // nothing. First owner: terrain (phase B) -> vk_owned_passes becomes non-zero there.
          // [phase B] terrain is owned by VK once the textured neighbourhood is uploaded: the GL terrain
          // pass is gated off (WorldRender::vk_owns_terrain) and the compose hook lays VK's terrain
          // colour+depth under the remaining GL passes.
          // [NATIVE PRESENT] VK presents directly: GL covers nothing, so VK owns every pass
          // unconditionally (the readiness gates protected the COMPOSE path, where GL still could).
          unsigned const s_vk_owned_passes = ((s_vk.native || (s_vk.compose_ok && s_vk.backend.terrainTextured())) && !vk_diff::enabled()) ? 1u : 0u;
          s_vk_backend_tt_ready() = s_vk.backend.terrainTextured();
          // [2026-09-02] Was `(s_vk_owned_passes != 0u)` with no readiness check -- the same bug
          // finding 161 fixed for water. VK claimed terrain (gating GL off) on frames where it had
          // no terrain geometry to draw, leaving the clear colour: "everything flashes blue and all
          // the doodads go missing". Log evidence: terrain=0 ms with ownsTerrain=1, every frame.
          // [2026-09-04] The readiness checks are BACK. Native claimed every pass unconditionally
          // (`s_vk.native || ...`), which re-broke finding 161: a pass owned on a frame VK has no
          // content for renders NOTHING, and in native GL's copy is invisible under the swapchain.
          _world->renderer()->vk_owns_terrain =
            (s_vk_owned_passes != 0u) && s_vk.backend.terrainReady();
          // [phase I] M2 and WATER were never gated, so GL kept drawing every model and every water
          // surface that VK had ALREADY drawn -- the whole scene twice, plus a full CPU sync. Gate
          // them on the same condition as terrain, and (for M2) per bucket, so anything VK rejected
          // still comes from GL.
          _world->renderer()->vk_owns_m2 =
            (s_vk_owned_passes != 0u) && s_vk.backend.m2Available();
          // [finding 161] WATER FALLBACK BUG. This used to be `(owned_passes != 0) &&
          // _draw_water.get()` -- it suppressed GL's water the moment VK owned terrain, WITHOUT ever
          // checking that VK actually has water to draw. M2 right above gets this right
          // (`&& m2Available()`); water never did. So on any map where VK's water derive produces
          // nothing, the water simply vanishes: VK draws none because its index count is 0, and GL
          // is gated off anyway. That is the "no water at all in Vulkan" report. Now VK only claims
          // water when it genuinely has a water mesh; otherwise GL keeps drawing it.
          _world->renderer()->vk_owns_water =
            (s_vk_owned_passes != 0u) && _draw_water.get() && s_vk.backend.waterReady();
          // [2026-09-04 NATIVE HOLE REPORT] In native, anything VK does not draw is INVISIBLE (GL
          // renders under the covered widget). Say out loud, every 60 frames, what VK is failing to
          // own and how much content is being dropped -- these numbers ARE the missing pixels the
          // user sees, and they were previously only inferable.
          if (s_vk.native)
          {
            static int s_hole_frames = 0;
            static int s_snap_idx = 0;
            if (++s_hole_frames >= 60)
            {
              s_hole_frames = 0;
              auto* wrh = _world->renderer();
              bool const water_hole = _draw_water.get() && !s_vk.backend.waterReady();
              LogError << "[VK] NATIVE HOLES: terrain=" << (wrh->vk_owns_terrain ? "vk" : "NONE")
                       << " m2=" << (wrh->vk_owns_m2 ? "vk" : "NONE")
                       << " water=" << (water_hole ? "NONE(no vk mesh)"
                                                   : (wrh->vk_owns_water ? "vk" : "off"))
                       << " | classicFedInstances/frame=" << wrh->classicIssued()
                       << " | cam=(" << _camera.position.x << "," << _camera.position.y << ","
                       << _camera.position.z << ") yaw=" << _camera.yaw()._
                       << " pitch=" << _camera.pitch()._
                       << " present=" << s_vk.backend.presentWidth() << "x"
                       << s_vk.backend.presentHeight()
                       << " | M2 draws=" << vkLastM2Draws()
                       << " inst=" << vkLastM2Instances()
                       << " texpairs=" << vkLastM2TexPairs()
                       << " unresolvedPairs=" << vkLastM2UnresolvedPairs()
                       << " bones=" << vkLastM2Bones()
                       << " slice=" << wrh->batchSliceDist() << std::endl;
              // [2026-09-04 SELF-DIAGNOSTIC] Dump the ACTUAL presented swapchain (what the window
              // shows) alongside each hole report -- first 6, rotating, so a SHORT session captures
              // the user's real frame at whatever camera they fly to. Their log says trees ARE drawn
              // (allHidden=0, 508 draws) while they report no change: only their own pixels resolve
              // that. Fires ~once/second, capped so it can't spam the disk.
              // [2026-09-06] 3 snaps, 300 frames apart: a 2288x1307 PNG encode on the main thread is
              // ~hundreds of ms, and six of them inside the first seconds stalled exactly the window
              // in which models stream in. Still enough to see the user's frame.
              // This block runs once per 60-frame report, so the gap is counted in TICKS:
              // 5 ticks = 300 frames between snaps. (It was 300 ticks = 18,000 frames -- snap #1
              // never fired and the probe below never armed.)
              static int s_snap_gap = 5;
              // Arm the M2 footprint probe one tick BEFORE snap #1 fires, so that snap shows
              // magenta wherever an M2 fragment rasterised -- disarmed right after the capture.
              // gap==3 here: the check runs BEFORE the ++ below, and the tick that sees gap==4
              // increments to 5 and fires the snap in the same breath -- arming there gave the
              // probe zero rendered frames. Arming at 3 leaves it on for the full 60-frame tick.
              if (s_snap_idx == 1 && s_snap_gap == 3)
                s_vk.backend.setM2FootprintProbe(true);
              if (s_snap_idx < 3 && ++s_snap_gap >= 5 && (s_snap_gap = 0, true))
              {
                std::vector<std::uint8_t> snap_px;
                if (s_vk.backend.presentedReadbackAvailable()
                    && s_vk.backend.readbackPresented(snap_px))
                {
                  std::string const snap_path = "native_snap_" + std::to_string(s_snap_idx) + ".png";
                  vk_diff::savePng(snap_path, snap_px.data(),
                                   static_cast<int>(s_vk.backend.presentWidth()),
                                   static_cast<int>(s_vk.backend.presentHeight()));
                  LogError << "[VK] native snap -> " << snap_path
                           << " cam=(" << _camera.position.x << "," << _camera.position.y << ","
                           << _camera.position.z << ") present="
                           << s_vk.backend.presentWidth() << "x" << s_vk.backend.presentHeight()
                           << std::endl;
                  ++s_snap_idx;
                  s_vk.backend.setM2FootprintProbe(false);
                }
              }
            }
          }
          {
            // Say out loud which GL passes are actually gated off, and what a frame costs. "It feels
            // slow" is not measurable; this is. Both numbers are what tell you whether VK mode is
            // doing the scene ONCE or twice.
            static auto s_last = std::chrono::steady_clock::now();
            static double s_acc = 0.0;
            static int s_frames = 0;
            auto const now_f = std::chrono::steady_clock::now();
            s_acc += std::chrono::duration<double, std::milli>(now_f - s_last).count();
            s_last = now_f;
            if (++s_frames >= 300)
            {
              {
                extern double g_vk_sec_terrain_ms, g_vk_sec_wmo_ms, g_vk_sec_m2_ms;
                extern double g_vk_feedbuild_ms;
                extern double g_vk_sec_gather_ms, g_vk_sec_ddraw_ms, g_vk_sec_indiv_ms,
                              g_vk_sec_clutter_ms, g_vk_sec_submit_ms;
                LogError << "[VK] M2 sub/frame: gather=" << (g_vk_sec_gather_ms / s_frames)
                         << " doodadDraw=" << (g_vk_sec_ddraw_ms / s_frames)
                         << " indiv=" << (g_vk_sec_indiv_ms / s_frames)
                         << " clutter=" << (g_vk_sec_clutter_ms / s_frames)
                         << " mdiSubmit=" << (g_vk_sec_submit_ms / s_frames) << " ms" << std::endl;
                g_vk_sec_gather_ms = g_vk_sec_ddraw_ms = g_vk_sec_indiv_ms = 0.0;
                g_vk_sec_clutter_ms = g_vk_sec_submit_ms = 0.0;
                LogError << "[VK] vk detail/frame: cull=" << (vk_stat_cull_ms() / s_frames)
                       << " m2feed=" << (vk_stat_m2up_ms() / s_frames)
                       << " composeWait=" << (vk_stat_compose_ms() / s_frames)
                       << " composePass=" << (vk_stat_composepass_ms() / s_frames)
                       << " PREP=" << (vk_stat_prep_ms() / s_frames)
                       << " POST=" << (vk_stat_post_ms() / s_frames) << " ms" << std::endl;
              LogError << "[VK] PREP split: S1(tiles/terrain/water/part)=" << (vk_stat_prep1_ms() / s_frames)
                       << " S2(light/sky/cloud/celest)=" << (vk_stat_prep2_ms() / s_frames)
                       << " S3(wmoFrame+M2 feed)=" << (vk_stat_prep3_ms() / s_frames) << " ms" << std::endl;
              LogError << "[VK] S2 split: sky=" << (vk_stat_s2sky_ms() / s_frames)
                       << " celest=" << (vk_stat_s2cel_ms() / s_frames)
                       << " particles=" << (vk_stat_s2part_ms() / s_frames)
                       << " wmoArena=" << (vk_stat_s2wmo_ms() / s_frames) << " ms" << std::endl;
              LogError << "[VK] SKY split: domeUpload=" << (vk_stat_skdome_ms() / s_frames)
                       << " cloudMesh=" << (vk_stat_skcmesh_ms() / s_frames)
                       << " cloudTex=" << (vk_stat_skctex_ms() / s_frames) << " ms" << std::endl;
              vk_stat_skdome_ms() = 0.0; vk_stat_skcmesh_ms() = 0.0; vk_stat_skctex_ms() = 0.0;
              vk_stat_s2sky_ms() = 0.0; vk_stat_s2cel_ms() = 0.0;
              vk_stat_s2part_ms() = 0.0; vk_stat_s2wmo_ms() = 0.0;
              vk_stat_prep_ms() = 0.0; vk_stat_post_ms() = 0.0;
              vk_stat_prep1_ms() = 0.0; vk_stat_prep2_ms() = 0.0; vk_stat_prep3_ms() = 0.0;
              vk_stat_cull_ms() = 0.0; vk_stat_m2up_ms() = 0.0; vk_stat_compose_ms() = 0.0;
              vk_stat_composepass_ms() = 0.0;
              LogError << "[VK] vk block (feeds+upload+submit+compose): "
                       << (vk_stat_vkblock_ms() / s_frames) << " ms/frame" << std::endl;
              vk_stat_vkblock_ms() = 0.0;
              LogError << "[VK] feed BUILD inside the scene pass: "
                       << (g_vk_feedbuild_ms / s_frames) << " ms/frame" << std::endl;
              g_vk_feedbuild_ms = 0.0;
              LogError << "[VK] GL scene sections/frame: terrain=" << (g_vk_sec_terrain_ms / s_frames)
                         << " wmo=" << (g_vk_sec_wmo_ms / s_frames)
                         << " m2=" << (g_vk_sec_m2_ms / s_frames) << " ms" << std::endl;
                g_vk_sec_terrain_ms = g_vk_sec_wmo_ms = g_vk_sec_m2_ms = 0.0;
              }
              LogError << "[VK] GL scene pass (traversal + ungated draws): "
                       << (vk_stat_glscene_ms() / s_frames) << " ms/frame" << std::endl;
              vk_stat_glscene_ms() = 0.0;
              LogError << "[VK] feed cost/frame: tileRepack=" << (vk_stat_tile_ms() / s_frames)
                       << " ms  wmoLiquid=" << (vk_stat_wliq_ms() / s_frames)
                       << " ms  m2=" << (vk_stat_m2_ms() / s_frames)
                       << " ms  particles=" << (vk_stat_part_ms() / s_frames)
                       << " ms" << std::endl;
              vk_stat_tile_ms() = 0.0; vk_stat_wliq_ms() = 0.0;
              vk_stat_m2_ms() = 0.0;   vk_stat_part_ms() = 0.0;
              LogError << "[VK] interop cost/frame: glClientWaitSync="
                       << (vk_stat_gl_sync_ms() / s_frames) << " ms  glWaitSemaphore="
                       << (vk_stat_sem_ms() / s_frames) << " ms" << std::endl;
              vk_stat_gl_sync_ms() = 0.0;
              vk_stat_sem_ms() = 0.0;
              LogError << "[VK] frame " << (s_acc / s_frames) << " ms mean over " << s_frames
                       << " | GL gated off: terrain=" << (_world->renderer()->vk_owns_terrain ? 1 : 0)
                       << " m2=" << (_world->renderer()->vk_owns_m2 ? 1 : 0)
                       << " water=" << (_world->renderer()->vk_owns_water ? 1 : 0)
                       << " | GL-only M2 instances still drawn=" << _world->renderer()->classicIssued()
                       << std::endl;
              s_acc = 0.0;
              s_frames = 0;
            }
          }
          // [2026-09-08 WDL HORIZON] VK owns the terrain here, so WorldRender::draw hands the per-frame
          // low-res horizon (client CMapLowDetail tile selection + MAHO split) to the backend instead of
          // drawing it with GL. Captureless on purpose: s_vk is a function-local static.
          _world->renderer()->vk_horizon_feed =
            [](Noggit::map_horizon::render const* r, glm::vec3 const& c, glm::mat4x4 const& ld_mvp)
            {
              if (!r || r->vertices().empty()
                  || (r->solid_indices().empty() && r->hole_indices().empty()))
              {
                s_vk.backend.setHorizon(nullptr, 0, 0, nullptr, 0, nullptr, 0, nullptr, nullptr);
                return;
              }
              float const rgb[3] = { c.x, c.y, c.z };
              s_vk.backend.setHorizon(&r->vertices()[0].x, r->vertices().size(), r->vertex_generation(),
                                      r->solid_indices().data(), r->solid_indices().size(),
                                      r->hole_indices().data(), r->hole_indices().size(), rgb,
                                      &ld_mvp[0][0]);
            };
          if (s_vk.compose_ok && s_vk_owned_passes != 0u)
          {
            _world->renderer()->pre_scene_compose = [&]()
            {
              VkPhaseTimer _t_composepass(vk_stat_composepass_ms());
              GLint prev_depth_func = GL_LEQUAL, prev_prog = 0, prev_vao = 0;
              GLboolean prev_depth_test = GL_FALSE, prev_blend = GL_FALSE, prev_depth_mask = GL_TRUE;
              gl.getIntegerv(GL_DEPTH_FUNC, &prev_depth_func);
              gl.getIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
              gl.getIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
              gl.getBooleanv(GL_DEPTH_TEST, &prev_depth_test);
              gl.getBooleanv(GL_BLEND, &prev_blend);
              gl.getBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);

              gl.enable(GL_DEPTH_TEST);
              gl.depthFunc(GL_ALWAYS);
              gl.depthMask(GL_TRUE);
              gl.disable(GL_BLEND);
              {
                OpenGL::Scoped::use_program p{*s_vk.compose_program};
                gl.activeTexture(GL_TEXTURE0);
                gl.bindTexture(GL_TEXTURE_2D, s_vk.tex);
                gl.activeTexture(GL_TEXTURE1);
                gl.bindTexture(GL_TEXTURE_2D, s_vk.depth_tex);
                p.uniform("vk_color", 0);
                p.uniform("vk_depth", 1);
                p.uniform("near_z", 0.25f); // MapView::projection() near
                p.uniform("far_z", _settings->value("farZ", 900).toFloat());
                p.uniform("max_view_z", _settings->value("view_distance", 900.f).toFloat());
                // When VK owns the dome, GL draws no sky -- so the compose must keep VK's far-plane
                // pixels instead of discarding them (see the shader).
                p.uniform("vk_owns_sky", _world->renderer()->skies() && _world->renderer()->skies()->vkOwnsDome() ? 1 : 0);
                gl.bindVertexArray(s_vk.compose_vao);
                gl.drawArraysInstanced(GL_TRIANGLES, 0, 3, 1); // (the wrapper has no plain drawArrays)
                gl.bindTexture(GL_TEXTURE_2D, 0);
                gl.activeTexture(GL_TEXTURE0);
                gl.bindTexture(GL_TEXTURE_2D, 0);
              }
              gl.bindVertexArray(static_cast<GLuint>(prev_vao));
              gl.useProgram(static_cast<GLuint>(prev_prog));
              gl.depthFunc(static_cast<GLenum>(prev_depth_func));
              gl.depthMask(prev_depth_mask);
              if (!prev_depth_test) gl.disable(GL_DEPTH_TEST);
              if (prev_blend) gl.enable(GL_BLEND);
            };
          }
          else
          {
            _world->renderer()->pre_scene_compose = nullptr;
          }
          vk_stat_compose_ms() += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t_compose0).count();
          // GL scene now (after the wait, before the readback/blit/signal below)
          {
            auto const t_mid_end = std::chrono::steady_clock::now();
            vk_stat_mid_ms() += std::chrono::duration<double, std::milli>(t_mid_end - t_after_prep).count();
          }
          draw_gl_scene();
          // everything from here to the end of the block: readback / blit / signal / fence
          VkPhaseTimer _t_tail_all(vk_stat_tail_ms());
          VkPhaseTimer _t_post(vk_stat_post_ms());

          GLint prev_read = 0, prev_draw = 0, vp[4] = {0, 0, 0, 0};
          // glGet* can force a driver round-trip; only the probe/blit paths below read these, and
          // both are gated off in a normal frame.
          static bool const s_post_needs_fbo = std::getenv("NOGGIT_VK_PROBE") != nullptr;
          if (s_post_needs_fbo || s_vk_full_view || vk_diff::enabled())
          {
            gl.getIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
            gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
          }
          gl.getIntegerv(GL_VIEWPORT, vp);

          // [VULKAN phase A DIAG] every ~240 frames dump the IMPORTED depth texture (what the compose quad
          // actually reads) as vk_diff/depth_probe.png + stats -> proves whether the D32 import is a
          // real depth field (smooth, 1.0 background) or tiling/compression garbage (the stipple).
          static bool const s_vk_probe = std::getenv("NOGGIT_VK_PROBE") != nullptr; // dev-only: 3 full-res readbacks + PNGs = 1.6 s spikes
          // one automatic dump ~5 s after the textured terrain first becomes ready (diagnosis without env vars)
          static int s_probe_once = 0;
          bool const probe_once = s_vk.backend.terrainTextured() && s_probe_once < 300 && (++s_probe_once == 300);
          if (s_vk.compose_ok && (s_vk_probe || probe_once))
          {
            static int s_probe_frames = 0;
            if (probe_once || (++s_probe_frames % 60) == 1)
            {
              // VK's own colour output (what the compose quad reads) -> vk_diff/color_probe.png
              {
                std::vector<std::uint8_t> rgba(static_cast<std::size_t>(VK_W) * VK_H * 4u);
                GLint prev_pack = 4;
                gl.getIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                gl.bindTexture(GL_TEXTURE_2D, s_vk.tex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                gl.bindTexture(GL_TEXTURE_2D, 0);
                glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
                vk_diff::savePng(vk_diff::outDir() + "/color_probe.png", rgba.data(), VK_W, VK_H);
                // and the GL frame as composited so far (default/current read framebuffer)
                std::vector<std::uint8_t> glpx(static_cast<std::size_t>(VK_W) * VK_H * 4u);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                gl.readPixels(vp[0], vp[1], VK_W, VK_H, GL_RGBA, GL_UNSIGNED_BYTE, glpx.data());
                glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
                vk_diff::savePng(vk_diff::outDir() + "/frame_probe.png", glpx.data(), VK_W, VK_H);
              }
              std::vector<float> depth(static_cast<std::size_t>(VK_W) * VK_H);
              gl.bindTexture(GL_TEXTURE_2D, s_vk.depth_tex);
              glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, depth.data()); // R32F depth-as-colour
              gl.bindTexture(GL_TEXTURE_2D, 0);
              std::size_t ones = 0, zeros = 0, nans = 0; float mn = 2.f, mx = -1.f;
              for (float v : depth)
              {
                if (v != v) { ++nans; continue; }
                if (v >= 1.f) ++ones; else if (v <= 0.f) ++zeros;
                mn = std::min(mn, v); mx = std::max(mx, v);
              }
              // neighbour roughness: mean |d(x)-d(x+1)| over interior pixels (garbage -> huge, real -> tiny)
              double rough = 0.0; std::size_t nr = 0;
              for (std::uint32_t y = VK_H / 4; y < VK_H * 3 / 4; y += 7)
                for (std::uint32_t x = 1; x + 1 < VK_W; ++x)
                {
                  float const a = depth[y * VK_W + x], b = depth[y * VK_W + x + 1];
                  if (a < 1.f && b < 1.f) { rough += std::fabs(a - b); ++nr; }
                }
              std::vector<std::uint8_t> gray(depth.size() * 4);
              for (std::size_t i = 0; i < depth.size(); ++i)
              {
                float const v = depth[i];
                // stretch the useful range: window depth is ~0.99x for most of the scene
                std::uint8_t const g = (v >= 1.f || v != v) ? 0 : static_cast<std::uint8_t>(std::min(255.f, std::max(0.f, (v - 0.98f) / 0.02f * 255.f)));
                gray[i * 4] = gray[i * 4 + 1] = gray[i * 4 + 2] = g; gray[i * 4 + 3] = 255;
              }
              vk_diff::savePng(vk_diff::outDir() + "/depth_probe.png", gray.data(), VK_W, VK_H);
              LogError << "[VK] depth probe: " << VK_W << "x" << VK_H << " min=" << mn << " max=" << mx
                       << " bg(=1)=" << (100.0 * ones / depth.size()) << "% zeros=" << zeros << " nan=" << nans
                       << " roughness=" << (nr ? rough / nr : -1.0) << " -> vk_diff/depth_probe.png" << std::endl;
            }
          }

          // [VK-DIFF] read both images BEFORE the preview blit overwrites the GL scene corner. GL scene =
          // current read framebuffer (bottom-up rows); VK image = the imported texture (row 0 is what the
          // un-flipped blit puts at GL y=0, i.e. the same orientation) -> compare directly.
          if (vk_diff::enabled())
          {
            static int s_diff_frames = 0;
            static bool s_diff_size_warned = false;
            static std::vector<std::uint8_t> s_gl_px, s_vk_px, s_diff_px;
            ++s_diff_frames;
            bool const want = diff_capture_now || (s_diff_frames % 60) == 0;
            if (want && (static_cast<std::uint32_t>(vp[2]) != VK_W || static_cast<std::uint32_t>(vp[3]) != VK_H))
            {
              if (!s_diff_size_warned)
              {
                LogError << "[VK-DIFF] viewport " << vp[2] << "x" << vp[3] << " != VK image " << VK_W << "x" << VK_H
                         << " (window resized after VK init) -- comparison skipped; relaunch at the final size"
                         << std::endl;
                s_diff_size_warned = true;
              }
            }
            else if (want)
            {
              std::size_t const bytes = static_cast<std::size_t>(VK_W) * VK_H * 4u;
              s_gl_px.resize(bytes);
              s_vk_px.resize(bytes);
              GLint prev_pack = 4;
              gl.getIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
              glPixelStorei(GL_PACK_ALIGNMENT, 1); // GL1.x entry points (raw, like glFinish below)
              gl.readPixels(vp[0], vp[1], VK_W, VK_H, GL_RGBA, GL_UNSIGNED_BYTE, s_gl_px.data());
              gl.bindTexture(GL_TEXTURE_2D, s_vk.tex);
              glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_vk_px.data());
              glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);

              double const tol = vk_diff::envf("NOGGIT_VK_DIFF_TOL", 0.5); // AF excluded -> a real port diff shows well under this
              static std::vector<float> s_vk_z;
              float const* zmask = nullptr;
              // NOGGIT_PARITY_SKY: the sky IS the subject, so keep its pixels in the metric. The
              // depth is still READ -- the classification image and the vkMissing/vkExtra accounting
              // are built from it, and skipping the readback blinded exactly the diagnostic that
              // tells a missing object from a wrong-coloured one.
              static bool const s_parity_sky_metric = std::getenv("NOGGIT_PARITY_SKY") != nullptr;
              if (s_vk.depth_tex)
              {
                s_vk_z.resize(static_cast<std::size_t>(VK_W) * VK_H);
                gl.bindTexture(GL_TEXTURE_2D, s_vk.depth_tex);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, s_vk_z.data());
                gl.bindTexture(GL_TEXTURE_2D, 0);
                zmask = s_parity_sky_metric ? nullptr : s_vk_z.data();
              }
              if (!s_vk_z.empty())
              {
                // GL only draws terrain within its view distance; VK draws its whole 3x3 ring -> mask
                // everything farther than the view distance (window depth of that distance)
                float const nz = 0.25f, fz = _settings->value("farZ", 900).toFloat();
                float const vd = std::min(fz, _settings->value("view_distance", 900.f).toFloat());
                float const dmax = (fz / (fz - nz)) * (1.f - nz / vd);
                for (float& z : s_vk_z) if (z >= dmax) z = 1.f;
              }
              vk_diff::Stats const st = vk_diff::compare(s_gl_px.data(), s_vk_px.data(), VK_W, VK_H,
                                                         diff_capture_now ? &s_diff_px : nullptr, 8, zmask);
              bool const pass = st.mean <= tol && st.pct_off <= 1.0;
              if (diff_capture_now && !s_vk_z.empty())
              {
                // GEOMETRY probe: compare GL's depth buffer with VK's -> silhouette/coverage mismatch
                // shows up here without any shading involved.
                std::vector<float> gl_z(static_cast<std::size_t>(VK_W) * VK_H);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                gl.readPixels(vp[0], vp[1], VK_W, VK_H, GL_DEPTH_COMPONENT, GL_FLOAT, gl_z.data());
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                std::size_t both = 0, only_gl = 0, only_vk = 0, none = 0;
                double zsum = 0.0;
                for (std::size_t i = 0; i < gl_z.size(); ++i)
                {
                  bool const g = gl_z[i] < 0.9999f, v = s_vk_z[i] < 1.f;
                  if (g && v) { ++both; zsum += std::fabs(gl_z[i] - s_vk_z[i]); }
                  else if (g) ++only_gl; else if (v) ++only_vk; else ++none;
                }
                LogError << "[VK-DIFF] geom " << diff_capture_name << ": both=" << (100.0 * both / gl_z.size())
                         << "% onlyGL=" << (100.0 * only_gl / gl_z.size()) << "% onlyVK=" << (100.0 * only_vk / gl_z.size())
                         << "% meanDepthDelta=" << (both ? zsum / both : 0.0) << std::endl;

                // CLASSIFICATION image: which pixels are compared, and which of those actually differ.
                // black masked | red onlyGL (VK missing) | blue onlyVK (VK extra) | green agree | white differ
                std::vector<std::uint8_t> cls(gl_z.size() * 4u, 255u);
                std::size_t both_bad = 0;
                for (std::size_t i = 0; i < gl_z.size(); ++i)
                {
                  bool const g = gl_z[i] < 0.9999f, v = s_vk_z[i] < 1.f;
                  std::uint8_t r = 0, gg = 0, b = 0;
                  if (!v) { r = gg = b = 0; }                       // masked out of the diff entirely
                  else if (!g) { b = 255; }                          // VK drew, GL did not
                  else
                  {
                    int e = 0;
                    for (int c = 0; c < 3; ++c)
                      e = std::max(e, std::abs(int(s_gl_px[i * 4 + c]) - int(s_vk_px[i * 4 + c])));
                    if (e > 8) { r = gg = b = 255; ++both_bad; } else { gg = 255; }
                  }
                  cls[i * 4] = r; cls[i * 4 + 1] = gg; cls[i * 4 + 2] = b; cls[i * 4 + 3] = 255;
                }
                // pixels GL drew but VK did not are masked away by the depth gate, so count them here
                std::size_t onlygl_shown = 0;
                for (std::size_t i = 0; i < gl_z.size(); ++i)
                  if (gl_z[i] < 0.9999f && !(s_vk_z[i] < 1.f)) { cls[i * 4] = 255; cls[i * 4 + 1] = 0; cls[i * 4 + 2] = 0; ++onlygl_shown; }
                LogError << "[VK-DIFF] cls " << diff_capture_name
                         << ": bothDiffer=" << (100.0 * both_bad / gl_z.size())
                         << "% vkMissing=" << (100.0 * onlygl_shown / gl_z.size())
                         << "% vkExtra=" << (100.0 * only_vk / gl_z.size()) << "%" << std::endl;
                vk_diff::savePng(vk_diff::outDir() + "/" + diff_capture_name + "_cls.png",
                                 cls.data(), VK_W, VK_H);
              }
              if (diff_capture_now)
              {
                std::string const dir = vk_diff::outDir();
                vk_diff::savePng(dir + "/" + diff_capture_name + "_gl.png", s_gl_px.data(), VK_W, VK_H);
                vk_diff::savePng(dir + "/" + diff_capture_name + "_vk.png", s_vk_px.data(), VK_W, VK_H);
                vk_diff::savePng(dir + "/" + diff_capture_name + "_diff.png", s_diff_px.data(), VK_W, VK_H);
                (pass ? s_diff_pass : s_diff_fail) += 1;
                LogError << "[VK-DIFF] bias " << diff_capture_name << " (VK-GL) r=" << st.bias_r
                         << " g=" << st.bias_g << " b=" << st.bias_b << std::endl;
                LogError << "[VK-DIFF] cam=" << diff_capture_name << " size=" << VK_W << "x" << VK_H
                         << " mean=" << st.mean << " max=" << st.max << " off>8LSB=" << st.pct_off << "%"
                         << " -> " << (pass ? "PASS" : "FAIL") << " (pngs in " << dir << ")" << std::endl;
                if (s_diff_done)
                {
                  LogError << "[VK-DIFF] SUMMARY " << s_diff_pass << " pass / " << s_diff_fail << " fail of "
                           << s_diff_cams.size() << " cameras -> "
                           << (s_diff_fail == 0 ? "PARITY" : "NO PARITY") << std::endl;
                  vk_diff::finished() = true;
                }
              }
              else
              {
                LogError << "[VK-DIFF] live mean=" << st.mean << " max=" << st.max
                         << " off>8LSB=" << st.pct_off << "% " << (pass ? "PASS" : "FAIL") << std::endl;
              }
            }
          }
          // [NATIVE PRESENT] the swapchain already showed the frame -- no GL blit, no GL->VK
          // semaphore signal, no read fence. Everything below is compose-mode interop.
          if (!s_vk.native)
          {
          gl.bindFramebuffer(GL_READ_FRAMEBUFFER, s_vk.fbo);
          // [VK-1c] NOGGIT_VK_FULL: the VK image IS the viewport (the GL world draw is skipped in this mode);
          // else the 192px bottom-right preview square.
          static bool const s_vk_full_blit = vk_diff::vkFull();
          if (s_vk_full_blit)
          {
            gl.blitFramebuffer(0, 0, VK_W, VK_H, 0, 0, vp[2], vp[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
          }
          else if (s_vk.compose_ok)
          {
            // [VULKAN phase A] composed under the scene already -- no corner preview needed
          }
          else
          {
            gl.blitFramebuffer(0, 0, VK_W, VK_H, vp[2] - 200, 8, vp[2] - 8, 200,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
          }
          gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
          gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw));

          s_vk.pSignalSemaphore(s_vk.sem_gl_done, 0, nullptr, s_vk.compose_ok ? 2 : 1, wait_texs, wait_layouts);
          // [phase A HARD SYNC] fence after every GL read of the shared images; waited before the next VK frame
          if (auto* xf = QOpenGLContext::currentContext() ? QOpenGLContext::currentContext()->extraFunctions() : nullptr)
          {
            s_gl_read_fence = xf->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
          }
          }
          s_vk.first_frame = false;
        }
      }
    }
  }
#endif
  // pure GL, VK not ready, or VK submit failed this frame: draw the scene exactly as before
  if (!gl_scene_drawn)
  {
    _world->renderer()->pre_scene_compose = nullptr; // no VK image this frame -> no base layer
    _world->renderer()->vk_horizon_feed = nullptr;    // GL draws the WDL horizon itself again
    _world->renderer()->vk_feeding = false;           // no VK consumer -> skip every feed
    _world->renderer()->vk_owns_terrain = false;      // GL draws its own terrain again
    _world->renderer()->vk_owns_m2 = false;
    _world->renderer()->vk_owns_water = false;
    // [NATIVE PRESENT] a backend gone inert must hand EVERY pass back to GL, not just these three
    _world->renderer()->vk_native = false;
    _world->renderer()->setVkOwnsWmo(false);
    _world->renderer()->setVkOwnsCelestials(false);
    if (_world->renderer()->skies())
      _world->renderer()->skies()->setVkOwnsDome(false);
    vk_stat_prevk_ms() += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t_paint0).count();
    draw_gl_scene();
  }
}

// [VULKAN NATIVE PRESENT, 2026-09-03] Create the native child window the swapchain presents into.
// Runs on the event loop (queued from draw_map's init) -- NEVER inside paintGL: creating widgets
// mid-paint re-enters layout. Input transparency is two-layered: WA_TransparentForMouseEvents on
// the container (Qt-side) and WM_NCHITTEST -> HTTRANSPARENT on the HWND (OS-side).
// [2026-09-04 REVERTED -- caused a FULLY BLACK viewport] The first attempt promoted every direct
// child of the viewport to a native window (WA_NativeWindow) and raised it. That is wrong here:
// `_viewport_overlay_ui` is a FULL-SIZE child covering the whole viewport, so turning it into a
// native window made it an opaque surface painted over the entire scene -> everything black.
//
// Left as a no-op deliberately, so the call sites stay and the reasoning is recorded. The real fix
// must promote ONLY the small control widgets (toolbars/buttons), never a full-size transparent
// overlay -- or avoid the native-child-window approach for presentation altogether.
void MapView::raiseViewportOverlayWidgets()
{
  // [2026-09-04] DISABLED -- this approach cannot work, proven twice on the user's machine.
  //
  // The Vulkan surface is a NATIVE child window, so it composites above ordinary Qt siblings and
  // hides the tool palette. The only way to lift Qt widgets above a native window is to make them
  // native too. Doing that DOES restore the toolbars -- and turns the viewport BLACK, with or
  // without lower() on the container. Giving a QOpenGLWidget native children changes how it
  // composites, and native mode gates off every GL pass, so the widget's empty (black) surface ends
  // up over the swapchain. Both states are broken:
  //     promote   -> toolbars visible, world black
  //     no promote-> world visible, toolbars hidden
  // One HWND cannot be both a Vulkan swapchain target and a Qt-painted widget surface, so this
  // needs a presentation-architecture change, not another patch here. Left as a no-op deliberately.
  //
  // NOTE for whoever revisits: readbackPresented() reads the SWAPCHAIN IMAGE, not what the OS
  // composites on screen -- it reported a perfect frame while the window was black. Any fix here
  // must be verified on the actual window, never by that readback.
}

// [2026-09-04 NATIVE UI COMPOSITE] Paint the editor overlay into a transparent image for Vulkan.
// Runs from a TIMER (event loop), never from paintGL -- see the note on _vk_ui_image.
void MapView::grabVkUiOverlay()
{
  if (!_vk_present_container || _destroying)
    return;

  qreal const dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
  int const uw = static_cast<int>(width() * dpr);
  int const uh = static_cast<int>(height() * dpr);
  if (uw <= 0 || uh <= 0)
    return;

  if (_vk_ui_image.width() != uw || _vk_ui_image.height() != uh)
  {
    _vk_ui_image = QImage(uw, uh, QImage::Format_RGBA8888);  // bytes R,G,B,A == VK RGBA8_UNORM
    _vk_ui_image.setDevicePixelRatio(dpr);
  }
  _vk_ui_image.fill(Qt::transparent);

  bool any = false;
  for (QObject* o : children())
  {
    QWidget* const w = qobject_cast<QWidget*>(o);
    if (!w || w == _vk_present_container || w->isHidden())
      continue;
    w->render(&_vk_ui_image, w->pos(), QRegion(),
              QWidget::DrawWindowBackground | QWidget::DrawChildren);
    any = true;
  }
  _vk_ui_image_dirty = any;
}

void MapView::ensureVkPresentSurface()
{
#ifdef _WIN32
  if (_vk_present_container || _destroying)
    return;
  _vk_present_window = new QWindow();
  _vk_present_window->setFlags(_vk_present_window->flags() | Qt::WindowTransparentForInput);
  _vk_present_window->setSurfaceType(QSurface::VulkanSurface); // no Qt backing store on this HWND
  _vk_present_container = QWidget::createWindowContainer(_vk_present_window, this);
  _vk_present_container->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  _vk_present_container->setFocusPolicy(Qt::NoFocus);
  _vk_present_container->setGeometry(rect());
  _vk_present_container->show();
  static bool s_filter_installed = false;
  if (!s_filter_installed)
  {
    s_filter_installed = true;
    qApp->installNativeEventFilter(&vkPresentHitFilter());
  }
  vkPresentHitFilter().hwnds.insert(reinterpret_cast<void*>(_vk_present_window->winId()));

  // [2026-09-04 UI-COVERED FIX, user report] A window container is a NATIVE child window, and a
  // native child always composites ABOVE non-native Qt siblings regardless of Qt's own stacking --
  // so the present surface hid the viewport-overlay UI (tool palette, top buttons, status widgets).
  // Qt's raise() alone cannot fix that: the sibling has to become native too, then the OS honours
  // the z-order. Push the present surface to the bottom and promote every other direct child.
  // NOTE: do NOT lower() this container. It is a native child window; lowering it puts the
  // swapchain BELOW the QOpenGLWidget's own surface, and in native mode GL draws nothing (every
  // pass is gated off) -- so the screen showed GL's empty black widget composited over a perfectly
  // good swapchain. The offscreen/presented readback cannot see that: it reads the swapchain image
  // directly, so the harness reported a full scene while the window was black. Leave the surface
  // where it is and lift only the small controls above it.
  raiseViewportOverlayWidgets();
  // Drive the overlay grab from the event loop at ~20 Hz. The UI does not change per frame, and
  // rendering the widget tree is CPU work, so this is deliberately not per-frame.
  // [2026-09-06] Overlay grab DISABLED. The in-frame composite does not draw (proven down to an
  // unconditional constant colour), so this 20 Hz QWidget::render() of the ENTIRE editor widget
  // tree was pure main-thread cost in the user's full-UI session -- and the main thread is what
  // finishes model loads and uploads. Re-enable only together with a working composite.
  LogError << "[VK] present surface created (" << width() << "x" << height()
           << "), overlay controls promoted" << std::endl;
#endif
}

void MapView::setCameraForCapture(glm::vec3 const& position, math::degrees yaw, math::degrees pitch)
{
  _camera.position = position;
  _camera.yaw(yaw);
  _camera.pitch(pitch);
  _camera_moved_since_last_draw = true;
  _needs_redraw = true;
  update();
}

void MapView::setVkParityForced(std::string const& cams_file)
{
  vk_diff::forced() = true;
  vk_diff::apiMode() = 1;
  vk_diff::parityCheck() = true;
  vk_diff::camsPath() = cams_file;
  vk_diff::finished() = false;
}

bool MapView::vkParityFinished()
{
  return vk_diff::finished();
}

void MapView::muteAudioForHarness()
{
  Noggit::Rendering::g_noggit_harness_silent = true;
  Noggit::Ui::WaterSoundPlayer::instance().stop_all();
  if (_zone_music_player)
  {
    _zone_music_player->set_enabled(false); // (stop_playback is private; set_enabled(false) silences the decks)
  }
  Noggit::Ui::SfxPlayer::instance().set_volume(0);
}

// Request a capture, then render one frame; paintGL performs the readback at its end (see
// _pending_screenshot). Reading after paintGL RETURNS was the bug -- twice.
void MapView::saveHarnessScreenshot(std::string const& path)
{
  if (width() <= 0 || height() <= 0 || !_gl_initialized)
    return;
  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());
  _pending_screenshot = path;
  _needs_redraw = true;
  paintGL();
  glFinish();
  if (!_pending_screenshot.empty())   // paintGL bailed early (redraw gate) -- capture what is there
  {
    _pending_screenshot.clear();
    captureFrameNow(path);
  }
}

void MapView::captureFrameNow(std::string const& path)
{
#ifdef _WIN32
  // [VULKAN NATIVE PRESENT] the frame never reaches the GL widget -- read it back from Vulkan.
  // Rows arrive bottom-up exactly like glReadPixels, so savePng treats both identically.
  if (g_vk_capture_backend && g_vk_capture_backend->presentActive())
  {
    std::vector<std::uint8_t> px;
    // Prefer the ACTUAL presented swapchain image (what the window shows) over the offscreen
    // render target -- the offscreen readback cannot prove the present path. Falls back to the
    // offscreen image when the surface has no TRANSFER_SRC.
    bool const from_swapchain =
        g_vk_capture_backend->presentedReadbackAvailable()
        && g_vk_capture_backend->readbackPresented(px);
    if (from_swapchain || g_vk_capture_backend->readbackImage(px))
    {
      int const w = static_cast<int>(from_swapchain ? g_vk_capture_backend->presentWidth()
                                                     : g_vk_capture_backend->width());
      int const h = static_cast<int>(from_swapchain ? g_vk_capture_backend->presentHeight()
                                                     : g_vk_capture_backend->height());
      LogError << "[VK-BENCH] capture source = "
               << (from_swapchain ? "PRESENTED swapchain" : "offscreen image") << std::endl;
      vk_diff::savePng(path, px.data(), w, h);
      double sum = 0.0, sum2 = 0.0;
      std::size_t const n = static_cast<std::size_t>(w) * h;
      for (std::size_t i = 0; i < n; ++i)
      {
        double const l = (px[i * 4u + 0u] + px[i * 4u + 1u] + px[i * 4u + 2u]) / 3.0;
        sum += l;
        sum2 += l * l;
      }
      double const mean = n ? sum / n : 0.0;
      double const var = n ? (sum2 / n - mean * mean) : 0.0;
      double const sd = var > 0.0 ? std::sqrt(var) : 0.0;
      LogError << "[VK-BENCH] IMAGE " << path << " mean=" << mean << " stddev=" << sd
               << (sd < 1.0 ? "  *** FLAT -- camera saw nothing, run is INVALID ***" : "")
               << " | cam=(" << _camera.position.x << "," << _camera.position.y << ","
               << _camera.position.z << ") yaw=" << _camera.yaw()._
               << " pitch=" << _camera.pitch()._
               << std::endl;
      LogError << "[VK-BENCH] screenshot " << path << " (" << w << "x" << h
               << ", native VK readback)" << std::endl;
      return;
    }
    LogError << "[VK] native readback failed -- falling back to GL capture" << std::endl;
  }
#endif
  glFinish();

  GLint vp[4] = { 0, 0, 0, 0 };
  gl.getIntegerv(GL_VIEWPORT, vp);
  std::vector<std::uint8_t> px(static_cast<std::size_t>(vp[2]) * vp[3] * 4u);
  // [black-screenshot fix] paintGL() draws into the QOpenGLWidget's OWN framebuffer, and by the time
  // it returns that FBO is no longer the read target -- so readPixels here sampled a buffer nothing
  // had drawn to and returned pure black. The renderer was fine the whole time: the VK colour probe
  // (which reads INSIDE paintGL, while the widget FBO is still bound) showed a full scene from the
  // very same frame. Bind the widget's framebuffer explicitly before reading.
  GLint prev_read = 0, prev_draw = 0;
  gl.getIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
  gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
  GLuint const widget_fbo = static_cast<GLuint>(defaultFramebufferObject());
  {
    // [MC BLACK] which buffer are we actually reading? In map 0 the widget FBO is the one the frame
    // was drawn into; in a WMO-only map the capture comes out black while the in-frame probe shows a
    // full scene, so the bindings must differ.
    static int s_fbdbg = 0;
    if (s_fbdbg++ < 4)
      LogError << "[VK] CAPTURE FBO: widget=" << widget_fbo
               << " prevRead=" << prev_read << " prevDraw=" << prev_draw
               << " vp=" << vp[2] << "x" << vp[3] << std::endl;
  }
  gl.bindFramebuffer(GL_READ_FRAMEBUFFER, widget_fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  gl.readPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
  vk_diff::savePng(path, px.data(), vp[2], vp[3]);
  // [blank-frame guard] Report mean+stddev of the captured pixels. A run that ends with the camera
  // in empty sky produces a FLAT image (stddev ~0) -- the renderer drew nothing, so both its image
  // check and its frame timings describe an empty scene, not the map. Without this the flat frame is
  // invisible in the log and reads as a rendering bug (or, worse, as a fast result).
  {
    double sum = 0.0, sum2 = 0.0;
    std::size_t const n = static_cast<std::size_t>(vp[2]) * vp[3];
    for (std::size_t i = 0; i < n; ++i)
    {
      double const l = (px[i * 4u + 0u] + px[i * 4u + 1u] + px[i * 4u + 2u]) / 3.0;
      sum += l;
      sum2 += l * l;
    }
    double const mean = n ? sum / n : 0.0;
    double const var = n ? (sum2 / n - mean * mean) : 0.0;
    double const sd = var > 0.0 ? std::sqrt(var) : 0.0;
    // Camera state too: an A/B image check is only comparable if BOTH runs ended at the same
    // viewpoint, and an A/B TIMING comparison is only valid if both flew over the same terrain.
    LogError << "[VK-BENCH] IMAGE " << path << " mean=" << mean << " stddev=" << sd
             << (sd < 1.0 ? "  *** FLAT -- camera saw nothing, run is INVALID ***" : "")
             << " | cam=(" << _camera.position.x << "," << _camera.position.y << ","
             << _camera.position.z << ") yaw=" << _camera.yaw()._
             << " pitch=" << _camera.pitch()._
             << std::endl;
  }
  LogError << "[VK-BENCH] screenshot " << path << " (" << vp[2] << "x" << vp[3] << ")" << std::endl;
}

void MapView::renderFrameForHarness()
{
  if (width() <= 0 || height() <= 0 || !_gl_initialized)
  {
    return;
  }
  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());
  _needs_redraw = true;
  // [GL-COST HUNT] Split the frame into CPU (paintGL) and the drain (glFinish). Every phase timer
  // lives inside paintGL, so anything spent WAITING for the GPU -- including the compose sampling
  // VK's image, which waits on VK's semaphore -- lands here and was invisible. VK's frame has ~6 ms
  // more "other" than GL's and nothing inside paintGL accounts for it.
  // [spike hunt] Per-FRAME phase deltas. Averages over 300 frames cannot see a stall that happens on
  // 1% of frames, and twice now they pointed at the wrong thing (78/79). Snapshot the accumulators
  // around this one frame and dump the split for the frames that actually stall.
  double const _f_pre0 = vk_stat_pg_pre_ms(), _f_map0 = vk_stat_pg_map_ms();
  double const _f_blk0 = vk_stat_block2_ms(), _f_scn0 = vk_stat_glscene_any_ms();
  double const _f_prep0 = vk_stat_prep_ms(), _f_tile0 = vk_stat_tile_ms();
  // finding 80: two in-flight spikes had PREP 11-15 ms with NO tile rebuild, so split PREP into
  // its stages and S1 into its feeds to find what else in the feed phase stalls.
  double const _f_p1_0 = vk_stat_prep1_ms(), _f_p2_0 = vk_stat_prep2_ms(), _f_p3_0 = vk_stat_prep3_ms();
  double const _f_cull0 = vk_stat_cull_ms(), _f_m2u0 = vk_stat_m2up_ms();
  double const _f_wliq0 = vk_stat_wliq_ms(), _f_part0 = vk_stat_part_ms();
  double const _f_sk10 = vk_stat_skdome_ms(), _f_sk20 = vk_stat_skcmesh_ms(),
               _f_sk30 = vk_stat_skctex_ms(), _f_s2c0 = vk_stat_s2cel_ms();
  double const _f_s2s0 = vk_stat_s2sky_ms(), _f_s2p0 = vk_stat_s2part_ms(),
               _f_s2w0 = vk_stat_s2wmo_ms();
  double const _f_wp0 = vk_stat_wpairs_ms(), _f_wa0 = vk_stat_warena_ms(),
               _f_wt0 = vk_stat_wtable_ms();
  double const _f_wps0 = vk_stat_wp_sweep_ms(), _f_wpl0 = vk_stat_wp_lock_ms(),
               _f_wpm0 = vk_stat_wp_make_ms(), _f_wpi0 = vk_stat_wp_idof_ms(),
               _f_wpn0 = vk_stat_wp_need();
  auto const t_pg0 = std::chrono::steady_clock::now();
  paintGL();
  auto const t_pg1 = std::chrono::steady_clock::now();
  glFinish();
  auto const t_pg2 = std::chrono::steady_clock::now();
  {
    static unsigned _f_n = 0;
    ++_f_n;
    double const _f_ms = std::chrono::duration<double, std::milli>(t_pg2 - t_pg0).count();
    bool const _f_reset = (vk_stat_prep_ms() - _f_prep0) < 0.0;
    if (_f_ms > 15.0 && _f_reset)
      LogError << "[VK] FRAME SPIKE f" << _f_n << " " << _f_ms
               << " ms | phase split VOID (300-frame report reset the accumulators)" << std::endl;
    else if (_f_ms > 15.0)
      LogError << "[VK] FRAME SPIKE f" << _f_n << " " << _f_ms << " ms | paintGL="
               << std::chrono::duration<double, std::milli>(t_pg1 - t_pg0).count()
               << " glFinish=" << std::chrono::duration<double, std::milli>(t_pg2 - t_pg1).count()
               << " | preMap=" << (vk_stat_pg_pre_ms() - _f_pre0)
               << " drawMap=" << (vk_stat_pg_map_ms() - _f_map0)
               << " vkBlock=" << (vk_stat_block2_ms() - _f_blk0)
               << " scenePass=" << (vk_stat_glscene_any_ms() - _f_scn0)
               << " PREP=" << (vk_stat_prep_ms() - _f_prep0)
               << " tileRebuild=" << (vk_stat_tile_ms() - _f_tile0)
               << " | S1=" << (vk_stat_prep1_ms() - _f_p1_0)
               << " S2=" << (vk_stat_prep2_ms() - _f_p2_0)
               << " S3=" << (vk_stat_prep3_ms() - _f_p3_0)
               << " | cull=" << (vk_stat_cull_ms() - _f_cull0)
               << " m2feed=" << (vk_stat_m2up_ms() - _f_m2u0)
               << " wmoLiq=" << (vk_stat_wliq_ms() - _f_wliq0)
               << " part=" << (vk_stat_part_ms() - _f_part0)
               << " | skyDome=" << (vk_stat_skdome_ms() - _f_sk10)
               << " cloudMesh=" << (vk_stat_skcmesh_ms() - _f_sk20)
               << " cloudTex=" << (vk_stat_skctex_ms() - _f_sk30)
               << " celest=" << (vk_stat_s2cel_ms() - _f_s2c0)
               << " | s2sky=" << (vk_stat_s2sky_ms() - _f_s2s0)
               << " s2part=" << (vk_stat_s2part_ms() - _f_s2p0)
               << " s2wmo=" << (vk_stat_s2wmo_ms() - _f_s2w0)
               << " [pairs=" << (vk_stat_wpairs_ms() - _f_wp0)
               << " arena=" << (vk_stat_warena_ms() - _f_wa0)
               << " table=" << (vk_stat_wtable_ms() - _f_wt0)
               << " sweep=" << (vk_stat_wp_sweep_ms() - _f_wps0)
               << " lock=" << (vk_stat_wp_lock_ms() - _f_wpl0)
               << " make=" << (vk_stat_wp_make_ms() - _f_wpm0)
               << " idof=" << (vk_stat_wp_idof_ms() - _f_wpi0)
               << " need=" << (vk_stat_wp_need() - _f_wpn0) << "]" << std::endl;
  }
  {
    static double s_paint = 0.0, s_finish = 0.0;
    static int s_n = 0;
    s_paint  += std::chrono::duration<double, std::milli>(t_pg1 - t_pg0).count();
    s_finish += std::chrono::duration<double, std::milli>(t_pg2 - t_pg1).count();
    if (++s_n >= 300)
    {
      LogError << "[VK] FRAME SPLIT(any api): paintGL=" << (s_paint / s_n)
               << " glFinish=" << (s_finish / s_n) << " ms/frame" << std::endl;
      {
        // [PIPELINE scope] GL draws still issued by the traversal, per frame, in THIS api.
        LogError << "[VK] GL DRAWS/frame(any api): instanced=" << (Noggit::Rendering::g_gl_draw_instanced / s_n)
                 << " single=" << (Noggit::Rendering::g_gl_draw_single / s_n)
                 << " persistent=" << (Noggit::Rendering::g_gl_draw_persistent / s_n)
                 << " particles=" << (Noggit::Rendering::g_gl_draw_particles / s_n)
                 << " ribbons=" << (Noggit::Rendering::g_gl_draw_ribbons / s_n)
                 << " wmoGroup=" << (Noggit::Rendering::g_gl_draw_wmo_group / s_n) << std::endl;
        Noggit::Rendering::g_gl_draw_instanced = Noggit::Rendering::g_gl_draw_single = Noggit::Rendering::g_gl_draw_persistent = 0;
        Noggit::Rendering::g_gl_draw_particles = Noggit::Rendering::g_gl_draw_ribbons = Noggit::Rendering::g_gl_draw_wmo_group = 0;
      }
      LogError << "[VK] PAINTGL SPLIT(any api): preDrawMap=" << (vk_stat_pg_pre_ms() / s_n)
               << " drawMap=" << (vk_stat_pg_map_ms() / s_n)
               << " postDrawMap=" << ((s_paint - vk_stat_pg_pre_ms() - vk_stat_pg_map_ms()) / s_n)
               << " vkBlock=" << (vk_stat_block2_ms() / s_n)
               << " mid=" << (vk_stat_mid_ms() / s_n)
               << " tail=" << (vk_stat_tail_ms() / s_n)
               << " | blkFrames=" << vk_stat_blk_frames()
               << " prepFrames=" << vk_stat_prep_frames()
               << " ms/frame" << std::endl;
      vk_stat_mid_ms() = 0.0; vk_stat_tail_ms() = 0.0;
      vk_stat_blk_frames() = 0.0; vk_stat_prep_frames() = 0.0;
      vk_stat_pg_pre_ms() = 0.0; vk_stat_pg_map_ms() = 0.0; vk_stat_block2_ms() = 0.0;
      s_paint = s_finish = 0.0; s_n = 0;
    }
  }
  doneCurrent();
}

QImage MapView::grabRenderedFrameForCapture()
{
  if (width() <= 0 || height() <= 0)
  {
    return {};
  }

  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture begin size="
             << width() << "x" << height()
             << " gl_initialized=" << _gl_initialized
             << std::endl;
  }
  makeCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after makeCurrent" << std::endl;
  }
  OpenGL::context::scoped_setter const _(::gl, context());
  _needs_redraw = true;
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture before paintGL" << std::endl;
  }
  paintGL();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after paintGL" << std::endl;
  }
  glFinish();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after glFinish" << std::endl;
  }

  QImage image(width(), height(), QImage::Format_RGBA8888);
  if (!image.isNull())
  {
    gl.readPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    image = image.mirrored(false, true);
  }
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture after readPixels null=" << image.isNull() << std::endl;
  }

  doneCurrent();
  if (capture_debug_enabled())
  {
    LogDebug << "MapView::grabRenderedFrameForCapture end" << std::endl;
  }
  return image;
}

bool MapView::event(QEvent* e)
{
  // Suppress shortcut triggers for PLAIN Z/X while in the creature/gameobject editor so they can
  // raise/lower the selected spawn instead of firing their menu shortcuts. Modifier combos MUST
  // pass through: Ctrl+Z is the spawn-edit undo and Ctrl+Shift+Z the redo -- swallowing those here
  // made "Ctrl+Z just lowers the creature". (Shift stays suppressed: Shift+Z/X = the coarse nudge.)
  if (e->type() == QEvent::ShortcutOverride
      && (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject))
  {
    auto* ke = static_cast<QKeyEvent*>(e);
    if ((ke->key() == Qt::Key_Z || ke->key() == Qt::Key_X)
        && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
    {
      e->accept();
      return true;
    }
  }
  return QOpenGLWidget::event(e);
}

bool MapView::eventFilter(QObject* obj, QEvent* e)
{
  // Keep the centred time globe from being shoved when the left secondary toolbar (patrol paths /
  // creature info, terrain-mode options) appears at the far left of the globe's row: mirror its width
  // into a spacer on the far right so the globe block stays centred on the true viewport centre.
  if (_globe_balance_spacer && _viewport_overlay_ui
      && obj == _viewport_overlay_ui->leftSecondaryToolbarHolder)
  {
    switch (e->type())
    {
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::Hide:
      {
        auto* holder = _viewport_overlay_ui->leftSecondaryToolbarHolder;
        int const w = holder->isVisible() ? std::max(holder->width(), holder->sizeHint().width()) : 0;
        if (_globe_balance_spacer->width() != w)
        {
          _globe_balance_spacer->setFixedWidth(w);
        }
        break;
      }
      default:
        break;
    }
  }
  return QOpenGLWidget::eventFilter(obj, e);
}

void MapView::keyPressEvent (QKeyEvent *event)
{
  // Creature / GameObject tools: X raises, Z lowers the selected spawn(s). Handled before the hotkey
  // loop so X/Z don't trigger their menu shortcuts (e.g. texture browser) while editing spawns.
  // PLAIN key (or Shift for the coarse step) only -- Ctrl/Alt/Meta combos fall through so Ctrl+Z /
  // Ctrl+Shift+Z reach the spawn-edit undo/redo actions instead of nudging the spawn.
  if ((terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
      && (event->key() == Qt::Key_X || event->key() == Qt::Key_Z)
      && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
  {
    bool const has_selection = terrainMode == editing_mode::creature
      ? _selected_creature_spawn_guid.has_value()
      : _selected_gameobject_spawn_guid.has_value();

    if (has_selection)
    {
      float const step = (event->modifiers() & Qt::ShiftModifier) ? 2.0f : 0.5f; // Shift = coarse
      glm::vec3 const delta(0.0f, event->key() == Qt::Key_X ? step : -step, 0.0f); // X up, Z down

      if (terrainMode == editing_mode::creature)
        translateSelectedCreatureSpawns(delta);
      else
        translateSelectedGameObjectSpawns(delta);

      _needs_redraw = true;
      return;
    }
  }

  size_t const modifier
    ( ((event->modifiers() & Qt::ShiftModifier) ? MOD_shift : 0)
    | ((event->modifiers() & Qt::ControlModifier) ? MOD_ctrl : 0)
    | ((event->modifiers() & Qt::AltModifier) ? MOD_alt : 0)
    | ((event->modifiers() & Qt::MetaModifier) ? MOD_meta : 0)
    | ((event->modifiers() & Qt::KeypadModifier) ? MOD_num : 0)
    | (_mod_space_down ? MOD_space : 0)
    );

  for (auto&& hotkey : hotkeys)
  {
    if (event->key() == hotkey.key && modifier == hotkey.modifiers && hotkey.condition())
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _ (::gl, context());

      hotkey.function();
      return;
    }
  }

  if (event->key() == Qt::Key_Space)
  {
    // [game mode] jump is EDGE-triggered like the client: one jump per physical press. A fresh
    // press only counts when space was UP (auto-repeat keydowns and held state don't re-arm).
    if (!_mod_space_down && !event->isAutoRepeat())
    {
      _game_jump_pressed = true;
    }
    _mod_space_down = true;
  }

  if (event->key() == Qt::Key_Z) _mod_z_down = true;
  if (event->key() == Qt::Key_X) _mod_x_down = true;

  checkInputsSettings();

  // movement (per-key states so opposing-pair releases hand over instead of zeroing the axis)
  if (event->key() == _inputs[0])
  {
    _key_move_fwd = true;
    moving = 1.0f;
  }
  if (event->key() == _inputs[1])
  {
    _key_move_back = true;
    moving = -1.0f;
  }

  if (event->key() == Qt::Key_Up)
  {
    lookat = 0.75f;
  }
  if (event->key() == Qt::Key_Down)
  {
    lookat = -0.75f;
  }

  if (event->key() == Qt::Key_Right)
  {
    turn = 0.75f;
  }
  if (event->key() == Qt::Key_Left)
  {
    turn = -0.75f;
  }

  if (event->key() == _inputs[2])
  {
    _key_strafe_pos = true;
    strafing = 1.0f;
  }
  if (event->key() == _inputs[3])
  {
    _key_strafe_neg = true;
    strafing = -1.0f;
  }

  if (event->key() == _inputs[4])
  {
    _key_updown_pos = true;
    updown = 1.0f;
  }
  if (event->key() == _inputs[5])
  {
    _key_updown_neg = true;
    updown = -1.0f;
  }

  if (event->key() == Qt::Key_2 && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = 1;
  }
  if (event->key() == Qt::Key_8 && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = -1;
  }

  if (event->key() == Qt::Key_4 && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = 1;
  }
  if (event->key() == Qt::Key_6 && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = -1;
  }

  if (event->key() == Qt::Key_3 && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = 1;
  }
  if (event->key() == Qt::Key_1 && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = -1;
  }

  if (event->key() == Qt::Key_7 && event->modifiers() & Qt::KeypadModifier)
  {
    keyr = 1;
  }
  if (event->key() == Qt::Key_9 && event->modifiers() & Qt::KeypadModifier)
  {
    keyr = -1;
  }

  if (event->key() == Qt::Key_Plus)
  {
    keys = 1;

    switch (terrainMode)
    {
      case editing_mode::mccv:
      {
        shaderTool->addColorToPalette();
        break;
      }
      default:
        break;
    }
  }
  if (event->key() == Qt::Key_Minus)
  {
    keys = -1;
  }
  if (event->key() == Qt::Key_Home)
  {
	  _camera.position = glm::vec3(_cursor_pos.x, _cursor_pos.y + 50, _cursor_pos.z);
    _camera_moved_since_last_draw = true;
  }

  if (event->key() == Qt::Key_L)
  {
    freelook = true;
  }

  if (_display_mode == display_mode::in_2D)
  {
    TileIndex cur_tile = TileIndex(_camera.position);

    if (event->key() == Qt::Key_Up)
    {
      auto next_z = cur_tile.z - 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Down)
    {
      auto next_z = cur_tile.z + 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Left)
    {
      auto next_x = cur_tile.x - 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Right)
    {
      auto next_x = cur_tile.x + 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }

  }

  if (_gizmo_on.get() && !_transform_gizmo.isUsing())
  {
    if (!_change_operation_mode && event->key() == Qt::Key_Space)
    {
      if (_gizmo_operation == ImGuizmo::OPERATION::TRANSLATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
      }
      else if (_gizmo_operation == ImGuizmo::OPERATION::ROTATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
      }
      else
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
      }

      _change_operation_mode = true;
    }
  }
}

void MapView::keyReleaseEvent (QKeyEvent* event)
{
  // A HELD key delivers synthetic auto-repeat release/press pairs -- each synthetic release read
  // as "let go" zeroed the movement axis for one frame, flapping the game-mode animation
  // Run->Stand->Run every repeat (char-anim trace: 20-100ms pairs while W was held) and
  // restarting its cross-fade mid-pose (the limb-spin on every start/stop). Only the REAL
  // release matters.
  if (event->isAutoRepeat())
  {
    return;
  }

  if (event->key() == Qt::Key_Space)
    _mod_space_down = false;

  if (event->key() == Qt::Key_Z) _mod_z_down = false;
  if (event->key() == Qt::Key_X) _mod_x_down = false;

  if (_change_operation_mode && event->key() == Qt::Key_Space)
    _change_operation_mode = false;

  checkInputsSettings();

  // movement: releasing one of an opposing pair hands the axis to the still-held key (releasing A
  // a moment after pressing D used to zero the axis and eat the input)
  if (event->key() == _inputs[0])
  {
    _key_move_fwd = false;
    moving = _key_move_back ? -1.0f : 0.0f;
  }
  if (event->key() == _inputs[1])
  {
    _key_move_back = false;
    moving = _key_move_fwd ? 1.0f : 0.0f;
  }

  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
  {
    lookat = 0.0f;
  }

  if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)
  {
    turn  = 0.0f;
  }

  if (event->key() == _inputs[2])
  {
    _key_strafe_pos = false;
    strafing = _key_strafe_neg ? -1.0f : 0.0f;
  }
  if (event->key() == _inputs[3])
  {
    _key_strafe_neg = false;
    strafing = _key_strafe_pos ? 1.0f : 0.0f;
  }

  if (event->key() == _inputs[4])
  {
    _key_updown_pos = false;
    updown = _key_updown_neg ? -1.0f : 0.0f;
  }
  if (event->key() == _inputs[5])
  {
    _key_updown_neg = false;
    updown = _key_updown_pos ? 1.0f : 0.0f;
  }
  

  if ((event->key() == Qt::Key_2 || event->key() == Qt::Key_8) && event->modifiers() & Qt::KeypadModifier)
  {
    keyx = 0.0f;
  }

  if ((event->key() == Qt::Key_4 || event->key() == Qt::Key_6) && event->modifiers() & Qt::KeypadModifier)
  {
    keyz = 0.0f;
  }

  if ((event->key() == Qt::Key_3 || event->key() == Qt::Key_1) && event->modifiers() & Qt::KeypadModifier)
  {
    keyy = 0.0f;
  }

  if ((event->key() == Qt::Key_7 || event->key() == Qt::Key_9) && event->modifiers() & Qt::KeypadModifier)
  {
    keyr  = 0.0f;
  }

  if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Minus)
  {
    keys = 0.0f;
  }

  if (event->key() == Qt::Key_L || event->key() == Qt::Key_Minus)
  {
    freelook = false;
  }

}

void MapView::checkInputsSettings()
{
  QString _locale = _settings->value("keyboard_locale", "QWERTY").toString();

  // default is QWERTY
  _inputs = std::array<Qt::Key, 6>{Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};

  if (_locale == "AZERTY")
  {
      _inputs = std::array<Qt::Key, 6>{Qt::Key_Z, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_A, Qt::Key_E};
  }
}

void MapView::focusOutEvent (QFocusEvent*)
{
  _mod_alt_down = false;
  _mod_z_down = false;
  _mod_x_down = false;
  _mod_ctrl_down = false;
  _mod_shift_down = false;
  _mod_space_down = false;
  _mod_num_down = false;

  moving = 0.0f;
  lookat = 0.0f;
  turn = 0.0f;
  strafing = 0.0f;
  updown = 0.0f;
  _key_move_fwd = _key_move_back = false;
  _key_strafe_pos = _key_strafe_neg = false;
  _key_updown_pos = _key_updown_neg = false;

  keyx = 0;
  keyz = 0;
  keyy = 0;
  keyr = 0;
  keys = 0;

  leftMouse = false;
  rightMouse = false;
  MoveObj = false;
  look = false;
  freelook = false;
}

void MapView::mouseMoveEvent (QMouseEvent* event)
{
  //! \todo:  move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());
  QLineF const relative_movement (_last_mouse_pos, event->pos());

  // [game mode] LMB orbit: rotate the VIEW around the character; facing untouched. Same drag feel
  // as the RMB look (matching signs of add_to_yaw/add_to_pitch, which subtract their argument).
  if (_game_orbiting && _game_mode_camera.get())
  {
    _game_orbit_yaw -= static_cast<float>(relative_movement.dx()) / XSENS;
    _game_orbit_pitch -= mousedir * static_cast<float>(relative_movement.dy()) / YSENS;
    _game_orbit_pitch = std::clamp(_game_orbit_pitch, -160.0f, 160.0f);
    _camera_moved_since_last_draw = true;
    // repaint NOW (coalesced by Qt), don't wait for the pacing timer: the timer-only redraw
    // added up to a full timer period of look latency and beat unevenly against vsync -- the
    // mouselook input lag / jerky stepping
    _needs_redraw = true;
    update();
    _last_mouse_pos = event->pos();
    return;
  }

  // [game mode] SPACE is the jump key -- it must not suppress mouselook like the editor's
  // space-modifier does (holding space while turning froze the camera)
  bool const look_mod_block = _mod_shift_down || _mod_ctrl_down || _mod_alt_down
                           || (!_game_mode_camera.get() && _mod_space_down);
  if ((look || freelook) && !look_mod_block)
  {
    _camera.add_to_yaw(math::degrees(relative_movement.dx() / XSENS));
    _camera.add_to_pitch(math::degrees(mousedir * relative_movement.dy() / YSENS));
    _camera_moved_since_last_draw = true;
    // repaint NOW (coalesced), same as the orbit branch: mouselook must not wait on the timer
    _needs_redraw = true;
    update();
  }

  // Drag placement follows the nearest SOLID surface (WMO roof / doodad / terrain). Picked here
  // rather than reusing _cursor_pos because the per-frame cursor update bails out while the camera is
  // moving -- so dragging a spawn WHILE flying would otherwise freeze it at a stale point. The result
  // is written back to _cursor_pos so the aim circle tracks the drag too.
  if ((_dragging_creature_spawn || _dragging_gameobject_spawn) && leftMouse)
  {
    if (auto const surface = surface_pos_under_cursor())
    {
      _cursor_pos = *surface;
    }

    if (_dragging_creature_spawn)
    {
      updateSelectedCreatureSpawnPosition(_cursor_pos);
    }
    else
    {
      updateSelectedGameObjectSpawnPosition(_cursor_pos);
    }

    _last_mouse_pos = event->pos();
    return;
  }

  if (leftMouse
      && (terrainMode == editing_mode::creature || terrainMode == editing_mode::gameobject)
      && _area_selection->isVisible()
      && _display_mode == display_mode::in_3D
      && !ImGuizmo::IsUsing())
  {
    _needs_redraw = true;
    _area_selection->setGeometry(QRect(_drag_start_pos, event->pos()).normalized());
  }

  updateCreatureSpawnHover(event->globalPos());
  updateGameObjectSpawnHover(event->globalPos());

  if (MoveObj)
  {
    mh = -aspect_ratio()*relative_movement.dx() / static_cast<float>(width());
    mv = -relative_movement.dy() / static_cast<float>(height());
  }
  else
  {
    mh = 0.0f;
    mv = 0.0f;
  }

  if (_mod_shift_down || _mod_ctrl_down || _mod_alt_down || _mod_space_down)
  {
    rh = relative_movement.dx() / XSENS * 5.0f;
    rv = relative_movement.dy() / YSENS * 5.0f;
  }

  if (rightMouse && _mod_alt_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                     Noggit::ActionModalityControllers::eALT | Noggit::ActionModalityControllers::eRMB);
        terrainTool->changeOrientation (-relative_movement.dx() / XSENS * 4.5f);
      }
      else
      {
        terrainTool->changeInnerRadius(relative_movement.dx() / 100.0f);
      }

    }
    else if (terrainMode == editing_mode::paint)
    {
      texturingTool->change_hardness(relative_movement.dx() / 300.0f);
    }
    else if (terrainMode == editing_mode::stamp)
    {
      stampTool->changeInnerRadius(relative_movement.dx() / 300.0f);
    }
  }

  if (rightMouse && _mod_shift_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eSHIFT | Noggit::ActionModalityControllers::eRMB);
        terrainTool->moveVertices (_world.get(), -relative_movement.dy() / YSENS);
      }
    }
  }

  if (rightMouse && _mod_ctrl_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eCTRL |
                                                       Noggit::ActionModalityControllers::eRMB);
        terrainTool->changeAngle(-relative_movement.dy() / YSENS * 4.f);
      }
    }
  }


  if (rightMouse && _mod_space_down)
  {
    if (terrainMode == editing_mode::ground)
    {
      if (terrainTool->_edit_type == eTerrainType_Vertex)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eRMB
                                                       | Noggit::ActionModalityControllers::eSPACE);
        terrainTool->setOrientRelativeTo(_world.get(), _cursor_pos);
      }
      else if (terrainTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                       Noggit::ActionModalityControllers::eRMB
                                                       | Noggit::ActionModalityControllers::eSPACE);
        terrainTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);
      }

    }
    else if (terrainMode == editing_mode::paint)
    {
      if (texturingTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                     Noggit::ActionModalityControllers::eRMB
                                                                     | Noggit::ActionModalityControllers::eSPACE);
        texturingTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);

      }

    }
    else if (terrainMode == editing_mode::mccv)
    {
      if (shaderTool->getImageMaskSelector()->isEnabled())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                     Noggit::ActionModalityControllers::eRMB
                                                                     | Noggit::ActionModalityControllers::eSPACE);
        shaderTool->getImageMaskSelector()->setRotation(-relative_movement.dx() / XSENS * 10.f);
        action->setBlockCursor(true);

      }

    }
    else if (terrainMode == editing_mode::stamp)
    {

      auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                                                                   Noggit::ActionModalityControllers::eRMB
                                                                   | Noggit::ActionModalityControllers::eSPACE);
      stampTool->changeRotation(-relative_movement.dx() / XSENS * 10.f);
      action->setBlockCursor(true);
    }
  }

  if (leftMouse && _mod_alt_down)
  {
	switch (terrainMode)
    {
    case editing_mode::ground:
      terrainTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::flatten_blur:
      flattenTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::paint:
      texturingTool->change_radius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::water:
      guiWater->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::mccv:
      shaderTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::areaid:
      ZoneIDBrowser->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::holes:
      holeTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::object:
      objectEditor->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::minimap:
      minimapTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    case editing_mode::stamp:
      stampTool->changeRadius(relative_movement.dx() / XSENS);
      break;
    default:
      break;
    }
  }

  if (leftMouse && _mod_space_down)
  {
    switch (terrainMode)
    {
    case editing_mode::ground:
      terrainTool->changeSpeed(relative_movement.dx() / 30.0f);
      break;
    case editing_mode::flatten_blur:
      flattenTool->changeSpeed(relative_movement.dx() / 30.0f);
      break;
    case editing_mode::paint:
      texturingTool->change_pressure(relative_movement.dx() / 300.0f);
      break;
    case editing_mode::mccv:
      shaderTool->changeSpeed(relative_movement.dx() / XSENS);
      break;
    case editing_mode::stamp:
      stampTool->changeSpeed(relative_movement.dx() / XSENS);
      break;
    default:
      break;
    }
  }

  if (leftMouse && (_mod_shift_down || _mod_ctrl_down))
  {
    if (terrainMode == editing_mode::object || terrainMode == editing_mode::minimap)
    {
      doSelection(false, true); // Required for radius selection in Object mode
    }
  }

  if (leftMouse && _mod_shift_down)
  {
    if (terrainMode == editing_mode::ground && _display_mode == display_mode::in_3D)
    {
      auto image_mask_selector = terrainTool->getImageMaskSelector();
      if (terrainTool->_edit_type != eTerrainType_Vertex && terrainTool->_edit_type != eTerrainType_Script &&
        image_mask_selector->isEnabled() && !image_mask_selector->getBrushMode())
      {
        auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN,
                                                       Noggit::ActionModalityControllers::eSHIFT
                                                       | Noggit::ActionModalityControllers::eLMB);

        action->setPostCallback(&MapView::randomizeTerrainRotation);

        terrainTool->changeTerrain(_world.get(), _cursor_pos, relative_movement.dx() / 30.0f);
      }
    }
    else if (terrainMode == editing_mode::stamp && _display_mode == display_mode::in_3D && !stampTool->getBrushMode())
    {
      auto action = NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eNO_FLAG,
                                                                   Noggit::ActionModalityControllers::eSHIFT
                                                                   | Noggit::ActionModalityControllers::eLMB);

      action->setPostCallback(&MapView::randomizeStampRotation);
      action->setBlockCursor(true);

      stampTool->execute(_cursor_pos, _world.get(), relative_movement.dx() / 30.0f, _mod_shift_down, _mod_alt_down, _mod_ctrl_down, false);
    }

  }

  if (leftMouse && terrainMode == editing_mode::object && _display_mode == display_mode::in_3D && !ImGuizmo::IsUsing())
  {
      _needs_redraw = true;
      _area_selection->setGeometry(QRect(_drag_start_pos, event->pos()).normalized());
  }

  if (_display_mode == display_mode::in_2D && leftMouse && _mod_alt_down && _mod_shift_down)
  {
    strafing = ((relative_movement.dx() / XSENS) / -1) * 5.0f;
    moving = (relative_movement.dy() / YSENS) * 5.0f;
  }

  if (_display_mode == display_mode::in_2D && rightMouse && _mod_shift_down)
  {
    updown = (relative_movement.dy() / YSENS);
  }

  _last_mouse_pos = event->pos();
}

void MapView::change_selected_wmo_nameset(int set)
{
    auto last_entry = _world->get_last_selected_model();
    if (last_entry)
    {
        if (last_entry.value().index() != eEntry_Object)
        {
            return;
        }
        auto obj = std::get<selected_object_type>(last_entry.value());
        if (obj->which() == eWMO)
        {
            WMOInstance* wmo = static_cast<WMOInstance*>(obj);
            wmo->change_nameset(set);
            _world->updateTilesWMO(wmo, model_update::none); // needed?
            auto tiles = wmo->getTiles();
            for (auto tile : tiles)
            {
                tile->changed = true;
            }
        }
    }
}

void MapView::change_selected_wmo_doodadset(int set)
{
  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      auto wmo = static_cast<WMOInstance*>(obj);
      wmo->change_doodadset(set);
      _world->updateTilesWMO(wmo, model_update::none);
      auto tiles = wmo->getTiles();
      for (auto tile : tiles)
      {
        tile->changed = true;
      }
    }
  }
}

void MapView::mousePressEvent(QMouseEvent* event)
{
  if(event->source() == Qt::MouseEventNotSynthesized)
  {
    _tablet_manager->setIsActive(false);
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  // [game mode] client mouse model: LEFT drag orbits the camera around the character without
  // touching its facing; RIGHT drag stays the facing control (below). Pressing RIGHT while
  // orbited first turns the character to where the camera looks (the client's RMB take-over).
  if (_game_mode_camera.get() && _display_mode == display_mode::in_3D)
  {
    if (event->button() == Qt::LeftButton)
    {
      // deliberately NOT setting leftMouse: the tick applies the active terrain tool while
      // leftMouse is held, and orbiting must never sculpt the map underneath
      _game_orbiting = true;
      _last_mouse_pos = event->pos();
      return;
    }
    if (event->button() == Qt::RightButton && (_game_orbit_yaw != 0.0f || _game_orbit_pitch != 0.0f))
    {
      _camera.yaw(math::degrees(_camera.yaw()._ + _game_orbit_yaw));
      _camera.pitch(math::degrees(_camera.pitch()._ + _game_orbit_pitch));
      _game_orbit_yaw = 0.0f;
      _game_orbit_pitch = 0.0f;
      _camera_moved_since_last_draw = true;
    }
  }

  switch (event->button())
  {
  case Qt::LeftButton:
    leftMouse = true;
    break;

  case Qt::RightButton:
    rightMouse = true;
    break;

  case Qt::MiddleButton:
    if (_world->has_selection())
    {
      MoveObj = true;
    }

    if(terrainMode == editing_mode::mccv)
    {
      shaderTool->pickColor(_world.get(), _cursor_pos);
    }
    break;

  default:
    break;
  }

  if (leftMouse && terrainMode == editing_mode::creature)
  {
      if (_mod_shift_down)
      {
        _drag_start_pos = event->pos();
        _needs_redraw = true;
        _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
        _area_selection->show();
        return;
      }

      tryStartCreatureSpawnDrag();
      _area_selection->hide();
      return;
  }

  if (leftMouse && terrainMode == editing_mode::gameobject)
  {
      if (_mod_shift_down)
      {
        _drag_start_pos = event->pos();
        _needs_redraw = true;
        _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
        _area_selection->show();
        return;
      }

      tryStartGameObjectSpawnDrag();
      _area_selection->hide();
      return;
  }

  if (leftMouse && ((terrainMode == editing_mode::object || terrainMode == editing_mode::minimap) && !_mod_ctrl_down))
  {
      if (_mod_shift_down && tryStartCreatureSpawnDrag())
      {
        _area_selection->hide();
        return;
      }

      _drag_start_pos = event->pos();
      _needs_redraw = true;
      _area_selection->setGeometry(QRect(_drag_start_pos, QSize()));
      _area_selection->show();
  }

  if (rightMouse)
  {
    _right_click_pos = event->pos();
    look = true;
  }
}

void MapView::wheelEvent (QWheelEvent* event)
{
  //! \todo: move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  // [game mode] the wheel is the client's camera zoom: out into 3rd person, back in to first
  // person. Consumed here so no tool underneath reacts while playing.
  if (_game_mode_camera.get())
  {
    float const notches = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (notches != 0.0f)
    {
      _game_third_person_distance =
        std::clamp(_game_third_person_distance - notches * 1.5f, 0.0f, 25.0f);
      _camera_moved_since_last_draw = true;
    }
    event->accept();
    return;
  }

  // While DRAGGING a spawn, the wheel rotates it instead of doing whatever the tool normally does.
  // Every dragged spawn turns about its own centre, so a multi-selection keeps its layout and each
  // member faces the new way. Wheel up = clockwise. Default 1 deg per notch, Shift = 10 deg,
  // Ctrl = 45 deg.
  if (_dragging_creature_spawn || _dragging_gameobject_spawn)
  {
    float const notches = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (notches != 0.0f)
    {
      float const step = _mod_shift_down ? 10.0f : (_mod_ctrl_down ? 45.0f : 1.0f);
      rotateDraggedSpawns(-notches * step); // negative: wheel up reads as clockwise on screen
    }

    event->accept();
    return;
  }

  auto&& delta_for_range
    ( [&] (float range)
      {
        //! \note / 8.f for degrees, / 40.f for smoothness
        return (_mod_ctrl_down ? 0.01f : 0.1f) 
          * range 
          // alt = horizontal delta
          * (_mod_alt_down ? event->angleDelta().x() : event->angleDelta().y())
          / 320.f
          ;
      }
    );

  if (terrainMode == editing_mode::paint)
  {
    if (_mod_space_down)
    {
      texturingTool->change_brush_level (delta_for_range (255.f));
    }
    else if (_mod_alt_down)
    {
      texturingTool->change_spray_size (delta_for_range (39.f));
    }
    else if (_mod_shift_down)
    {
      texturingTool->change_spray_pressure (delta_for_range (10.f));
    }
  }
  else if (terrainMode == editing_mode::flatten_blur)
  {
    if (_mod_alt_down)
    {
      flattenTool->changeOrientation (delta_for_range (360.f));
    }
    else if (_mod_shift_down)
    {
      flattenTool->changeAngle (delta_for_range (89.f));
    }
    else if (_mod_space_down)
    {
      //! \note not actual range
      flattenTool->changeHeight (delta_for_range (40.f));
    }
  }
  else if (terrainMode == editing_mode::water)
  {
    if (_mod_alt_down)
    {
      guiWater->changeOrientation (delta_for_range (360.f));
    }
    else if (_mod_shift_down)
    {
      guiWater->changeAngle (delta_for_range (89.f));
    }
    else if (_mod_space_down)
    {
      //! \note not actual range
      guiWater->change_height (delta_for_range (40.f));
    }
  }
}

void MapView::mouseReleaseEvent (QMouseEvent* event)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  switch (event->button())
  {
  case Qt::LeftButton:
    if (_game_orbiting)
    {
      // [game mode] end the LMB orbit; the orbit offsets PERSIST (client behaviour: the camera
      // stays where you left it until RMB takes over).
      _game_orbiting = false;
      break;
    }
    if (_dragging_creature_spawn)
    {
      leftMouse = false;
      _dragging_creature_spawn = false;
      _creature_drag_anchor_pos = std::optional<glm::vec3>();

      // Only the dragged spawns changed, so retext just their rows. A full rebuild here was a ~1s
      // freeze on mouse-up (it allocates an item per spawn in the world). Capture the guids before
      // clearing the drag state.
      std::vector<std::uint32_t> moved;
      moved.reserve(_creature_drag_initial_positions.size());
      SpawnUndoOp undo_op;
      undo_op.kind = SpawnUndoOp::Kind::Move;
      for (auto const& drag_state : _creature_drag_initial_positions)
      {
        moved.push_back(drag_state.guid);
        // The drag-start snapshot IS the before-state; record it only if the spawn actually moved.
        if (auto const* spawn = _world->findCreatureSpawn(drag_state.guid))
        {
          if (glm::distance(spawn->pos, drag_state.pos) > 0.001f
              || std::abs(spawn->orientation - drag_state.orientation) > 0.001f)
          {
            undo_op.moves.push_back({drag_state.guid, drag_state.pos, drag_state.orientation});
          }
        }
      }
      if (!undo_op.moves.empty())
      {
        pushCreatureUndoOp(std::move(undo_op));
      }
      _creature_drag_initial_positions.clear();

      updateDatabaseStatus();
      refreshCreatureBrowserItems(moved);
      refreshCreatureEditorKnobs();
      break;
    }

    if (_dragging_gameobject_spawn)
    {
      leftMouse = false;
      _dragging_gameobject_spawn = false;
      _gameobject_drag_anchor_pos = std::optional<glm::vec3>();

      std::vector<std::uint32_t> moved;
      moved.reserve(_gameobject_drag_initial_positions.size());
      SpawnUndoOp undo_op;
      undo_op.kind = SpawnUndoOp::Kind::Move;
      for (auto const& drag_state : _gameobject_drag_initial_positions)
      {
        moved.push_back(drag_state.guid);
        if (auto const* spawn = _world->findGameObjectSpawn(drag_state.guid))
        {
          if (glm::distance(spawn->pos, drag_state.pos) > 0.001f
              || std::abs(spawn->orientation - drag_state.orientation) > 0.001f)
          {
            undo_op.moves.push_back({drag_state.guid, drag_state.pos, drag_state.orientation});
          }
        }
      }
      if (!undo_op.moves.empty())
      {
        pushGameObjectUndoOp(std::move(undo_op));
      }
      _gameobject_drag_initial_positions.clear();

      updateGameObjectBrowserStatus();
      refreshGameObjectBrowserItems(moved);
      refreshGameObjectEditorKnobs();
      break;
    }

    leftMouse = false;

    if (_display_mode == display_mode::in_2D)
    {
      strafing = 0;
      moving = 0;
    }

    if (terrainMode == editing_mode::creature)
    {
      auto drag_end_pos = event->pos();

      if (_area_selection->isVisible())
      {
        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
          selectCreatureSpawnsInArea(QRect(_drag_start_pos, drag_end_pos), true);
        }
        else if (auto guid = findCreatureSpawnAtCursor())
        {
          addCreatureSpawnToSelection(*guid);
        }

        _area_selection->hide();
        break;
      }

      if (auto guid = findCreatureSpawnAtCursor())
      {
        setSelectedCreatureSpawn(guid);
      }
      else
      {
        setSelectedCreatureSpawn(std::optional<std::uint32_t>());
      }

      break;
    }

    if (terrainMode == editing_mode::gameobject)
    {
      auto drag_end_pos = event->pos();

      if (_area_selection->isVisible())
      {
        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
          selectGameObjectSpawnsInArea(QRect(_drag_start_pos, drag_end_pos), true);
        }
        else if (auto guid = findGameObjectSpawnAtCursor())
        {
          addGameObjectSpawnToSelection(*guid);
        }

        _area_selection->hide();
        break;
      }

      if (auto guid = findGameObjectSpawnAtCursor())
      {
        setSelectedGameObjectSpawn(guid);
      }
      else
      {
        setSelectedGameObjectSpawn(std::optional<std::uint32_t>());
      }

      break;
    }

    if ((terrainMode == editing_mode::object || terrainMode == editing_mode::minimap) && !_mod_ctrl_down)
    {
        auto drag_end_pos = event->pos();

        if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
        {
            const std::array<glm::vec2, 2> selection_box
            {
                glm::vec2(std::min(_drag_start_pos.x(), drag_end_pos.x()), std::min(_drag_start_pos.y(), drag_end_pos.y())),
                glm::vec2(std::max(_drag_start_pos.x(), drag_end_pos.x()), std::max(_drag_start_pos.y(), drag_end_pos.y()))
            };
            // _world->select_objects_in_area(selection_box, !_mod_shift_down, model_view(), projection(), width(), height(), objectEditor->drag_selection_depth(), _camera.position);
            _world->select_objects_in_area(selection_box, !_mod_shift_down, model_view(), projection(), width(), height(), 3000.0f, _camera.position);
        }
        else // Do normal selection when we just clicked
        {
            doSelection(false);
        }
        
        _area_selection->hide();
    }
    else 
    {
        doSelection(true);
    }

    break;

  case Qt::RightButton:
    rightMouse = false;

    look = false;

    if (_display_mode == display_mode::in_2D)
      updown = 0;

    // // may need to be done in constructor of widget
    // this->setContextMenuPolicy(Qt::CustomContextMenu); 
    // connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
    //     this, SLOT(ShowContextMenu(const QPoint&)));



    break;

  case Qt::MiddleButton:
    MoveObj = false;
    break;

  default:
    break;
  }
}

void MapView::save(save_mode mode)
{
  bool save = true;

  // Save minimap creator model filters
  minimapTool->saveFiltersToJSON();

  if (AsyncLoader::instance().important_object_failed_loading())
  {
    save = false;
    QPushButton *yes, *no;

    QMessageBox first_warning;
    first_warning.setIcon(QMessageBox::Critical);
    first_warning.setWindowIcon(QIcon (":/icon"));
    first_warning.setWindowTitle("Some models couldn't be loaded");
    first_warning.setText("Error:\nSome models could not be loaded and saving will cause collision and culling issues, would you still like to save ?");
    // roles are swapped to force the user to pay attention and both are "accept" roles so that escape does nothing
    no = first_warning.addButton("No", QMessageBox::ButtonRole::AcceptRole);
    yes = first_warning.addButton("Yes", QMessageBox::ButtonRole::YesRole);
    first_warning.setDefaultButton(no);

    first_warning.exec();

    if (first_warning.clickedButton() == yes)
    {
      QMessageBox second_warning;
      second_warning.setIcon(QMessageBox::Warning);
      second_warning.setWindowIcon(QIcon (":/icon"));
      second_warning.setWindowTitle("Are you sure ?");
      second_warning.setText( "If you save you will have to save again all the adt containing the defective/missing models once you've fixed said models to correct all the issues.\n"
                              "By clicking yes you accept to bear all the consequences of your action and forfeit the right to complain to the developers about any culling and collision issues.\n\n"
                              "So... do you REALLY want to save ?"
                            );
      no = second_warning.addButton("No", QMessageBox::ButtonRole::YesRole);
      yes = second_warning.addButton("Yes", QMessageBox::ButtonRole::AcceptRole);
      second_warning.setDefaultButton(no);

      second_warning.exec();

      if (second_warning.clickedButton() == yes)
      {
        save = true;
      }
    }
  }

  if ( mode == save_mode::current 
    && save 
    && (QMessageBox::warning
          (nullptr
          , "Save current map tile only"
          , "This can cause a collision bug when placing objects between two ADT borders!\n\n"
            "We recommend you to use the normal save function rather than "
            "this one to get the collisions right."
          , QMessageBox::Save | QMessageBox::Cancel
          , QMessageBox::Cancel
          ) == QMessageBox::Cancel
       )
     )
  {
    save = false;
  }

  if (save)
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    switch (mode)
    {
    case save_mode::current: _world->mapIndex.saveTile(TileIndex(_camera.position), _world.get()); break;
    case save_mode::changed: _world->mapIndex.saveChanged(_world.get()); break;
    case save_mode::all:     _world->mapIndex.saveall(_world.get()); break;
    }
    // write wdl, we update wdl data prior in the mapIndex saving fucntions above
    _world->horizon.save_wdl(_world.get());


    NOGGIT_ACTION_MGR->purge();
    AsyncLoader::instance().reset_object_fail();


    _main_window->statusBar()->showMessage("Map saved", 2000);

  }
  else
  {
    QMessageBox::warning
      ( nullptr
      , "Map NOT saved"
      , "The map was NOT saved, don't forget to save before leaving"
      , QMessageBox::Ok
      );
  }
}

void MapView::addHotkey(Qt::Key key, size_t modifiers, std::function<void()> function, std::function<bool()> condition)
{
  hotkeys.emplace_front (key, modifiers, function, condition);
}

void MapView::randomizeTerrainRotation()
{
  auto image_mask_selector = terrainTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeTexturingRotation()
{
  auto image_mask_selector = texturingTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeShaderRotation()
{
  auto image_mask_selector = shaderTool->getImageMaskSelector();
  if (!image_mask_selector->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  image_mask_selector->setRotation(uid(gen));
}

void MapView::randomizeStampRotation()
{
  if (!stampTool->getRandomizeRotation())
    return;

  unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
  std::mt19937 gen(ms);
  std::uniform_int_distribution<> uid(0, 360);

  stampTool->changeRotation(uid(gen));
}

void MapView::unloadOpenglData()
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  ModelManager::unload_all(_context);
  WMOManager::unload_all(_context);
  TextureManager::unload_all(_context);

  for (MapTile* tile : _world->mapIndex.loaded_tiles())
  {
    tile->renderer()->unload();
    tile->Water.renderer()->unload();

    for (int i = 0; i < 16; ++i)
    {
      for (int j = 0; j < 16; ++j)
      {
        tile->getChunk(i, j)->unload();
      }
    }
  }

  _world->renderer()->unload();

  _buffers.unload();
  _gl_initialized = false;
}

QWidget* MapView::getSecondaryToolBar()
{
    return _viewport_overlay_ui->secondaryToolbarHolder;
}

QWidget* MapView::getLeftSecondaryToolbar()
{
    return _viewport_overlay_ui->leftSecondaryToolbarHolder;
}

QWidget* MapView::getActiveStampModeItem()
{
  auto item = stampTool->getActiveBrushItem();
  if (item)
    return item->getTool();
  else
    return nullptr;
}

void MapView::onSettingsSave()
{
  OpenGL::TerrainParamsUniformBlock* params = _world->renderer()->getTerrainParamsUniformBlock();
  params->wireframe_type = _settings->value("wireframe/type", 0).toInt();
  params->wireframe_radius = _settings->value("wireframe/radius", 1.5f).toFloat();
  params->wireframe_width = _settings->value ("wireframe/width", 1.f).toFloat();

  /* temporaryyyyyy */
  params->climb_value = 1.0f;

  QColor c = _settings->value("wireframe/color").value<QColor>();
  glm::vec4 wireframe_color(c.redF(), c.greenF(), c.blueF(), c.alphaF());
  params->wireframe_color = wireframe_color;

  _world->renderer()->markTerrainParamsUniformBlockDirty();
}

void MapView::ShowContextMenu(QPoint pos) 
{
    // QApplication::startDragDistance() is 10
    auto mouse_moved = QApplication::startDragDistance() < (_right_click_pos - pos).manhattanLength();;

    // don't show context menu if dragging mouse
    if (mouse_moved || ImGuizmo::IsUsing())
        return;

    // TODO : build the menu only once, store it and instead use setVisible ?

    QMenu* menu = new QMenu(this);

    // Undo
    QAction action_undo("Undo", this);
    menu->addAction(&action_undo);
    action_undo.setShortcut(QKeySequence::Undo);
    QObject::connect(&action_undo, &QAction::triggered, [=]()
        {
            // Same rule as the Edit-menu Ctrl+Z: spawn modes own their undo stack exclusively.
            if (terrainMode == editing_mode::creature)
            {
                if (!undoCreatureEdit())
                {
                    _main_window->statusBar()->showMessage("Nothing to undo (creature edits)", 3000);
                }
                return;
            }
            if (terrainMode == editing_mode::gameobject)
            {
                if (!undoGameObjectEdit())
                {
                    _main_window->statusBar()->showMessage("Nothing to undo (gameobject edits)", 3000);
                }
                return;
            }
            NOGGIT_ACTION_MGR->undo();
        });
    // Redo
    QAction action_redo("Redo", this);
    menu->addAction(&action_redo);
    action_redo.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    QObject::connect(&action_redo, &QAction::triggered, [=]()
        {
            NOGGIT_ACTION_MGR->redo();
        });

    menu->addSeparator();

    if (terrainMode == editing_mode::object)
    {
        bool has_selected_objects = _world->get_selected_model_count();
        bool has_copied_objects = objectEditor->clipboardSize();

        // Copy
        QAction action_8("Copy Object(s)", this);
        menu->addAction(&action_8);
        action_8.setEnabled(has_selected_objects);
        action_8.setShortcut(QKeySequence::Copy);
        QObject::connect(&action_8, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                    objectEditor->copy_current_selection(_world.get());
            });

        // Paste
        QAction action_9("Paste Object(s)", this);
        menu->addAction(&action_9);
        action_9.setEnabled(has_copied_objects);
        action_9.setShortcut(QKeySequence::Paste); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_9, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                    objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        // Delete
        QAction action_10("Delete Object(s)", this);
        menu->addAction(&action_10);
        action_10.setEnabled(has_selected_objects);
        action_10.setShortcut(QKeySequence::Delete); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_10, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                    DeleteSelectedObjects();
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        // Duplicate
        QAction action_11("Duplicate Object(s)", this);
        menu->addAction(&action_11);
        action_11.setEnabled(has_copied_objects);
        action_11.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B)); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_11, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED);
                    objectEditor->copy_current_selection(_world.get());
                    objectEditor->pasteObject(_cursor_pos, _camera.position, _world.get(), &_object_paste_params);
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        menu->addSeparator();

        // selection stuff
        QAction action_1("Select all Like Selected", this); // select all objects with the same model
        action_1.setToolTip("Warning : Doing actions on models overlapping unloaded tiles can cause crash");
        menu->addAction(&action_1);
        action_1.setEnabled(_world->get_selected_model_count() == 1);
        QObject::connect(&action_1, &QAction::triggered, [=]()
            {
                auto last_entry = _world->get_last_selected_model();
                if (last_entry)
                {
                    if (!last_entry.value().index() == eEntry_Object)
                        return;

                    auto obj = std::get<selected_object_type>(last_entry.value());
                    auto model_name = obj->instance_model()->file_key().filepath();
                    // auto models = _world->get_models_by_filename()[model_name];

                    _world->reset_selection();

                    if (obj->which() == eMODEL)
                    {
                        _world->getModelInstanceStorage().for_each_m2_instance([&](ModelInstance& model_instance)
                            {
                                if (model_instance.instance_model()->file_key().filepath() == model_name)
                                {
                                    // objects_to_select.push_back(model_instance.uid);
                                    _world->add_to_selection(&model_instance);
                                }
                            });
                    }
                    else if (obj->which() == eWMO)
                        _world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& wmo_instance)
                            {
                                if (wmo_instance.instance_model()->file_key().filepath() == model_name)
                                {
                                    // objects_to_select.push_back(wmo_instance.uid);
                                    _world->add_to_selection(&wmo_instance);
                                }
                            });

                    // for (auto uid_it = objects_to_select.begin(); uid_it != objects_to_select.end(); uid_it++)
                    // {
                    //     auto instance = _world->getObjectInstance(*uid_it);
                    //     // if (!_world->is_selected(instance))
                    //         _world->add_to_selection(instance);
                    // }
                }
            });

        QAction action_2("Hide Selected Objects", this);
        menu->addAction(&action_2);
        action_2.setEnabled(has_selected_objects);
        action_2.setShortcut(Qt::Key_H);
        QObject::connect(&action_2, &QAction::triggered, [=]()
            {
                if (_world->has_selection())
                {
                    for (auto& obj : _world->get_selected_objects())
                    {
                        if (obj->which() == eMODEL)
                            static_cast<ModelInstance*>(obj)->model->hide();
                        else if (obj->which() == eWMO)
                            static_cast<WMOInstance*>(obj)->wmo->hide();
                    }
                }
            });

        QAction action_3("Hide Unselected Objects", this);


        // QAction action_2("Show Hidden", this);

        QAction action_palette_add("Add Object To Palette", this);
        menu->addAction(&action_palette_add);
        action_palette_add.setEnabled(_world->get_selected_model_count() == 1);
        QObject::connect(&action_palette_add, &QAction::triggered, [=]()
            {
                auto last_entry = _world->get_last_selected_model();
                if (last_entry)
                {
                    if (!last_entry.value().index() == eEntry_Object)
                        return;

                    getObjectPalette()->setVisible(true);
                    auto obj = std::get<selected_object_type>(last_entry.value());
                    auto model_name = obj->instance_model()->file_key().filepath();
                    _object_palette->addObjectByFilename(model_name.c_str());
                }

            });

        menu->addSeparator();

        // allow replacing all selected?
        QAction action_replace("Replace Models (By Clipboard)", this);
        menu->addAction(&action_replace);
        action_replace.setEnabled(has_selected_objects && objectEditor->clipboardSize() == 1);
        action_replace.setToolTip("Replace the currently selected objects by the object in the clipboard (There must only be one!). M2s can only be replaced by m2s");
        QObject::connect(&action_replace, &QAction::triggered, [=]()
            {
                if (terrainMode != editing_mode::object && NOGGIT_CUR_ACTION)
                    return;

                // verify this
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_ADDED | Noggit::ActionFlags::eOBJECTS_REMOVED); // Noggit::ActionFlags::eOBJECTS_TRANSFORMED
                // NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);

                // get the model to replace by
                auto replace_select = objectEditor->getClipboard().front();
                auto replace_obj = std::get<selected_object_type>(replace_select);
                // bool replace_is_wmo = replace_obj->which() == eWMO;
                auto replace_path = replace_obj->instance_model()->file_key();

                // iterate selection (objects to replace)
                for (auto& source_obj : _world->get_selected_objects())
                {

                        math::degrees::vec3 source_rot(math::degrees(0)._, math::degrees(0)._, math::degrees(0)._);
                        source_rot = source_obj->dir;
                        float source_scale = source_obj->scale;
                        auto source_pos = source_obj->pos;

                        if (source_obj->instance_model()->file_key().filepath() == replace_path)
                            continue;

                        // TODO : Test if this breaks if clipboard is empty

                        if (replace_obj->which() == eWMO)
                        {
                            // if (!replace_is_wmo)
                            //     continue;

                            // auto replace_wmo = static_cast<WMOInstance*>(replace_obj);
                            // auto source_wmo = static_cast<WMOInstance*>(source_obj);

                            auto new_obj = _world->addWMOAndGetInstance(replace_path, source_pos, source_rot);
                            new_obj->wmo->wait_until_loaded();
                            new_obj->wmo->waitForChildrenLoaded();
                            new_obj->recalcExtents();

                        }
                        else if (replace_obj->which() == eMODEL)
                        {
                            // if (replace_is_wmo)
                            //     continue;

                            // auto replace_m2 = static_cast<ModelInstance*>(replace_obj);
                            // auto source_m2 = static_cast<ModelInstance*>(source_obj);

                            // Just swapping model
                            // Issue : doesn't work with actions
                            // _world->updateTilesEntry(entry, model_update::remove);
                            // source_m2->model = scoped_model_reference(replace_path, _context);
                            // source_m2->recalcExtents();
                            // _world->updateTilesEntry(entry, model_update::add);
                            

                            auto new_obj = _world->addM2AndGetInstance(replace_path
                                , source_pos
                                , source_scale
                                , source_rot
                                , &_object_paste_params
                                , true
                            );
                            new_obj->model->wait_until_loaded();
                            new_obj->model->waitForChildrenLoaded();
                            new_obj->recalcExtents();
                        }
                }
                // can cause the usual crash of deleting models overlapping unloaded tiles.
                DeleteSelectedObjects();
                // NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                NOGGIT_ACTION_MGR->endAction();
            });

        QAction action_snap("Snap Selected To Ground", this);
        menu->addAction(&action_snap);
        action_snap.setEnabled(has_selected_objects);
        action_snap.setShortcut(Qt::Key_PageDown); // (Qt::CTRL | Qt::Key_P)
        QObject::connect(&action_snap, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION)
                {
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                    snap_selected_models_to_the_ground();
                    NOGGIT_ACTION_MGR->endAction();
                }
            });

        QAction action_save_obj_coords("Save objects coords(to file)", this);
        menu->addAction(&action_save_obj_coords);
        action_save_obj_coords.setEnabled(has_selected_objects);
        QObject::connect(&action_save_obj_coords, &QAction::triggered, [=]()
            {
                if (terrainMode == editing_mode::object)
                {
                    if (_world->has_selection() && _world->get_selected_model_count())
                    {
                        std::stringstream obj_data;
                        for (auto& obj : _world->get_selected_objects())
                        {
                            obj_data << "\"Object : " << obj->instance_model()->file_key().filepath() << "(UID :" << obj->uid << ")\"," << std::endl;
                            obj_data << "\"Scale : " << obj->scale << "\"," << std::endl;
                            // coords string in ts-wow format
                            obj_data << "\"Coords(server): {map:" << _world->getMapID() << ",x:" << (ZEROPOINT - obj->pos.z) << ",y:" << (ZEROPOINT - obj->pos.x)
                                << ",z:" << obj->pos.y << ",o:";

                            float server_rot = 2 * glm::pi<float>() - glm::pi<float>() / 180.0 * (float(obj->dir.y) < 0 ? fabs(float(obj->dir.y)) + 180.0 : fabs(float(obj->dir.y) - 180.0));
                            // float server_rot = glm::radians(obj->dir.y) + glm::radians(180.f);

                            obj_data << server_rot << "}\"," << std::endl;

                            /// converting db gobject rotation to noggit. Keep commented for later usage
                            /*
                            glm::quat test_db_quat = glm::quat(1.0, 1.0, 1.0, 1.0);
                            test_db_quat.x = 0.607692, test_db_quat.y = -0.361538, test_db_quat.z = 0.607693, test_db_quat.w = 0.361539;
                            glm::vec3 rot_euler = glm::eulerAngles(test_db_quat);
                            glm::vec3 rot_degrees = glm::degrees(rot_euler); 
                            rot_degrees = glm::vec3(rot_degrees.y, rot_degrees.z - 180.f, rot_degrees.x); // final noggit coords
                            */

                            glm::quat rot_quat = glm::quat(glm::vec3(glm::radians(obj->dir.z), glm::radians(obj->dir.x), server_rot));
                            auto normalized_quat = glm::normalize(rot_quat);

                            obj_data << "\"Rotation (server quaternion): {x:" << normalized_quat.x << ",y:" << normalized_quat.y << ",z:" << normalized_quat.z
                                << ",w:" << normalized_quat.w << "}\"," <<  std::endl << "\n";
                        }

                        std::ofstream f("saved_objects_data.txt", std::ios_base::app);
                        f << "\"Saved " << _world->get_selected_model_count() << " objects at : " << QDateTime::currentDateTime().toString("dd MMMM yyyy hh:mm:ss").toStdString() << "\"" << std::endl;
                        f << obj_data.str();
                        f.close();
                    }
                }
            });

        menu->addSeparator();
        // TODO
        QAction action_group("Group Selected Objects", this);
        menu->addAction(&action_group);
        // check if all selected objects are already grouped
        bool groupable = false; 
        if ( _world->has_multiple_model_selected())
        {
            // if there's no existing groups, that means it's always groupable
            if (!_world->_selection_groups.size())
                groupable = true;

            if (!groupable)
            {
                // check if there's any ungrouped object
                for (auto obj : _world->get_selected_objects())
                {
                    bool obj_ungrouped = true;
                    for (auto& group : _world->_selection_groups)
                    {
                        if (group.contains_object(obj))
                            obj_ungrouped = false;
                    }
                    if (obj_ungrouped)
                    {
                        groupable = true;
                        break;
                    }
                }
            }
        }
        action_group.setEnabled(groupable);
        QObject::connect(&action_group, &QAction::triggered, [=]()
            {
                // remove all groups the objects are already in and create a new one
                // for (auto obj : _world->get_selected_objects())
                // {
                //     for (auto& group : _world->_selection_groups)
                //     {
                //         if (group.contains_object(obj))
                //         {
                //             group.remove_group();
                //         }
                //     }
                // }
                for (auto& group : _world->_selection_groups)
                {
                    if (group.isSelected())
                    {
                        group.remove_group();
                    }
                }

                _world->add_object_group_from_selection();
            });


        QAction action_ungroup("Ungroup Selected Objects", this);
        menu->addAction(&action_ungroup);
        bool group_selected = false;
        for (auto& group : _world->_selection_groups)
        {
            if (group.isSelected())
            {
                group_selected = true;
                break;
            }
        }
        action_ungroup.setEnabled(group_selected);
        QObject::connect(&action_ungroup, &QAction::triggered, [=]()
            {
                _world->clear_selection_groups();
            });


        menu->exec(mapToGlobal(pos)); // synch
        // menu->popup(mapToGlobal(pos)); // asynch, needs to be preloaded to work
    };

}
