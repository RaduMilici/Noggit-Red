// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/mcp/McpServer.hpp>

#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/SceneObject.hpp>
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
      // execute. KNOWN LIMITATION: models placed via add_m2 do NOT render LIVE in the editor -- an
      // unresolved GL-upload bug (their buffers fail to bind at draw; identical hand-pasted models
      // render fine, cause not found across 4 fixes + 3 traces). They ARE written to the map correctly
      // (query_objects sees them) and appear after SAVE + tile reload. Live in-editor render is open.
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

    QJsonObject McpServer::cmd_focus_camera(QJsonObject const& req)
    {
      glm::vec3 const target(
          static_cast<float>(req.value("x").toDouble())
        , static_cast<float>(req.value("y").toDouble())
        , static_cast<float>(req.value("z").toDouble()));

      // Reuse the tested framing path (loads the tile, places the eye back+up at ~45 deg, aims).
      _view->focus_camera_on_target(target);
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
