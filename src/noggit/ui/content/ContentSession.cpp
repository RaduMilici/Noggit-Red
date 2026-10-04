// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/content/ContentSession.hpp>
#include <noggit/creator/ItemBrowser.hpp>
#include <noggit/creator/ContentEditors.hpp>
#include <noggit/creator/SpellService.hpp>

#include <noggit/ui/content/ClientData.hpp>

#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <stdexcept>

namespace Noggit::Ui::Content
{
  namespace
  {
    std::vector<NamedEntry> convert(std::vector<Creator::NamedEntry> const& entries)
    {
      std::vector<NamedEntry> out;
      out.reserve(entries.size());
      for (auto const& entry : entries)
      {
        out.push_back({entry.entry, entry.name, entry.kind, entry.display});
      }
      return out;
    }

    std::vector<NamedEntry> questEntries(Creator::QuestList const& list)
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
    std::unique_ptr<ContentSession> session(new ContentSession());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try
    {
      auto lists = Creator::loadContentLists(session->_layouts);
      session->_quests = std::move(lists.quests);
      session->_own_quests = std::move(lists.own_quests);
      session->_own_items = std::move(lists.own_items);

      LookupSource source;
      source.vanilla = ClientData::vanillaClient();
      source.creatures = convert(lists.creatures);
      source.objects = convert(lists.objects);
      source.items = convert(lists.items);
      source.spells = convert(lists.spells);
      source.quests = questEntries(session->_quests);
      source.zones = ClientData::zones();
      source.reputation_factions = ClientData::reputationFactions();
      source.faction_templates = ClientData::factionTemplates();
      source.item_icon = &ClientData::itemIcon;
      for (auto const& trigger : lists.area_triggers)
      {
        source.area_triggers.push_back({trigger.id, trigger.name, trigger.map, trigger.x, trigger.y, trigger.z,
                                        trigger.radius, trigger.quest, trigger.other_use});
      }
      session->_lookups = buildLookups(source);
      // Items and spells made from these browsers are added to the lists right away.
      auto* lookups = session->_lookups.get();
      lookups->items->browse = [lookups](QWidget* parent, std::uint32_t current) -> std::optional<std::uint32_t>
      {
        auto const chosen = Creator::pickItem(parent, "Choose item", {}, current);
        if (chosen)
        {
          try
          {
            if (auto const info = Creator::ItemService::get(*chosen))
            {
              lookups->addItem(*chosen, info->name, std::uint32_t(info->quality), info->display);
            }
          }
          catch (...)
          {
          }
        }
        return chosen;
      };
      lookups->spells->browse = [lookups](QWidget* parent, std::uint32_t current) -> std::optional<std::uint32_t>
      {
        auto const chosen = Creator::pickSpell(parent, "Choose spell", current);
        if (chosen)
        {
          try
          {
            for (auto const& found : Creator::SpellService::search(QString::number(*chosen), false, 5))
            {
              if (found.id == *chosen)
              {
                lookups->addSpell(*chosen, found.name);
              }
            }
          }
          catch (...)
          {
          }
        }
        return chosen;
      };
    }
    catch (std::exception const& e)
    {
      QApplication::restoreOverrideCursor();
      QMessageBox::warning(parent, "Quests", QString::fromUtf8(e.what()));
      return nullptr;
    }
    QApplication::restoreOverrideCursor();
    return session;
  }

  bool ContentSession::reloadQuests()
  {
    try
    {
      _quests = Creator::loadQuestList(_layouts);
      return true;
    }
    catch (std::exception const&)
    {
      return false;
    }
  }

  void ContentSession::setOwnQuest(std::uint32_t quest, bool own)
  {
    own ? void(_own_quests.insert(quest)) : void(_own_quests.erase(quest));
  }

  void ContentSession::setOwnItem(std::uint32_t item, bool own)
  {
    own ? void(_own_items.insert(item)) : void(_own_items.erase(item));
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
