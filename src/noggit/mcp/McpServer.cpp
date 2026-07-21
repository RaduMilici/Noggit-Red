// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/mcp/McpServer.hpp>

#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/SceneObject.hpp>
#include <noggit/ModelInstance.h>
#include <noggit/Model.h>
#include <noggit/WMOInstance.h>
#include <noggit/WMO.h>
#include <noggit/Brush.h>
#include <noggit/TextureManager.h>
#include <noggit/ContextObject.hpp>
#include <noggit/SceneObject.hpp>
#include <noggit/MapChunk.h>
#include <noggit/tool_enums.hpp>
#include <noggit/map_enums.hpp>
#include <math/trig.hpp>

#include <QImage>
#include <QFile>
#include <set>
#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_context.hpp>

#include <opengl/scoped.hpp>
#include <opengl/context.hpp>

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QString>
#include <QDebug>

#include <glm/vec3.hpp>

#include <exception>
#include <string>
#include <algorithm>
#include <cmath>

namespace Noggit
{
  namespace Mcp
  {
    namespace
    {
      quint16 resolve_port()
      {
        quint16 const fallback = 8172;
        QByteArray const env = qgetenv("NOGGIT_MCP_PORT");
        if (env.isEmpty())
          return fallback;

        bool ok = false;
        int const p = QString::fromLocal8Bit(env).toInt(&ok);
        if (ok && p > 0 && p < 65536)
          return static_cast<quint16>(p);
        return fallback;
      }

      QJsonObject make_error(QString const& message)
      {
        QJsonObject o;
        o["ok"] = false;
        o["error"] = message;
        return o;
      }

      QJsonObject make_ok()
      {
        QJsonObject o;
        o["ok"] = true;
        return o;
      }
    }

    McpServer::McpServer(MapView* view, QObject* parent)
      : QObject(parent)
      , _view(view)
      , _server(new QTcpServer(this))
      , _port(resolve_port())
    {
      connect(_server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);

      if (_server->listen(QHostAddress::LocalHost, _port))
        qInfo().noquote() << QString("[MCP] listening on 127.0.0.1:%1").arg(_port);
      else
        qWarning().noquote() << QString("[MCP] failed to listen on 127.0.0.1:%1 - %2")
                                  .arg(_port).arg(_server->errorString());
    }

    McpServer::~McpServer() = default;

    bool McpServer::isListening() const { return _server && _server->isListening(); }
    quint16 McpServer::port() const { return _port; }

