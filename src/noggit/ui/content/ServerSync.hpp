// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Keeps a tortoise-deploy server in step with what Noggit writes to its database, through the project's
// SSH tunnel settings (Settings -> MySQL -> SSH tunnel). Only available in MySQL builds.
//
//  - Every database write (spawns, NPCs, quests, items) leaves a copy in <project>/sql_exports/. The whole
//    folder is mirrored to <deploy folder>/storage/database/custom-sql/noggit/ on the server, where
//    tortoise-deploy replays it on every database start -- so the changes survive the world database
//    being re-created during an update. Mirroring (instead of uploading single files) also removes the
//    copies of deleted NPCs/quests/items on the server.
//  - The world server only loads spawns at startup; restartWorldServer runs
//    `docker compose restart mangosd` in the deploy folder.

class QWidget;

namespace Noggit::Ui
{
  // After a database write: mirrors sql_exports/ shortly afterwards (batched), when the project enables
  // "copy SQL exports to the server". Silent on success (status bar), warns on failure.
  void scheduleServerExportSync();

  // Assist menu: mirror now and report the result, regardless of the automatic setting.
  void syncExportsToServerNow(QWidget* parent);

  // Assist menu: confirm, then restart the world server (disconnects everyone online).
  void restartWorldServer(QWidget* parent);
}
