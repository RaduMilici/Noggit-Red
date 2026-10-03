#include "ProductionSync.hpp"
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <memory>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
using Rollback = ProductionSync::Outcome::Rollback;
QString const createOwnership =
  "CREATE TABLE IF NOT EXISTS creator_content (kind VARCHAR(20) NOT NULL, entry INT UNSIGNED NOT NULL, PRIMARY KEY(kind,entry)) ENGINE=InnoDB";
QStringList const worldTables{"creature_template", "creature", "quest_template", "item_template"};
void writeFile(QString const& path, QByteArray const& bytes) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    throw std::runtime_error(("Cannot write " + path).toStdString());
}
QString title(EntityType type) {
  return type == EntityType::Npc ? "NPC" : type == EntityType::Spawn ? "Placement" : type == EntityType::Item ? "Item" : "Quest";
}
// The server's message, in terms of the profile.
QString databaseProblem(QString const& message, ProductionProfile const& profile) {
  if (message.contains("Access denied", Qt::CaseInsensitive))
    return "The production database rejected user \"" + profile.dbUser + "\". Check the database user name and password.";
  if (message.contains("Unknown database", Qt::CaseInsensitive))
    return "There is no database \"" + profile.database + "\" on production. For tortoise-deploy it is tw_world.";
  return message;
}
struct Connection {
  std::unique_ptr<Ssh::Tunnel> tunnel;
  std::unique_ptr<SqlConnection> db;
  QVector<Fields> query(QString const& sql) { return db->query(sql); }
};
Connection open(ProductionProfile const& profile, QString const& password) {
  Connection c;
  c.tunnel = std::make_unique<Ssh::Tunnel>(profile.ssh(), profile.dbHost, profile.dbPort);
  try { c.db = std::make_unique<SqlConnection>(profile.endpoint(c.tunnel->localPort(), password), "Production database"); }
  catch (std::exception const& e) {
    auto out = c.tunnel->output();
    if (Ssh::classify(out) == Ssh::Problem::Forward) throw Ssh::Error(Ssh::Problem::Forward, Ssh::explain(Ssh::Problem::Forward, profile.ssh()), out);
    throw std::runtime_error(databaseProblem(QString::fromUtf8(e.what()), profile).toStdString());
  }
  auto tables = c.query("SELECT COUNT(*) AS n FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('"
                        + worldTables.join("','") + "')");
  if (tables.isEmpty() || tables[0]["n"].toInt() != worldTables.size())
    throw std::runtime_error(("\"" + profile.database + "\" on production is not a Tortoise world database.").toStdString());
  return c;
}
QString latestMigration(Connection& c) {
  try {
    auto rows = c.query("SELECT Name FROM migrations ORDER BY Id DESC LIMIT 1");
    return rows.isEmpty() ? QString() : rows[0]["Name"].toString();
  } catch (std::exception const&) { return {}; }
}
}
ProductionBackup ProductionBackup::take(QVector<QPair<QString, QString>> const& scopes, QueryFunction const& production) {
  ProductionBackup backup; backup.scopes = scopes;
  for (auto const& [table, where] : scopes) {
    QJsonArray rows;
    for (auto const& row : production("SELECT * FROM " + table + " WHERE " + where)) rows.append(QJsonObject::fromVariantMap(row));
    backup.rows.push_back(rows);
  }
  return backup;
}
int ProductionBackup::rowCount() const { int count = 0; for (auto const& r : rows) count += r.size(); return count; }
QJsonObject ProductionBackup::toJson() const {
  QJsonArray list;
  for (int i = 0; i < scopes.size(); ++i) list.append(QJsonObject{{"table", scopes[i].first}, {"where", scopes[i].second}, {"rows", rows[i]}});
  return {{"format", 1}, {"taken", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"scopes", list}};
}
QStringList ProductionBackup::restoreStatements() const {
  QStringList statements;
  for (int i = 0; i < scopes.size(); ++i) {
    statements << "DELETE FROM " + scopes[i].first + " WHERE " + scopes[i].second + ";";
    statements << ChangeTracker::statements(ChangeTracker::upsert(scopes[i].first, rows[i]));
  }
  return statements;
}
QStringList ProductionSync::conflicts(ChangePackage const& package, QueryFunction const& production) {
  QStringList clashes;
  for (int i = 0; i < package.changes.size(); ++i) {
    auto const& c = package.changes[i];
    if (!package.owned.value(i) || c.action == ChangeAction::Delete) continue;
    QString existing;
    if (ChangeTracker::capture(production, c.type, c.entity, &existing).isEmpty()) continue;
    if (!production("SELECT entry FROM creator_content WHERE kind='" + toString(c.type) + "' AND entry=" + QString::number(c.entity)).isEmpty()) continue;
    clashes << QString("%1 #%2 \"%3\": production already has \"%4\"").arg(title(c.type)).arg(c.entity).arg(c.label, existing);
  }
  return clashes;
}
ProductionSync::Check ProductionSync::test(ProductionProfile const& profile, QString const& password, QJsonObject const& localSource) {
  if (auto problem = profile.validate(); !problem.isEmpty()) throw std::runtime_error(problem.toStdString());
  auto c = open(profile, password);
  Check check;
  check.lines << "SSH: logged in as " + profile.sshUser + "@" + profile.sshHost + ":" + QString::number(profile.sshPort);
  auto version = c.query("SELECT VERSION() AS v, CURRENT_USER() AS u");
  check.lines << "Database: " + profile.database + " (" + version[0]["v"].toString() + "), user " + version[0]["u"].toString();
  auto count = [&](QString const& sql) { auto rows = c.query(sql); return rows.isEmpty() ? 0 : rows[0]["n"].toInt(); };
  check.lines << QString("World: %1 NPCs, %2 quests").arg(count("SELECT COUNT(*) AS n FROM creature_template")).arg(count("SELECT COUNT(*) AS n FROM quest_template"));
  bool ownership = count("SELECT COUNT(*) AS n FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='creator_content'") > 0;
  check.lines << (ownership ? QString("Synced from Noggit so far: %1 entries").arg(count("SELECT COUNT(*) AS n FROM creator_content"))
                            : QString("Nothing synced from Noggit yet"));
  if (auto remote = latestMigration(c), local = localSource["contentVersion"].toObject()["migration"].toString(); !remote.isEmpty()) {
    check.lines << "Content version: " + remote;
    if (!local.isEmpty() && local != remote)
      check.warnings << "Production's content version differs from your local world (" + local + "). Synced content may refer to "
                        "game data one of them does not have.";
  }
  // Least privilege: changes to the world database only.
  QString grants;
  for (auto const& row : c.query("SHOW GRANTS")) for (auto const& value : row) grants += value.toString().remove('\\') + '\n';
  if (version[0]["u"].toString().startsWith("root@") || grants.contains("ALL PRIVILEGES ON *.*") || grants.contains("WITH GRANT OPTION"))
    check.warnings << "This database user has broad rights. Prefer a dedicated user limited to the world database (see the Creator README).";
  bool scoped = grants.contains("ALL PRIVILEGES");
  if (!scoped) {
    QStringList missing;
    for (auto right : {"SELECT", "INSERT", "UPDATE", "DELETE"}) if (!grants.contains(right)) missing << right;
    if (!ownership && !grants.contains("CREATE")) missing << "CREATE";
    if (!missing.isEmpty()) check.warnings << "The database user may lack these rights on " + profile.database + ": " + missing.join(", ") + ".";
  }
  check.lines << (profile.restartCommand.isEmpty() ? QString("No restart command: restart the world server yourself after a sync")
                                                   : QString("Restart command set (not run by this test)"));
  return check;
}
ProductionSync::Outcome ProductionSync::run(ProductionProfile const& profile, QString const& password, ChangePackage const& package,
                                            QString const& folder, Progress const& progress) {
  Outcome out;
  QFile logFile(folder + "/sync.log"); logFile.open(QIODevice::Append | QIODevice::Text);
  auto log = [&](QString const& line) {
    logFile.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + line + "\n").toUtf8()); logFile.flush();
  };
  auto finish = [&](QString const& status) {
    try {
      writeFile(folder + "/result.json", QJsonDocument(QJsonObject{
        {"status", status}, {"error", out.error}, {"details", out.details}, {"finished", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"rollback", out.rollback == Rollback::Restored ? "restored" : out.rollback == Rollback::Failed ? "failed" : "not needed"}}).toJson());
    } catch (std::exception const& e) { log(QString::fromUtf8(e.what())); }
    log("Result: " + status);
    return out;
  };
  auto fail = [&](Step step, QString const& error, QString const& details) {
    out.error = error; out.details = details;
    log("Failed: " + error + (details.isEmpty() ? QString() : "\n" + details));
    progress(step, State::Failed, error);
    return finish(out.rollback == Rollback::Failed ? "rollback-failed" : out.rollback == Rollback::Restored ? "rolled-back" : "failed");
  };
  log("Sync to production: " + profile.describe());

  progress(Step::Connect, State::Running, "Connecting to " + profile.sshHost + "…");
  Connection c;
  try {
    if (auto problem = profile.validate(); !problem.isEmpty()) throw std::runtime_error(problem.toStdString());
    c = open(profile, password);
    auto locked = c.query("SELECT GET_LOCK('noggit_creator_sync',0) AS locked");
    if (locked.isEmpty() || locked[0]["locked"].toInt() != 1) throw std::runtime_error("Another Sync to Production is running on this server. Try again when it has finished.");
  } catch (Ssh::Error const& e) { return fail(Step::Connect, QString::fromUtf8(e.what()), e.details); }
  catch (std::exception const& e) { return fail(Step::Connect, QString::fromUtf8(e.what()), c.tunnel ? c.tunnel->output() : QString()); }
  progress(Step::Connect, State::Done, profile.sshUser + "@" + profile.sshHost);

  progress(Step::Backup, State::Running, "Saving the production rows these changes touch…");
  ProductionBackup backup;
  try {
    auto query = [&c](QString const& sql) { return c.query(sql); };
    c.db->execute(createOwnership.toStdString());
    if (auto clashes = conflicts(package, query); !clashes.isEmpty())
      return fail(Step::Backup, "Production already uses these IDs for content that was not synced from Noggit, so nothing was changed:\n"
                                + clashes.join('\n'), {});
    backup = ProductionBackup::take(ChangeTracker::footprint(package.changes, query), query);
    writeFile(folder + "/backup.json", QJsonDocument(backup.toJson()).toJson());
    writeFile(folder + "/restore.sql", ("-- Puts back the production rows saved before this sync.\nSET NAMES utf8mb4;\n"
                                        + backup.restoreStatements().join('\n') + "\n").toUtf8());
  } catch (std::exception const& e) { return fail(Step::Backup, "Could not back up production: " + QString::fromUtf8(e.what()), {}); }
  log(QString("Backup: %1 rows in %2 scopes").arg(backup.rowCount()).arg(backup.scopes.size()));
  progress(Step::Backup, State::Done, QString("%1 production rows saved").arg(backup.rowCount()));

  // Restores the backup; a broken connection gets one fresh one.
  auto rollback = [&] {
    auto restore = [&](SqlConnection& db) { for (auto const& s : backup.restoreStatements()) db.execute(s.toStdString()); };
    try { c.db->execute("ROLLBACK"); } catch (std::exception const&) { /* MyISAM rows are restored below */ }
    try { restore(*c.db); out.rollback = Rollback::Restored; }
    catch (std::exception const& first) {
      log("Restore failed: " + QString::fromUtf8(first.what()) + "; retrying on a new connection");
      try { auto fresh = open(profile, password); restore(*fresh.db); out.rollback = Rollback::Restored; }
      catch (std::exception const& second) { out.rollback = Rollback::Failed; log("Restore failed: " + QString::fromUtf8(second.what())); }
    }
    log(out.rollback == Rollback::Restored ? "Production rows restored from the backup" : "Production could NOT be restored");
  };

  auto statements = ChangeTracker::statements(package.sql);
  progress(Step::Apply, State::Running, QString("Applying %1 statements…").arg(statements.size()));
  int applied = 0;
  try {
    // World tables are MyISAM, which ignores transactions; the backup is what undoes them.
    c.db->execute("START TRANSACTION");
    for (auto const& statement : statements) { c.db->execute(statement.toStdString()); ++applied; }
    c.db->execute("COMMIT");
  } catch (std::exception const& e) {
    QString failed = applied < statements.size() ? statements[applied].left(300) : QString("COMMIT");
    log(QString("Statement %1 of %2 failed: %3").arg(applied + 1).arg(statements.size()).arg(failed));
    rollback();
    return fail(Step::Apply, QString::fromUtf8(e.what()), "Failed statement:\n" + failed);
  }
  log(QString("Applied %1 statements").arg(applied));
  progress(Step::Apply, State::Done, QString("%1 statements").arg(applied));

  if (profile.restartCommand.trimmed().isEmpty()) {
    progress(Step::Restart, State::Skipped, "No restart command set: restart the world server to load the changes.");
  } else {
    progress(Step::Restart, State::Running, "Restarting the world server…");
    try {
      auto result = Ssh::run(profile.ssh(), profile.restartCommand, 10 * 60 * 1000);
      log("Restart command output:\n" + result.output.trimmed());
      if (result.exitCode != 0)
        throw Ssh::Error(Ssh::Problem::Failed, QString("The restart command failed (exit code %1).").arg(result.exitCode), result.output);
    } catch (Ssh::Error const& e) {
      rollback();
      return fail(Step::Restart, QString::fromUtf8(e.what()), e.details);
    }
    progress(Step::Restart, State::Done, "World server restarted");
  }
  out.ok = true;
  return finish("synced");
}
}