    void McpServer::onNewConnection()
    {
      while (QTcpSocket* socket = _server->nextPendingConnection())
      {
        connect(socket, &QTcpSocket::readyRead, this, &McpServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    }

    void McpServer::onReadyRead()
    {
      QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
      if (!socket)
        return;

      while (socket->canReadLine())
      {
        QByteArray const line = socket->readLine().trimmed();
        if (line.isEmpty())
          continue;

        QJsonObject response;
        QJsonParseError parse_error{};
        QJsonDocument const doc = QJsonDocument::fromJson(line, &parse_error);

        if (parse_error.error != QJsonParseError::NoError || !doc.isObject())
        {
          response = make_error("invalid JSON: " + parse_error.errorString());
        }
        else
        {
          try
          {
            response = dispatch(doc.object());
          }
          catch (std::exception const& e)
          {
            response = make_error(QString("exception: ") + e.what());
          }
          catch (...)
          {
            response = make_error("unknown exception");
          }
        }

        QByteArray out = QJsonDocument(response).toJson(QJsonDocument::Compact);
        out.append('\n');
        socket->write(out);
        socket->flush();
      }
    }

    QJsonObject McpServer::dispatch(QJsonObject const& req)
    {
      QString const cmd = req.value("cmd").toString();

      if (!_view || !_view->getWorld())
        return make_error("no map open");

      if (cmd == "ping")          return cmd_ping(req);
      if (cmd == "run_lua")       return cmd_run_lua(req);
      if (cmd == "place_model")   return cmd_place_model(req);
      if (cmd == "place_wmo")     return cmd_place_wmo(req);
      if (cmd == "change_terrain")return cmd_change_terrain(req);
      if (cmd == "blur_terrain")  return cmd_blur_terrain(req);
      if (cmd == "flatten_terrain")return cmd_flatten_terrain(req);
      if (cmd == "paint_texture") return cmd_paint_texture(req);
      if (cmd == "import_heightmap") return cmd_import_heightmap(req);
      if (cmd == "import_heightmap_raw") return cmd_import_heightmap_raw(req);
      if (cmd == "clear_textures") return cmd_clear_textures(req);
      if (cmd == "autotexture")   return cmd_autotexture(req);
      if (cmd == "add_water")     return cmd_add_water(req);
      if (cmd == "edit_model")    return cmd_edit_model(req);
      if (cmd == "delete_model")  return cmd_delete_model(req);
      if (cmd == "focus_camera")  return cmd_focus_camera(req);
      if (cmd == "query_objects") return cmd_query_objects(req);
      if (cmd == "height_at")     return cmd_height_at(req);
      if (cmd == "save")          return cmd_save(req);
      if (cmd == "undo")          return cmd_undo(req);
      if (cmd == "redo")          return cmd_redo(req);

      return make_error("unknown cmd: '" + cmd + "'");
    }

    QJsonObject McpServer::cmd_ping(QJsonObject const&)
    {
      QJsonObject o = make_ok();
      o["result"] = "pong";
      o["port"] = _port;
      return o;
    }

    QJsonObject McpServer::cmd_run_lua(QJsonObject const& req)
    {
      std::string const code = req.value("code").toString().toStdString();
      if (code.empty())
        return make_error("run_lua: empty 'code'");

      // scriptingTool exists as soon as a map is open (created in MapView::setupScriptingUi);
      // its script_context is the live sol::state already bound to this MapView's World.
      Noggit::Scripting::scripting_tool* tool = _view->scriptingTool;
      if (!tool)
        return make_error("run_lua: no scripting tool");
      Noggit::Scripting::script_context* ctx = tool->get_context();
      if (!ctx)
        return make_error("run_lua: no script context");

      // Broad flag set so any edit the Lua performs (terrain/texture/objects/...) is recorded into
      // the single action -> the whole call is ONE undo step. add_m2/add_wmo also register into the
      // active action (world_model_instances_storage), so placed models are undoable too.
      int const flags =
          Noggit::ActionFlags::eOBJECTS_ADDED
        | Noggit::ActionFlags::eOBJECTS_REMOVED
        | Noggit::ActionFlags::eOBJECTS_TRANSFORMED
        | Noggit::ActionFlags::eCHUNKS_TERRAIN
        | Noggit::ActionFlags::eCHUNKS_TEXTURE
        | Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR
        | Noggit::ActionFlags::eCHUNKS_AREAID
        | Noggit::ActionFlags::eCHUNKS_HOLES
        | Noggit::ActionFlags::eCHUNKS_WATER
        | Noggit::ActionFlags::eCHUNKS_FLAGS;

      // Make this MapView's GL context current so gl.* calls in the script (terrain/texture brushes)
      // execute. NOTE: models placed via Lua add_m2 still don't render live (add_m2 omits
      // waitForChildrenLoaded) -- use the dedicated `place_model` command for live M2 placement, which
      // mirrors the Ctrl+V paste path. run_lua remains the escape hatch for scripted/bulk terrain ops.
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl_setter(::gl, _view->context());

      QString error;
      QString result_str;
      NOGGIT_ACTION_MGR->beginAction(_view, flags);
      try
      {
        sol::protected_function_result res = ctx->safe_script(code, sol::script_pass_on_error);
        if (!res.valid())
        {
          sol::error const err = res;
          error = QString("lua error: ") + err.what();
        }
        else
        {
          // Return channel: hand the script's return value back to the caller so it can self-verify
          // (e.g. `run_lua("... return count")` -> {"ok":true,"result":"5"}). tostring() handles any type.
          sol::object ret = res;
          if (ret.valid() && ret.get_type() != sol::type::nil)
          {
            sol::protected_function tostring = (*ctx)["tostring"];
            std::string const s = tostring(ret);
            result_str = QString::fromStdString(s);
          }
        }
      }
      catch (std::exception const& e)
      {
        error = QString("lua exception: ") + e.what();
      }
      catch (...)
      {
        error = "lua unknown exception";
      }
      // Always close the action, even on error, so we never leave a dangling open action.
      NOGGIT_ACTION_MGR->endAction();

      // Added/removed models are pushed to the per-tile RENDER lists by a background queue that
      // add_m2/updateTilesModel only ENQUEUE. The world data (what query_objects reads) updates
      // synchronously, but the viewport won't draw the model until that queue drains -- so drain it
      // here (the same thing saveTile/saveChanged do) or placements stay invisible until a reload.
      _view->getWorld()->wait_for_all_tile_updates();

      _view->requestRedraw();
      _view->update();

      if (!error.isEmpty())
        return make_error(error);
      QJsonObject o = make_ok();
      if (!result_str.isEmpty())
        o["result"] = result_str;
      return o;
    }

    QJsonObject McpServer::cmd_place_model(QJsonObject const& req)
    {
      std::string const path = req.value("path").toString().toStdString();
      if (path.empty())
        return make_error("place_model: empty 'path'");

      float const x     = static_cast<float>(req.value("x").toDouble());
      float const y     = static_cast<float>(req.value("y").toDouble());
      float const z     = static_cast<float>(req.value("z").toDouble());
      float const scale = static_cast<float>(req.value("scale").toDouble(1.0));
      float const roty  = static_cast<float>(req.value("rotation").toDouble(0.0));

      // Mirror the interactive Ctrl+V paste (ObjectEditor::pasteObject) EXACTLY -- the path that renders
      // LIVE. Crucially NO makeCurrent and NO wait_for_all_tile_updates (paste does neither; the frame
      // timer draws it), plus the full readiness sequence INCLUDING waitForChildrenLoaded, which the Lua
      // add_m2 omitted -- the one difference that left add_m2 models unbindable at draw until a reload.
      ModelInstance* inst = nullptr;
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_ADDED);
      try
      {
        inst = _view->getWorld()->addM2AndGetInstance(
            path, glm::vec3(x, y, z), scale,
            math::degrees::vec3(glm::vec3(0.f, roty, 0.f)), nullptr, false);
        if (inst)
        {
          inst->model->wait_until_loaded();
          inst->model->waitForChildrenLoaded();
          inst->recalcExtents();
        }
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("place_model: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->requestRedraw();
      _view->update();

      QJsonObject o = make_ok();
      if (inst)
        o["uid"] = static_cast<double>(inst->uid);
      return o;
    }

    QJsonObject McpServer::cmd_place_wmo(QJsonObject const& req)
    {
      std::string const path = req.value("path").toString().toStdString();
      if (path.empty())
        return make_error("place_wmo: empty 'path'");

      float const x   = static_cast<float>(req.value("x").toDouble());
      float const y   = static_cast<float>(req.value("y").toDouble());
      float const z   = static_cast<float>(req.value("z").toDouble());
      float const rx  = static_cast<float>(req.value("rx").toDouble(0.0));
      float const ry  = static_cast<float>(req.value("ry").toDouble(0.0));
      float const rz  = static_cast<float>(req.value("rz").toDouble(0.0));

      // Mirror ObjectEditor::pasteObject's WMO branch exactly so buildings render live.
      WMOInstance* inst = nullptr;
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_ADDED);
      try
      {
        inst = _view->getWorld()->addWMOAndGetInstance(
            path, glm::vec3(x, y, z),
            math::degrees::vec3(glm::vec3(rx, ry, rz)));
        if (inst)
        {
          inst->wmo->wait_until_loaded();
          inst->wmo->waitForChildrenLoaded();
          inst->recalcExtents();
        }
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("place_wmo: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->requestRedraw();
      _view->update();

      QJsonObject o = make_ok();
      if (inst)
        o["uid"] = static_cast<double>(inst->uid);
      return o;
    }

    QJsonObject McpServer::cmd_change_terrain(QJsonObject const& req)
    {
      float const x       = static_cast<float>(req.value("x").toDouble());
      float const y       = static_cast<float>(req.value("y").toDouble());
      float const z       = static_cast<float>(req.value("z").toDouble());
      float const change  = static_cast<float>(req.value("change").toDouble(20.0));   // + raises, - lowers
      float const radius  = static_cast<float>(req.value("radius").toDouble(40.0));
      float const inner   = static_cast<float>(req.value("inner_radius").toDouble(0.0));
      int   const brush   = req.value("brush_type").toInt(2);   // eTerrainType: 2=Smooth, 6=Gaussian

      // Terrain edits are synchronous vertex changes on the GUI thread (registerChunkUpdate ->
      // re-upload in the next paintGL), so they render live with just context + action + redraw.
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
      try
      {
        _view->getWorld()->changeTerrain(glm::vec3(x, y, z), change, radius, brush, inner);
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("change_terrain: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_blur_terrain(QJsonObject const& req)
    {
      float const x      = static_cast<float>(req.value("x").toDouble());
      float const y      = static_cast<float>(req.value("y").toDouble());
      float const z      = static_cast<float>(req.value("z").toDouble());
      float const remain = static_cast<float>(req.value("remain").toDouble(0.5));  // 0..1 blend strength
      float const radius = static_cast<float>(req.value("radius").toDouble(40.0));
      int   const brush  = req.value("brush_type").toInt(2);  // eFlattenType: 0 Flat,1 Linear,2 Smooth

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
      try
      {
        // raise+lower both true -> softens the surface toward the local neighbourhood average,
        // rounding off cliffs and sharp edges left by the Smooth-brush cutoff.
        _view->getWorld()->blurTerrain(glm::vec3(x, y, z), remain, radius, brush, flatten_mode(true, true));
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("blur_terrain: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_flatten_terrain(QJsonObject const& req)
    {
      float const x       = static_cast<float>(req.value("x").toDouble());
      float const z       = static_cast<float>(req.value("z").toDouble());
      float const height  = static_cast<float>(req.value("height").toDouble());  // target y to flatten to
      float const remain  = static_cast<float>(req.value("remain").toDouble(1.0));// 1 = full flatten
      float const radius  = static_cast<float>(req.value("radius").toDouble(40.0));
      int   const brush   = req.value("brush_type").toInt(0);  // eFlattenType: 0 Flat (uniform)

      glm::vec3 const origin(x, height, z);  // origin.y is the flatten target height

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
      try
      {
        // angle=orientation=0 -> a level pad at `height`; raise+lower both -> pull terrain to it either way.
        _view->getWorld()->flattenTerrain(origin, remain, radius, brush, flatten_mode(true, true),
                                          origin, math::degrees(0.0), math::degrees(0.0));
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("flatten_terrain: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_paint_texture(QJsonObject const& req)
    {
      std::string const texture = req.value("texture").toString().toStdString();
      if (texture.empty())
        return make_error("paint_texture: empty 'texture'");

      float const x        = static_cast<float>(req.value("x").toDouble());
      float const y        = static_cast<float>(req.value("y").toDouble());
      float const z        = static_cast<float>(req.value("z").toDouble());
      float const strength = static_cast<float>(req.value("strength").toDouble(1.0));   // 0..1 coverage
      float const pressure = static_cast<float>(req.value("pressure").toDouble(0.9));
      float const hardness = static_cast<float>(req.value("hardness").toDouble(0.5));
      float const radius   = static_cast<float>(req.value("radius").toDouble(15.0));

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      try
      {
        Brush brush;
        brush.setHardness(hardness);
        brush.setRadius(radius);
        _view->getWorld()->paintTexture(
            glm::vec3(x, y, z), &brush, strength, pressure,
            scoped_blp_texture_reference(texture, Noggit::NoggitRenderContext::MAP_VIEW));
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("paint_texture: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_import_heightmap(QJsonObject const& req)
    {
      QString const path = req.value("path").toString();
      if (path.isEmpty())
        return make_error("import_heightmap: empty 'path'");

      float const x          = static_cast<float>(req.value("x").toDouble());
      float const z          = static_cast<float>(req.value("z").toDouble());
      float const multiplier = static_cast<float>(req.value("multiplier").toDouble(100.0)); // white px height
      unsigned const mode    = static_cast<unsigned>(req.value("mode").toInt(0));            // 0 Set,1 Add,...
      bool const tiled       = req.value("tiled_edges").toBool(false);

      QImage img;
      if (!img.load(path))
        return make_error("import_heightmap: failed to load image '" + path + "'");

      glm::vec3 const pos(x, 0.f, z);
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
      try
      {
        // Sets each vertex height = (pixel_gray/255) * multiplier over the whole ADT tile at pos: a
        // smooth heightfield with no brush-stacking waves. Then a zero-delta changeTerrain over the tile
        // forces normal recalculation (importADTHeightmap only flags VERTEX) so shading is correct.
        _view->getWorld()->importADTHeightmap(pos, img, multiplier, mode, tiled);
        _view->getWorld()->changeTerrain(pos, 0.0f, 800.0f, eTerrainType_Smooth, 0.0f);
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("import_heightmap: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_import_heightmap_raw(QJsonObject const& req)
    {
      // Import terrain heights at FULL FLOAT precision from a raw float32 array (NxN, row-major, row->worldZ,
      // col->worldX across the tile). Avoids the 8-bit PNG quantization that stair-steps flat ground and
      // wrecks the slopes (which locked grass out and made the rock checkerboard). Sets vertex heights
      // directly + recomputes real normals per chunk.
      QString const path = req.value("path").toString();
      if (path.isEmpty()) return make_error("import_heightmap_raw: empty 'path'");
      float const x  = static_cast<float>(req.value("x").toDouble());
      float const z  = static_cast<float>(req.value("z").toDouble());
      int   const NN = req.value("n").toInt(257);

      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return make_error("import_heightmap_raw: cannot open '" + path + "'");
      QByteArray const bytes = file.readAll();
      if (bytes.size() < static_cast<int>(NN) * NN * static_cast<int>(sizeof(float)))
        return make_error("import_heightmap_raw: file too small for a " + QString::number(NN) + "^2 float grid");
      const float* arr = reinterpret_cast<const float*>(bytes.constData());

      float const TILE = 533.33333f, CHUNK = TILE / 16.0f;
      float const xmin = std::floor(x / TILE) * TILE, zmin = std::floor(z / TILE) * TILE;
      glm::vec3 const pos(x, 0.f, z);

      World* world = _view->getWorld();
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TERRAIN);
      int set_chunks = 0;
      try
      {
        for (int l = 0; l < 16; ++l)
          for (int k = 0; k < 16; ++k)
          {
            MapChunk* c = world->getChunkAt(glm::vec3(xmin + (k + 0.5f) * CHUNK, 0.f, zmin + (l + 0.5f) * CHUNK));
            if (!c) continue;
            NOGGIT_CUR_ACTION->registerChunkTerrainChange(c);
            for (int i = 0; i < mapbufsize; ++i)
            {
              glm::vec3& v = c->mVertices[i];
              float const fc = (v.x - xmin) / TILE * (NN - 1);
              float const fr = (v.z - zmin) / TILE * (NN - 1);
              int const c0 = std::min(std::max(static_cast<int>(fc), 0), NN - 2);
              int const r0 = std::min(std::max(static_cast<int>(fr), 0), NN - 2);
              float const tc = fc - c0, tr = fr - r0;
              float const h00 = arr[r0*NN + c0],     h01 = arr[r0*NN + c0 + 1];
              float const h10 = arr[(r0+1)*NN + c0], h11 = arr[(r0+1)*NN + c0 + 1];
              v.y = h00*(1-tc)*(1-tr) + h01*tc*(1-tr) + h10*(1-tc)*tr + h11*tc*tr;
            }
            c->registerChunkUpdate(ChunkUpdateFlags::VERTEX | ChunkUpdateFlags::NORMALS);
            world->recalc_norms(c);
            ++set_chunks;
          }
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("import_heightmap_raw: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      world->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      QJsonObject o = make_ok();
      o["chunks"] = set_chunks;
      return o;
    }

    QJsonObject McpServer::cmd_clear_textures(QJsonObject const& req)
    {
      float const x = static_cast<float>(req.value("x").toDouble());
      float const z = static_cast<float>(req.value("z").toDouble());
      glm::vec3 const pos(x, 0.f, z);

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      try
      {
        _view->getWorld()->clearTextures(pos);  // erases all texture layers on the whole tile at pos
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("clear_textures: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_autotexture(QJsonObject const& req)
    {
      // DEV-LEARNED model: a biome BASE layer (dirt/snow, NOT grass), then GRASS on flat + ROCK on steep
      // as SMOOTH per-vertex alpha weights that follow slope. Smooth weights (not hard dabs) = no checkerboard.
      std::string base  = req.value("base").toString().toStdString();   // ground base (dirt/snow per biome)
      std::string const grass = req.value("grass").toString().toStdString();  // flat overlay
      std::string const rock  = req.value("rock").toString().toStdString();   // steep overlay
      std::string       sand  = req.value("sand").toString().toStdString();   // water bed/shore
      if (base.empty()) base = req.value("dirt").toString().toStdString();     // back-compat
      if (base.empty()) base = grass;
      if (base.empty()) return make_error("autotexture: need a 'base' texture");
      if (sand.empty()) sand = base;

      float const x      = static_cast<float>(req.value("x").toDouble());
      float const z      = static_cast<float>(req.value("z").toDouble());
      float const radius = static_cast<float>(req.value("radius").toDouble(300.0));
      // normal-up (1=flat): grass fades in above t_flat, rock fades in below t_steep; base shows between.
      float const t_flat  = static_cast<float>(req.value("slope_flat").toDouble(0.93));
      float const t_steep = static_cast<float>(req.value("slope_steep").toDouble(0.66));
      float const water_level = static_cast<float>(req.value("water_level").toDouble(-1e9));
      float const shore  = static_cast<float>(req.value("shore_height").toDouble(2.5));
      float const nz     = static_cast<float>(req.value("noise").toDouble(0.04));

      auto h1 = [](float a, float b){ float s = std::sin(a*12.9898f + b*78.233f)*43758.55f; return s - std::floor(s); };
      auto sstep = [](float a, float b, float xx){ float t = (xx-a)/(b-a); t = t<0?0:(t>1?1:t); return t*t*(3.f-2.f*t); };

      World* world = _view->getWorld();
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());

      std::set<MapChunk*> chunks;
      for (float dx = -radius; dx <= radius; dx += 16.0f)
        for (float dz = -radius; dz <= radius; dz += 16.0f)
          if (MapChunk* c = world->getChunkAt(glm::vec3(x + dx, 0.f, z + dz)))
            chunks.insert(c);

      int painted_accents = 0;
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      try
      {
        Brush bb; bb.setHardness(1.0f); bb.setRadius(60.0f);   // uniform BASE layer
        Brush acc; acc.setHardness(0.5f); acc.setRadius(16.0f); // wider -> can stride verts (fewer paint calls)

        for (MapChunk* c : chunks)
        {
          world->paintTexture(c->vcenter, &bb, 255.0f, 1.0f,
              scoped_blp_texture_reference(base, Noggit::NoggitRenderContext::MAP_VIEW));

          for (int i = 0; i < mapbufsize; i += 2)   // stride 2 (radius 16 still overlaps) -> ~half the paints
          {
            glm::vec3 const v = c->mVertices[i];
            float ny = c->mNormals[i].y;
            int lo = (i >= 1) ? i - 1 : i, hi = (i + 1 < mapbufsize) ? i + 1 : i;
            ny = (ny + c->mNormals[lo].y + c->mNormals[hi].y) / 3.0f;      // smoothed slope
            float const nyj = ny + (h1(v.x, v.z) - 0.5f) * nz;            // tiny organic jitter

            // WATER: sand bed + smooth shoreline
            if (water_level > -1e8f && v.y <= water_level + shore)
            {
              float wsand = (v.y <= water_level + 0.4f) ? 1.0f : (1.0f - (v.y - water_level) / std::max(0.1f, shore));
              world->paintTexture(v, &acc, 255.0f * std::min(1.0f, std::max(0.0f, wsand)), 0.85f,
                  scoped_blp_texture_reference(sand, Noggit::NoggitRenderContext::MAP_VIEW));
              ++painted_accents; continue;
            }
            // SMOOTH slope-driven overlays (weights vary gradually -> feathered blend, like the devs)
            float const wf = grass.empty() ? 0.f : sstep(t_flat - 0.07f, t_flat + 0.03f, nyj);      // grass on flat
            float const wr = rock.empty()  ? 0.f : sstep(t_steep + 0.11f, t_steep - 0.05f, nyj);    // rock on steep
            if (wf > 0.03f)
            { world->paintTexture(v, &acc, 255.0f * wf, 0.85f,
                scoped_blp_texture_reference(grass, Noggit::NoggitRenderContext::MAP_VIEW)); ++painted_accents; }
            if (wr > 0.03f)
            { world->paintTexture(v, &acc, 255.0f * wr, 0.85f,
                scoped_blp_texture_reference(rock, Noggit::NoggitRenderContext::MAP_VIEW)); ++painted_accents; }
          }
        }
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("autotexture: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      world->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();

      QJsonObject o = make_ok();
      o["chunks"] = static_cast<int>(chunks.size());
      o["accents"] = painted_accents;
      return o;
    }

    QJsonObject McpServer::cmd_add_water(QJsonObject const& req)
    {
      float const x      = static_cast<float>(req.value("x").toDouble());
      float const y      = static_cast<float>(req.value("y").toDouble());
      float const z      = static_cast<float>(req.value("z").toDouble());
      float const radius = static_cast<float>(req.value("radius").toDouble(40.0));
      float const height = static_cast<float>(req.value("height").toDouble(y));  // flat water level
      int   const liquid = req.value("liquid_id").toInt(2);                      // LiquidType.dbc id

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eCHUNKS_WATER);
      try
      {
        // add=true, lock+override_height with origin.y=height => flat water plane at `height`
        // (same recipe as vert::set_water). override_liquid_id=true forces our chosen type.
        _view->getWorld()->paintLiquid(
            glm::vec3(x, y, z), radius, liquid, true,
            math::radians(0.f), math::radians(0.f), true,
            glm::vec3(0.f, height, 0.f), true, true, 1.0f);
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("add_water: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      _view->getWorld()->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_edit_model(QJsonObject const& req)
    {
      if (!req.contains("uid"))
        return make_error("edit_model: missing 'uid'");
      std::uint32_t const uid = static_cast<std::uint32_t>(req.value("uid").toDouble());

      World* world = _view->getWorld();
      SceneObject* obj = world->getObjectInstance(uid);
      if (!obj)
        return make_error(QString("edit_model: no object with uid %1").arg(uid));

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
      try
      {
        // Mirror World::set_model_pos: pull the instance out of its old tile, mutate, re-add to the
        // (possibly new) tile so the render lists follow the object. registerObjectTransformed = undo.
        world->updateTilesEntry(obj, model_update::remove);
        NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

        if (req.contains("x") && req.contains("y") && req.contains("z"))
          obj->pos = glm::vec3(static_cast<float>(req.value("x").toDouble()),
                               static_cast<float>(req.value("y").toDouble()),
                               static_cast<float>(req.value("z").toDouble()));

        if (req.contains("rx") || req.contains("ry") || req.contains("rz"))
          obj->dir = math::degrees::vec3(glm::vec3(
              static_cast<float>(req.value("rx").toDouble(obj->dir.x)),
              static_cast<float>(req.value("ry").toDouble(obj->dir.y)),
              static_cast<float>(req.value("rz").toDouble(obj->dir.z))));

        if (req.contains("scale") && obj->which() == eMODEL)
          obj->scale = static_cast<float>(req.value("scale").toDouble(obj->scale));

        obj->recalcExtents();
        world->updateTilesEntry(obj, model_update::add);
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("edit_model: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      world->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();

      QJsonObject o = make_ok();
      o["uid"]   = static_cast<double>(obj->uid);
      o["x"]     = obj->pos.x;
      o["y"]     = obj->pos.y;
      o["z"]     = obj->pos.z;
      o["scale"] = obj->scale;
      return o;
    }

    QJsonObject McpServer::cmd_delete_model(QJsonObject const& req)
    {
      if (!req.contains("uid"))
        return make_error("delete_model: missing 'uid'");
      std::uint32_t const uid = static_cast<std::uint32_t>(req.value("uid").toDouble());

      World* world = _view->getWorld();
      if (!world->getObjectInstance(uid))
        return make_error(QString("delete_model: no object with uid %1").arg(uid));

      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl(::gl, _view->context());
      NOGGIT_ACTION_MGR->beginAction(_view, Noggit::ActionFlags::eOBJECTS_REMOVED);
      try
      {
        // World::deleteInstance -> storage.delete_instance already clears the render tile entry
        // (updateTilesEntry remove) and registers the removal for undo when an action is active.
        world->deleteInstance(static_cast<int>(uid));
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        return make_error(QString("delete_model: ") + e.what());
      }
      NOGGIT_ACTION_MGR->endAction();

      world->wait_for_all_tile_updates();
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_focus_camera(QJsonObject const& req)
    {
      glm::vec3 const target(
          static_cast<float>(req.value("x").toDouble())
        , static_cast<float>(req.value("y").toDouble())
        , static_cast<float>(req.value("z").toDouble()));

      // Default to a HIGH, steep overview so builds are framed from above, not aimed into the dirt.
      // distance = eye-to-target range; pitch = look-down angle (bigger => higher eye). Scale distance
      // up for larger scenes.
      float const distance = static_cast<float>(req.value("distance").toDouble(130.0));
      float const pitch    = static_cast<float>(req.value("pitch").toDouble(55.0));

      _view->focus_camera_on_target(target, distance, pitch);
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_query_objects(QJsonObject const& req)
    {
      glm::vec3 const p(
          static_cast<float>(req.value("x").toDouble())
        , static_cast<float>(req.value("y").toDouble())
        , static_cast<float>(req.value("z").toDouble()));
      float const radius = static_cast<float>(req.value("radius").toDouble(50.0));

      std::vector<SceneObject*> const objects = _view->getWorld()->getObjectsInRange(p, radius);

      QJsonArray arr;
      for (SceneObject* o : objects)
      {
        if (!o)
          continue;
        QJsonObject j;
        j["uid"] = static_cast<double>(o->uid);
        j["type"] = (o->which() == eMODEL) ? "m2" : "wmo";
        j["x"] = o->pos.x;
        j["y"] = o->pos.y;
        j["z"] = o->pos.z;
        j["scale"] = o->scale;
        arr.append(j);
      }

      QJsonObject o = make_ok();
      o["count"] = arr.size();
      o["objects"] = arr;
      return o;
    }

    QJsonObject McpServer::cmd_height_at(QJsonObject const& req)
    {
      glm::vec3 const q(
          static_cast<float>(req.value("x").toDouble())
        , 0.f
        , static_cast<float>(req.value("z").toDouble()));

      glm::vec3 const g = _view->getWorld()->get_ground_height(q);

      QJsonObject o = make_ok();
      o["x"] = g.x;
      o["y"] = g.y;
      o["z"] = g.z;
      o["height"] = g.y;
      return o;
    }

    QJsonObject McpServer::cmd_save(QJsonObject const&)
    {
      _view->save(save_mode::changed);
      return make_ok();
    }

    QJsonObject McpServer::cmd_undo(QJsonObject const&)
    {
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl_setter(::gl, _view->context());
      NOGGIT_ACTION_MGR->undo();
      _view->getWorld()->wait_for_all_tile_updates();  // drain tile queue so the change renders live
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }

    QJsonObject McpServer::cmd_redo(QJsonObject const&)
    {
      _view->makeCurrent();
      OpenGL::context::scoped_setter const _gl_setter(::gl, _view->context());
      NOGGIT_ACTION_MGR->redo();
      _view->getWorld()->wait_for_all_tile_updates();  // drain tile queue so the change renders live
      _view->requestRedraw();
      _view->update();
      return make_ok();
    }
  } // namespace Mcp
} // namespace Noggit
