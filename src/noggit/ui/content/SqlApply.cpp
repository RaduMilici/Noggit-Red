// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/content/SqlApply.hpp>

#include <mysql/content_db.h>

#include <noggit/project/CurrentProject.hpp>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtWidgets/QMessageBox>

namespace Noggit::Ui
{
  bool confirmSqlApply(QWidget* parent, QString const& title, QString const& summary, QString const& sql)
  {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setIcon(QMessageBox::Warning);
    box.setText(summary + "\n\nTarget database: "
                + QString::fromStdString(mysql::connectionDescription()));
    QString preview = sql;
    if (preview.size() > 4000)
    {
      preview = preview.left(4000) + "\n[... truncated ...]";
    }
    box.setDetailedText(preview);
    box.setStandardButtons(QMessageBox::Apply | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Apply;
  }

  void reportSqlResult(QWidget* parent, mysql::SqlScriptResult const& result, QString const& what)
  {
    if (result.ok)
    {
      QMessageBox::information(parent, "SQL applied",
        QString("%1: %2 statement(s) applied to %3.")
          .arg(what)
          .arg(result.statements_executed)
          .arg(QString::fromStdString(mysql::connectionDescription())));
    }
    else
    {
      QMessageBox::critical(parent, "SQL apply failed",
        QString("%1 failed and was rolled back.\n\nError: %2%3")
          .arg(what)
          .arg(QString::fromStdString(result.error))
          .arg(result.failed_statement.empty()
                 ? QString()
                 : QString("\n\nFailed statement:\n%1").arg(QString::fromStdString(result.failed_statement))));
    }
  }

  QString writeSqlExport(QString const& relative_path, std::string const& sql, std::string const& error)
  {
    auto const* project = Noggit::Project::CurrentProject::get();
    if (sql.empty() || !project)
    {
      return QString("Could not save a copy to sql_exports/: %1")
               .arg(error.empty() ? QString("no project") : QString::fromStdString(error));
    }
    QDir project_dir(QString::fromStdString(project->ProjectPath));
    QString const path = project_dir.filePath("sql_exports/" + relative_path);
    QFile file(path);
    if (!project_dir.mkpath(QFileInfo(path).absolutePath())
        || !file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
      return QString("Could not save a copy to %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
    }
    file.write(QByteArray::fromStdString(sql));
    return QString("A copy of its definition was saved to\n%1").arg(QDir::toNativeSeparators(path));
  }

  void removeSqlExport(QString const& relative_path)
  {
    if (auto const* project = Noggit::Project::CurrentProject::get())
    {
      QFile::remove(QDir(QString::fromStdString(project->ProjectPath)).filePath("sql_exports/" + relative_path));
    }
  }

  bool applyPlan(QWidget* parent, mysql::content::Database& db, QString const& title, QString const& question,
                 QStringList const& consequences, std::vector<std::string> const& statements,
                 std::vector<std::string> const& cleanup, bool require_first_row)
  {
    QString summary = question;
    for (auto const& line : consequences)
    {
      summary += "\n\n\u2022 " + line;
    }
    QString sql;
    for (auto const& statement : statements)
    {
      sql += QString::fromStdString(statement) + ";\n\n";
    }
    if (!confirmSqlApply(parent, title, summary, sql))
    {
      return false;
    }
    auto const result = db.execute(statements, cleanup, require_first_row);
    if (!result.ok)
    {
      reportSqlResult(parent, result, title);
    }
    return result.ok;
  }

  void showProblems(QWidget* parent, QString const& title, QStringList const& problems)
  {
    QMessageBox::warning(parent, title, "This cannot be saved yet:\n\n\u2022 " + problems.join("\n\u2022 "));
  }

  QStringList toQStringList(std::vector<std::string> const& lines)
  {
    QStringList out;
    for (auto const& line : lines)
    {
      out << QString::fromStdString(line);
    }
    return out;
  }
}

#endif
