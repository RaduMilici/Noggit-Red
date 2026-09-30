// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifdef USE_MYSQL_UID_STORAGE

#include <noggit/ui/content/ContentSession.hpp>

#include <noggit/ui/content/ClientData.hpp>

#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

namespace Noggit::Ui::Content
{
  namespace
  {
    std::vector<NamedEntry> convert(std::vector<mysql::content::NamedEntry> const& entries)
    {
      std::vector<NamedEntry> out;
      out.reserve(entries.size());
      for (auto const& entry : entries)
      {
        out.push_back({entry.entry, entry.name, entry.kind, entry.display});
      }
      return out;
    }

    std::vector<NamedEntry> questEntries(mysql::content::QuestList const& list)
    {
      std::vector<NamedEntry> out;
      for (auto const& quest : list.quests)
      {
        out.push_back({quest.entry, quest.title, quest.level, 0});
      }
      return out;
    }

    std::string plain(QString const& label)
    {
      // "Young Wolf (#299)" -> "Young Wolf"
      int const id = label.lastIndexOf(" (#");
      return (id > 0 ? label.left(id) : label).toStdString();
    }
  }

  std::unique_ptr<ContentSession> ContentSession::open(QWidget* parent)
  {
    std::string error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto db = mysql::content::Database::open(&error);
    std::unique_ptr<ContentSession> session;
    if (db)
    {
      session.reset(new ContentSession());
      session->_db = std::move(db);
      if (!mysql::content::questList(*session->_db, session->_quests, &error))
      {
        session.reset();
      }
    }
    if (!session)
    {
      QApplication::restoreOverrideCursor();
      QMessageBox::critical(parent, "World database", QString("Could not read the world database:\n%1")
                                                        .arg(QString::fromStdString(error)));
      return nullptr;
    }

    auto& database = *session->_db;
    LookupSource source;
    source.vanilla = ClientData::vanillaClient();
    source.creatures = convert(mysql::content::creatures(database));
    source.objects = convert(mysql::content::objects(database));
    source.items = convert(mysql::content::items(database));
    source.spells = convert(mysql::content::spells(database));
    source.quests = questEntries(session->_quests);
    source.zones = ClientData::zones();
    source.reputation_factions = ClientData::reputationFactions();
    source.faction_templates = ClientData::factionTemplates();
    source.item_icon = &ClientData::itemIcon;
    for (auto const& trigger : mysql::content::areaTriggers(database))
    {
      source.area_triggers.push_back({trigger.id, trigger.name, trigger.map, trigger.x, trigger.y, trigger.z,
                                      trigger.radius, trigger.quest, trigger.other_use});
    }
    session->_lookups = buildLookups(source);
    QApplication::restoreOverrideCursor();
    return session;
  }

  bool ContentSession::reloadQuests()
  {
    return mysql::content::questList(*_db, _quests);
  }

  Noggit::Quest::NameLookup ContentSession::names() const
  {
    Noggit::Quest::NameLookup names;
    auto* lookups = _lookups.get();
    names.npc = [lookups](std::uint32_t id) { return plain(lookups->creatureName(id)); };
    names.object = [lookups](std::uint32_t id) { return plain(lookups->objectName(id)); };
    names.item = [lookups](std::uint32_t id) { return plain(lookups->itemName(id)); };
    names.quest = [this](std::uint32_t id)
    {
      for (auto const& quest : _quests.quests)
      {
        if (quest.entry == id)
        {
          return quest.title;
        }
      }
      return std::string();
    };
    return names;
  }
}

#endif
