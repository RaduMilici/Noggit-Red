// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

// Shared UI for everything that writes to the world database (spawn apply, NPC and quest editors).
// Only available in MySQL builds (USE_MYSQL_UID_STORAGE).

#include <mysql/mysql.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <string>
#include <vector>

class QWidget;

namespace mysql::content
{
  class Database;
}

namespace Noggit::Ui
{
  // Confirmation showing the exact target connection, with the SQL about to run as the detail text.
  bool confirmSqlApply(QWidget* parent, QString const& title, QString const& summary, QString const& sql);

  void reportSqlResult(QWidget* parent, mysql::SqlScriptResult const& result, QString const& what);

  // Writes `sql` to <project>/sql_exports/<relative_path>. Returns a line for the result message: where it
  // was saved, or why it could not be.
  QString writeSqlExport(QString const& relative_path, std::string const& sql, std::string const& error);

  // Removes <project>/sql_exports/<relative_path> (a deleted NPC / quest / item).
  void removeSqlExport(QString const& relative_path);

  // Asks for confirmation (the question, the consequences as a list, the SQL as details), runs the statements
  // on `db`, and reports a failure. True when they were written.
  bool applyPlan(QWidget* parent, mysql::content::Database& db, QString const& title, QString const& question,
                 QStringList const& consequences, std::vector<std::string> const& statements,
                 std::vector<std::string> const& cleanup = {}, bool require_first_row = false);

  // A refused plan's reasons, shown to the user.
  void showProblems(QWidget* parent, QString const& title, QStringList const& problems);

  QStringList toQStringList(std::vector<std::string> const& lines);
}
