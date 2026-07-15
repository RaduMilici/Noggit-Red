// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <QObject>
#include <QtGlobal>

class MapView;
class QTcpServer;
class QJsonObject;

namespace Noggit
{
  namespace Mcp
  {
    // Phase 0 MCP endpoint.
    //
    // A tiny line-delimited-JSON TCP server (localhost only) that lets an external agent drive
    // this MapView: run Lua against the live scripting engine, move the camera, query the scene,
    // save, undo/redo. It is OWNED BY the MapView and created on the GUI thread, so QTcpServer /
    // QTcpSocket signals are delivered inside the Qt event loop -- the same thread that owns the
    // World, the ActionManager and the GL context. That means every request is handled inline with
    // no cross-thread marshalling. (Trade-off: a slow request briefly blocks the UI; fine for now.)
    //
    // Protocol: one JSON object per line in, one JSON object per line out.
    //   in : {"cmd":"run_lua","code":"add_m2('World\\...\\tree.m2', vec(x,y,z), 1.0, vec(0,0,0))"}
    //   out: {"ok":true}                     or   {"ok":false,"error":"..."}
    //
    // Port: env NOGGIT_MCP_PORT, else 8172.
    class McpServer : public QObject
    {
      Q_OBJECT
    public:
      explicit McpServer(MapView* view, QObject* parent = nullptr);
      ~McpServer() override;

      bool isListening() const;
      quint16 port() const;

    private slots:
      void onNewConnection();
      void onReadyRead();

    private:
      QJsonObject dispatch(QJsonObject const& request);

      QJsonObject cmd_ping         (QJsonObject const& req);
      QJsonObject cmd_run_lua      (QJsonObject const& req);
      QJsonObject cmd_place_model  (QJsonObject const& req);
      QJsonObject cmd_focus_camera (QJsonObject const& req);
      QJsonObject cmd_query_objects(QJsonObject const& req);
      QJsonObject cmd_height_at    (QJsonObject const& req);
      QJsonObject cmd_save         (QJsonObject const& req);
      QJsonObject cmd_undo         (QJsonObject const& req);
      QJsonObject cmd_redo         (QJsonObject const& req);

      MapView* _view = nullptr;
      QTcpServer* _server = nullptr;
      quint16 _port = 0;
    };
  } // namespace Mcp
} // namespace Noggit
