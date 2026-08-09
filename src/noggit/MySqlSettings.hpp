// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QVariant>

// Per-project MySQL connection settings.
//
// The MySQL settings (server/user/pwd/db/port/enabled) used to live under the single global QSettings
// group "project/mysql/*", so every project shared one connection -- switching between e.g. a 3.3.5a
// project and a Turtle project meant re-typing them each time. These helpers namespace the settings by
// the active project instead, so each project remembers its own.
//
// The active project is identified by "project/current_path" in QSettings, which ApplicationProject::
// loadProject records when a project is opened.
namespace Noggit
{
  // Stable id derived from the active project's path. FNV-1a over the lowercased UTF-8 path -- it MUST
  // be deterministic across runs (QSettings persists to disk), so we deliberately do NOT use qHash,
  // whose seed can be randomized per process. Empty when no project is active.
  inline QString projectKeyId()
  {
    QString const path = QSettings().value("project/current_path").toString().toLower();
    if (path.isEmpty())
    {
      return QString();
    }
    QByteArray const bytes = path.toUtf8();
    quint32 h = 2166136261u;
    for (char b : bytes)
    {
      h ^= static_cast<unsigned char>(b);
      h *= 16777619u;
    }
    return QString::number(h, 16);
  }

  // Key for a per-project MySQL setting (use when WRITING). Falls back to the legacy global key when no
  // project is active.
  inline QString mysqlSettingKey(QString const& suffix)
  {
    QString const id = projectKeyId();
    if (id.isEmpty())
    {
      return QStringLiteral("project/mysql/") + suffix;
    }
    return QStringLiteral("project/mysql_p/") + id + QStringLiteral("/") + suffix;
  }

  // Value of a per-project MySQL setting (use when READING). Migration: returns the per-project value
  // if it has been saved, else the legacy global value (so existing shared settings seed a project the
  // first time it's opened), else the hardcoded default.
  inline QVariant mysqlSetting(QString const& suffix, QVariant const& def)
  {
    QSettings settings;
    return settings.value(mysqlSettingKey(suffix),
                          settings.value(QStringLiteral("project/mysql/") + suffix, def));
  }
}
